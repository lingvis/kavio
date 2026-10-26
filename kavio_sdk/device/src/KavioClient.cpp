/*
 * KavioClient.cpp
 *
 * Device Component SDK client implementation for Platform IPC sessions, automatic reconnect,
 * private App Channel state and raw channel datagrams. Payload content is not logged.
 */

#include <kavio/KavioClient.h>

#include "internal/FrameShm.h"
#include "internal/EncodedStreamShm.h"
#include "internal/IPCSocket.h"
#include "internal/AppConfigManager.h"
#include <kavio/KavioSdkVersion.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <unordered_set>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace kavio {
namespace {

using ProtoHeader = kavio::ipc::proto::IPCHeaderPb;
using ProtoMessageType = kavio::ipc::proto::MessageTypePb;
using ProtoPackage = kavio::ipc::proto::IPCPackagePb;
using ProtoPixelFormat = kavio::ipc::proto::PixelFormatPb;

std::mutex g_sdk_log_mutex;

template <typename... Args>
void SdkLog(const char* level, Args&&... args)
{
    std::lock_guard<std::mutex> lock(g_sdk_log_mutex);
    std::cerr << "[KavioSDK][" << level << "]";
    using Expander = int[];
    (void)Expander{0, ((std::cerr << " " << std::forward<Args>(args)), 0)...};
    std::cerr << std::endl;
}

static const size_t kCommChannelMaxPayloadSize = 1024 * 1024;
static const size_t kAppConfigMaxPayloadSize = 1024 * 1024;

std::string ResolveClientName(const kavio::ipc::proto::A2PReqCreateSessionPb& options) {
    char program_path[PATH_MAX + 1] = {};
    const ssize_t length = ::readlink("/proc/self/exe", program_path, PATH_MAX);
    if (length > 0) {
        program_path[length] = '\0';
        const char* file_name = std::strrchr(program_path, '/');
        file_name = file_name ? file_name + 1 : program_path;
        if (*file_name != '\0') {
            return file_name;
        }
    }
    return !options.client_name().empty() ? options.client_name() : "kavio_sdk_client";
}

int32_t ResolveClientPid(const kavio::ipc::proto::A2PReqCreateSessionPb& options) {
    return options.client_pid() != 0 ? options.client_pid() : static_cast<int32_t>(::getpid());
}

PixelFormat FromProto(ProtoPixelFormat value) {
    switch (value) {
    case kavio::ipc::proto::PIXEL_FORMAT_UYVY:
        return PixelFormat::kUYVY;
    case kavio::ipc::proto::PIXEL_FORMAT_YUYV:
        return PixelFormat::kYUYV;
    case kavio::ipc::proto::PIXEL_FORMAT_BGRA8888:
        return PixelFormat::kBGRA8888;
    default:
        return PixelFormat::kUnknown;
    }
}

PixelFormat FromRuntimeProto(kavio::runtime::proto::RuntimePixelFormatPb value) {
    using namespace kavio::runtime::proto;
    switch (value) {
    case RUNTIME_PIXEL_FORMAT_UYVY:
        return PixelFormat::kUYVY;
    case RUNTIME_PIXEL_FORMAT_YUYV:
        return PixelFormat::kYUYV;
    case RUNTIME_PIXEL_FORMAT_BGRA8888:
        return PixelFormat::kBGRA8888;
    default:
        return PixelFormat::kUnknown;
    }
}

bool RuntimeRxConnected(const kavio::runtime::proto::RuntimeDataPb& runtime_data) {
    if (!runtime_data.has_rx_info()) {
        return false;
    }
    const auto& rx = runtime_data.rx_info();
    using namespace kavio::runtime::proto;
    return rx.status() == RUNTIME_VIDEO_STATUS_CONNECTED ||
           rx.status() == RUNTIME_VIDEO_STATUS_RUNNING;
}

ProtoPackage MakePackage(ProtoMessageType type, uint64_t track_id) {
    ProtoPackage package;
    ProtoHeader* header = package.mutable_header();
    header->set_msg_type(type);
    header->set_track_id(track_id);
    return package;
}

enum class ClientState {
    kInvalid = 0,
    kReady,
};

struct SurfaceOwner {
    virtual ~SurfaceOwner() = default;
    virtual bool IsTxSurfaceCurrent(uint32_t shm_id,
                                    const std::shared_ptr<FrameShm>& shm) const = 0;
    virtual Status DestroyTxSurface(uint32_t shm_id,
                                    const std::shared_ptr<FrameShm>& shm,
                                    uint64_t& request_id) = 0;
};

}  // namespace

struct TxSurface::Impl {
    kavio::ipc::proto::ShmInfoPb info;
    std::shared_ptr<FrameShm> shm;
    std::weak_ptr<SurfaceOwner> owner;
};

struct KavioClient::Impl : public std::enable_shared_from_this<KavioClient::Impl>,
                           public SurfaceOwner {
    kavio::ipc::proto::A2PReqCreateSessionPb session_options;
    bool auto_query_runtime_on_connect = true;
    IClientMessageHandler* handler = nullptr;
    AppConfigManager app_config;

    mutable std::mutex lifecycle_mutex;
    mutable std::mutex session_mutex;
    mutable std::mutex socket_mutex;
    mutable std::mutex pending_mutex;
    std::unordered_map<uint64_t, uint32_t> pending_tx_destroy;
    std::unordered_map<uint64_t, uint32_t> pending_h264_unsubscribe;
    std::unordered_map<uint64_t, kavio::ipc::proto::RxParamsPb> pending_rx_params;
    std::unordered_map<uint64_t, kavio::ipc::proto::TxParamsPb> pending_tx_params;

    struct QueuedMessage {
        ProtoPackage packet;
        bool disconnected = false;
        size_t bytes = 0;
    };
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<QueuedMessage> messages;
    size_t queue_bytes = 0;
    bool dispatch_stop = false;
    std::thread dispatch_thread;
    static constexpr size_t kMaxQueuedMessages = 4096;
    static constexpr size_t kMaxQueueBytes = 16 * 1024 * 1024;
    std::condition_variable connection_cv;

    std::unique_ptr<IPCSocket> socket;
    std::thread recv_thread;
    std::thread run_thread;

    std::atomic<bool> connected{false};
    std::atomic<bool> transport_connected{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> worker_running{false};
    std::atomic<uint64_t> next_track_id{1};

    std::atomic<uint64_t> session_create_track_id{0};

    // 仅保存 SDK 本地操作需要的最小会话状态；Session 的完整回复通过 protobuf OnMessage 交给 App。
    uint32_t session_id = 0;
    uint32_t rx_shm_id = 0;
    RuntimeData runtime_data;
    bool runtime_synced_once = false;
    kavio::ipc::proto::RxParamsPb rx_params;
    kavio::ipc::proto::TxParamsPb tx_params;
    bool rx_frame_ready = false;
    uint64_t rx_generation = 0;
    ClientState client_state = ClientState::kInvalid;
    bool usb_host_online = false;
    uint32_t comm_channel_id = 0;
    bool comm_peer_online = false;
    std::unique_ptr<FrameShm> rx_shm;
    std::unordered_map<uint32_t, std::shared_ptr<FrameShm>> tx_shms;

    std::shared_ptr<EncodedStreamShm> h264_shm;
    uint32_t h264_stream_id = 0;

    uint64_t AllocTrackId() {
        return next_track_id.fetch_add(1, std::memory_order_relaxed);
    }

    bool Enqueue(const ProtoPackage& packet) {
        const size_t bytes = packet.ByteSizeLong();
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lk(queue_mutex);
            if (dispatch_stop) return false;
            if (messages.size() >= kMaxQueuedMessages ||
                bytes > kMaxQueueBytes - std::min(queue_bytes, kMaxQueueBytes)) {
                overflow = true;
            } else {
                messages.push_back({packet, false, bytes});
                queue_bytes += bytes;
            }
        }
        if (!overflow) { queue_cv.notify_one(); return true; }
        SdkLog("ERROR", "message queue overflow", "bytes=", bytes);
        // Data-bearing protobuf messages must never be silently dropped.
        // Disconnect; the App sees OnDisconnected and may retry on reconnect.
        std::lock_guard<std::mutex> lk(socket_mutex);
        if (socket) socket->Disconnect();
        return false;
    }

    void EnqueueDisconnected() {
        {
            std::lock_guard<std::mutex> lk(queue_mutex);
            if (dispatch_stop) return;
            messages.clear();
            queue_bytes = 0;
            messages.push_back({ProtoPackage{}, true, 0});
        }
        queue_cv.notify_one();
    }

    void DispatchLoop() {
        while (true) {
            QueuedMessage item;
            {
                std::unique_lock<std::mutex> lk(queue_mutex);
                queue_cv.wait(lk, [&] { return dispatch_stop || !messages.empty(); });
                if (dispatch_stop && messages.empty()) break;
                item = std::move(messages.front());
                messages.pop_front();
                queue_bytes -= item.bytes;
            }
            if (!handler) continue;
            try {
                if (item.disconnected) handler->OnDisconnected();
                else handler->OnMessage(item.packet);
            } catch (const std::exception& e) {
                SdkLog("ERROR", "App OnMessage exception:", e.what());
            } catch (...) {
                SdkLog("ERROR", "App OnMessage unknown exception");
            }
        }
    }

    bool IsSessionReadyLocked() const {
        return connected.load(std::memory_order_acquire) && session_id != 0;
    }

    bool IsSessionReady() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return IsSessionReadyLocked();
    }

    void ResetSessionDataLocked() {
        session_id = 0;
        rx_shm_id = 0;
        runtime_data.Clear();
        runtime_synced_once = false;
        rx_frame_ready = false;
        rx_generation = 0;
        usb_host_online = false;
        comm_channel_id = 0;
        comm_peer_online = false;
        rx_shm.reset();
        tx_shms.clear();
        h264_shm.reset();
        h264_stream_id = 0;
    }

    RuntimeData SnapshotRuntimeData() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return runtime_data;
    }


    bool SnapshotClientReady() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return client_state == ClientState::kReady;
    }

    bool SnapshotUSBHostOnline() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return usb_host_online;
    }

    bool SnapshotCommChannelReady() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return usb_host_online && comm_channel_id != 0 && comm_peer_online;
    }

    RxInfo BuildRxInfoLocked() const {
        RxInfo out;

        if (runtime_data.has_rx_info()) {
            const auto& rx = runtime_data.rx_info();
            out.connected = RuntimeRxConnected(runtime_data);
            out.width = rx.width();
            out.height = rx.height();
            // RuntimeVideoChannelStatePb 不包含 stride。
            out.stride = 0;
            out.pixel_format = FromRuntimeProto(rx.pixel_format());
        }

        if (rx_frame_ready && rx_shm && rx_shm->IsValid() && rx_shm_id != 0) {
            out.frame_available = true;
        }

        return out;
    }

    RxInfo SnapshotRxInfo() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return BuildRxInfoLocked();
    }

    bool SetClientStateLocked(ClientState next_state) {
        if (client_state == next_state) return false;
        client_state = next_state;
        return true;
    }

    Status SendPacket(const ProtoPackage& packet) {
        std::lock_guard<std::mutex> lk(socket_mutex);
        if (!socket || !socket->IsConnected()) {
            return Status::Error(StatusCode::Common_NotConnected, "socket is not connected");
        }
        return socket->Send(packet);
    }

    bool IsTxSurfaceCurrent(uint32_t shm_id,
                            const std::shared_ptr<FrameShm>& shm) const override {
        std::lock_guard<std::mutex> lk(session_mutex);
        if (!IsSessionReadyLocked() || client_state != ClientState::kReady) {
            return false;
        }
        const auto it = tx_shms.find(shm_id);
        return it != tx_shms.end() && it->second == shm;
    }

    Status DestroyTxSurface(uint32_t shm_id,
                            const std::shared_ptr<FrameShm>& shm,
                            uint64_t& request_id) override {
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (!IsSessionReadyLocked())
                return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
            auto it = tx_shms.find(shm_id);
            if (it == tx_shms.end() || it->second != shm)
                return Status::Error(StatusCode::Common_InvalidArgument, "surface not owned by client");
        }
        const auto id = AllocTrackId();
        auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_DESTROY_TX_SHM, id);
        req.mutable_payload()->mutable_a2p_req_destroy_tx_shm()->set_shm_id(shm_id);
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            pending_tx_destroy[id] = shm_id;
        }
        auto st = SendPacket(req);
        if (!st.ok()) {
            std::lock_guard<std::mutex> lk(pending_mutex);
            pending_tx_destroy.erase(id);
            return st;
        }
        request_id = id;
        return Status::Ok();
    }

    void ProcessAsyncStatus(const ProtoPackage& packet) {
        const uint64_t id = packet.header().msg_type() == kavio::ipc::proto::MSG_P2A_RSP_ERROR &&
                            packet.payload().has_p2a_rsp_error() && packet.payload().p2a_rsp_error().track_id()
                            ? packet.payload().p2a_rsp_error().track_id() : packet.header().track_id();
        const bool ok = packet.header().msg_type() == kavio::ipc::proto::MSG_P2A_RSP_STATUS &&
                        packet.payload().has_p2a_rsp_status() &&
                        packet.payload().p2a_rsp_status().code() == kavio::ipc::proto::STATUS_OK;
        std::lock_guard<std::mutex> lk(pending_mutex);
        auto tx = pending_tx_destroy.find(id);
        if (tx != pending_tx_destroy.end()) {
            if (ok) {
                std::lock_guard<std::mutex> slk(session_mutex);
                tx_shms.erase(tx->second);
            }
            pending_tx_destroy.erase(tx);
        }
        auto hs = pending_h264_unsubscribe.find(id);
        if (hs != pending_h264_unsubscribe.end()) {
            if (ok) {
                std::lock_guard<std::mutex> slk(session_mutex);
                if (h264_stream_id == hs->second) {
                    h264_shm.reset(); h264_stream_id = 0;
                }
            }
            pending_h264_unsubscribe.erase(hs);
        }
        auto rx = pending_rx_params.find(id);
        if (rx != pending_rx_params.end()) {
            if (ok) {
                std::lock_guard<std::mutex> slk(session_mutex);
                rx_params = rx->second;
                *session_options.mutable_rx_params() = rx_params;
                if (!rx_params.enabled()) {
                    rx_shm.reset(); rx_shm_id = 0; rx_frame_ready = false;
                }
            }
            pending_rx_params.erase(rx);
        }
        auto txp = pending_tx_params.find(id);
        if (txp != pending_tx_params.end()) {
            if (ok) {
                std::lock_guard<std::mutex> slk(session_mutex);
                tx_params = txp->second;
                *session_options.mutable_tx_params() = tx_params;
            }
            pending_tx_params.erase(txp);
        }
    }

    void MergeRuntimeDataLocked(const RuntimeData& in) {
        if (in.has_rx_info()) {
            runtime_data.mutable_rx_info()->CopyFrom(in.rx_info());
        }
        if (in.has_tx_info()) {
            runtime_data.mutable_tx_info()->CopyFrom(in.tx_info());
        }
        if (in.has_input()) {
            runtime_data.mutable_input()->CopyFrom(in.input());
        }
        runtime_synced_once = true;
    }

    void HandleTransportDisconnected(const Status& status) {
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            transport_connected.store(false, std::memory_order_release);
            connected.store(false, std::memory_order_release);
            ResetSessionDataLocked();
            SetClientStateLocked(ClientState::kInvalid);
        }

        { std::lock_guard<std::mutex> lk(pending_mutex);
          pending_tx_destroy.clear(); pending_h264_unsubscribe.clear();
          pending_rx_params.clear(); pending_tx_params.clear(); }
        EnqueueDisconnected();
        connection_cv.notify_all();
        (void)status;
    }

    void HandleSessionCreated(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_rsp_create_session()) {
            return;
        }

        const auto& body = packet.payload().p2a_rsp_create_session();
        if (!body.has_rx_params() || !body.has_tx_params()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lk(session_mutex);
            session_id = body.session_id();
            rx_params = body.rx_params();
            tx_params = body.tx_params();
            *session_options.mutable_rx_params() = rx_params;
            *session_options.mutable_tx_params() = tx_params;
            comm_channel_id = body.comm_channel_id();
            comm_peer_online = body.comm_peer_online();
            connected.store(true, std::memory_order_release);
            SetClientStateLocked(ClientState::kReady);
        }


        connection_cv.notify_all();

        if (auto_query_runtime_on_connect) {
            const uint64_t track_id = AllocTrackId();
            auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_QUERY_RUNTIME_DATA, track_id);
            req.mutable_payload()->mutable_a2p_req_query_runtime_data();
            (void)SendPacket(req);
        }

    }

    void HandleError(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_rsp_error()) return;
        const auto id = packet.payload().p2a_rsp_error().track_id() ?
            packet.payload().p2a_rsp_error().track_id() : packet.header().track_id();
        if (id == session_create_track_id.load(std::memory_order_acquire)) {
            bool session_not_established = false;
            {
                std::lock_guard<std::mutex> lk(session_mutex);
                session_not_established = session_id == 0;
                if (session_not_established) {
                    connected.store(false, std::memory_order_release);
                    SetClientStateLocked(ClientState::kInvalid);
                }
            }
            if (session_not_established) {
                // Preserve the existing initial-session failure behavior:
                // stop retrying after the server explicitly refuses Session creation.
                EnqueueDisconnected();
                stop_requested.store(true, std::memory_order_release);
                { std::lock_guard<std::mutex> lk(socket_mutex);
                  if (socket) socket->Disconnect(); }
                connection_cv.notify_all();
                return;
            }
        }
        ProcessAsyncStatus(packet);
    }

    void HandleStatusReply(const ProtoPackage& packet) {
        if (packet.payload().has_p2a_rsp_status()) ProcessAsyncStatus(packet);
    }

    void HandleRuntimeDataPacket(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_rsp_runtime_data() &&
            !packet.payload().has_p2a_ntf_runtime_data()) {
            return;
        }

        RuntimeData runtime_snapshot;
        RxInfo prev_rx;
        RxInfo next_rx;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            if (packet.header().msg_type() == kavio::ipc::proto::MSG_P2A_NTF_RUNTIME_DATA)
                MergeRuntimeDataLocked(packet.payload().p2a_ntf_runtime_data());
            else
                MergeRuntimeDataLocked(packet.payload().p2a_rsp_runtime_data());
            runtime_snapshot = runtime_data;
            next_rx = BuildRxInfoLocked();
        }


    }


    void HandleRxShmReady(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_ntf_rx_shm_ready()) {
            return;
        }

        const auto& body = packet.payload().p2a_ntf_rx_shm_ready();
        std::string error;
        FrameShm::OpenConfig open_config;
        open_config.name = body.rx_shm().name();
        std::unique_ptr<FrameShm> opened = FrameShm::Open(open_config, &error);

        RxInfo prev_rx;
        RxInfo next_rx;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            rx_shm.reset();
            rx_shm_id = body.rx_shm().id();

            if (!opened || !opened->IsValid()) {
                rx_shm_id = 0;
                rx_frame_ready = false;
            } else {
                rx_shm = std::move(opened);
                rx_generation = body.generation();
                rx_frame_ready = true;
            }
            next_rx = BuildRxInfoLocked();
        }

        (void)error;
    }

    void HandleRxShmInvalidated(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_ntf_rx_shm_invalidated()) {
            return;
        }

        RxInfo prev_rx;
        RxInfo next_rx;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            const auto& body = packet.payload().p2a_ntf_rx_shm_invalidated();
            if (rx_shm_id == body.shm_id()) {
                rx_shm.reset();
                rx_shm_id = 0;
                rx_frame_ready = false;
            }
            next_rx = BuildRxInfoLocked();
        }


    }

    // TX/H.264 create replies are forwarded untouched. Apps explicitly invoke
    // OpenTxSurface/OpenH264Stream within OnMessage if they want to use SHM.
    void HandleH264Invalidated(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_ntf_encode_stream_invalidated()) return;
        const auto& msg = packet.payload().p2a_ntf_encode_stream_invalidated();
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (h264_stream_id == msg.stream_id()) {
                h264_shm.reset();
                h264_stream_id = 0;
            }
        }

    }

    void HandleH264FrameReady(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_ntf_encode_frame_ready()) return;
        const auto& msg = packet.payload().p2a_ntf_encode_frame_ready();
        // A queued notification may arrive after this Session unsubscribes.
        // Ignore it instead of reporting a stale frame to the application.
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (h264_shm && h264_stream_id != msg.stream_id()) return;
        }

    }

    void HandleUSBHostOnlineState(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_ntf_usb_host_state()) {
            return;
        }
        const auto& body = packet.payload().p2a_ntf_usb_host_state();
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            usb_host_online = body.online();
        }

    }

    void HandleCommPeerState(const ProtoPackage& packet) {
        if (!packet.payload().has_p2a_ntf_comm_peer_state()) {
            return;
        }
        const auto& body = packet.payload().p2a_ntf_comm_peer_state();
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (comm_channel_id == 0 || body.channel_id() != comm_channel_id) {
                return;
            }
            comm_peer_online = body.online();
        }
        SdkLog("INFO", "comm peer state", "track_id=", packet.header().track_id(),
               "channel_id=", body.channel_id(), "online=", body.online(),
               "reason=", body.reason());

    }

    void HandlePacket(const ProtoPackage& packet) {
        bool forward = true;
        SdkLog("DEBUG", "IPC receive", "msg_type=", static_cast<int>(packet.header().msg_type()),
               "track_id=", packet.header().track_id(),
               "serialized_bytes=", packet.ByteSizeLong());
        switch (packet.header().msg_type()) {
        case kavio::ipc::proto::MSG_P2A_RSP_ERROR:
            HandleError(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_RSP_STATUS:
            HandleStatusReply(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_RSP_CREATE_SESSION:
            HandleSessionCreated(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_RSP_RUNTIME_DATA:
        case kavio::ipc::proto::MSG_P2A_NTF_RUNTIME_DATA:
            HandleRuntimeDataPacket(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_NTF_RX_SHM_READY:
            HandleRxShmReady(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_NTF_RX_SHM_INVALIDATED:
            HandleRxShmInvalidated(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_NTF_ENCODE_STREAM_INVALIDATED:
            HandleH264Invalidated(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_NTF_ENCODE_FRAME_READY:
            HandleH264FrameReady(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_NTF_USB_HOST_STATE:
            HandleUSBHostOnlineState(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_NTF_COMM_PEER_STATE:
            HandleCommPeerState(packet);
            break;
        case kavio::ipc::proto::MSG_P2A_DATA_COMM_CHANNEL: {
            forward = packet.payload().has_p2a_data_comm_channel();
            if (forward) {
                const auto& body = packet.payload().p2a_data_comm_channel();
                forward = !body.payload().empty() &&
                          body.payload().size() <= kCommChannelMaxPayloadSize;
                if (forward) {
                    std::lock_guard<std::mutex> lk(session_mutex);
                    forward = comm_channel_id != 0 && body.channel_id() == comm_channel_id;
                }
            }
            break;
        }
        case kavio::ipc::proto::MSG_P2A_REQ_GET_APP_CONFIG: {
            forward = false;
            if (packet.header().track_id() == 0 ||
                !packet.payload().has_p2a_req_get_app_config()) break;

            auto response = MakePackage(kavio::ipc::proto::MSG_A2P_RSP_GET_APP_CONFIG,
                                        packet.header().track_id());
            auto* reply = response.mutable_payload()->mutable_a2p_rsp_get_app_config();
            reply->set_schema_description_json(app_config.DescriptionJson());
            reply->set_config_json(app_config.CurrentJson());
            const auto st = SendPacket(response);
            if (!st.ok()) {
                SdkLog("ERROR", "GET app config reply failed", "track_id=",
                       packet.header().track_id(), "reason=", st.message);
            }
            break;
        }
        case kavio::ipc::proto::MSG_P2A_REQ_SET_APP_CONFIG: {
            forward = false;
            if (packet.header().track_id() == 0 ||
                !packet.payload().has_p2a_req_set_app_config()) break;
            const auto& json = packet.payload().p2a_req_set_app_config().config_json();
            if (json.empty() || json.size() > kAppConfigMaxPayloadSize) break;

            Json::Value value;
            Json::CharReaderBuilder reader_options;
            std::string error;
            std::unique_ptr<Json::CharReader> reader(reader_options.newCharReader());
            Status st = reader->parse(json.data(), json.data() + json.size(), &value, &error)
                ? app_config.Update(value)
                : Status::Error(StatusCode::Common_InvalidArgument,
                                "invalid configuration JSON: " + error);

            auto response = MakePackage(kavio::ipc::proto::MSG_A2P_RSP_SET_APP_CONFIG,
                                        packet.header().track_id());
            auto* reply = response.mutable_payload()->mutable_a2p_rsp_set_app_config();
            if (st.ok()) {
                reply->set_config_json(app_config.CurrentJson());
            } else {
                reply->set_error_code(static_cast<int32_t>(st.code));
                reply->set_error_message(st.message);
            }
            const auto sent = SendPacket(response);
            if (!sent.ok()) {
                SdkLog("ERROR", "SET app config reply failed", "track_id=",
                       packet.header().track_id(), "reason=", sent.message);
            }
            forward = st.ok();  // Only committed SET reaches the App OnMessage callback.
            break;
        }
        default:
            break;
        }
        if (forward) Enqueue(packet);
    }

    void RecvLoop() {
        while (!stop_requested.load(std::memory_order_acquire)) {
            ProtoPackage packet;
            const auto st = socket->Receive(packet);
            if (!st.ok()) {
                if (!stop_requested.load(std::memory_order_acquire)) {
                    HandleTransportDisconnected(st);
                }
                break;
            }
            HandlePacket(packet);
        }
    }

    Status StartConnectionAttemptLocked() {
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            ResetSessionDataLocked();
            SetClientStateLocked(ClientState::kInvalid);
        }
        transport_connected.store(false, std::memory_order_release);
        connected.store(false, std::memory_order_release);

        auto new_socket = std::make_unique<IPCSocket>();
        auto st = new_socket->Connect(DefaultIpcSocketPath());
        if (!st.ok()) {
            SdkLog("WARN", "connect failed", "socket=", DefaultIpcSocketPath(),
                   "reason=", st.message);
            return st;
        }
        if (stop_requested.load(std::memory_order_acquire)) {
            new_socket->Disconnect();
            return Status::Error(StatusCode::Common_Cancelled, "client stopped");
        }
        {
            std::lock_guard<std::mutex> socket_lk(socket_mutex);
            if (stop_requested.load(std::memory_order_acquire)) {
                new_socket->Disconnect();
                return Status::Error(StatusCode::Common_Cancelled, "client stopped");
            }
            socket = std::move(new_socket);
        }
        SdkLog("INFO", "transport connected", "socket=", DefaultIpcSocketPath());
        transport_connected.store(true, std::memory_order_release);

        recv_thread = std::thread([this] { RecvLoop(); });

        const uint64_t create_track_id = AllocTrackId();
        session_create_track_id.store(create_track_id, std::memory_order_release);
        auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_CREATE_SESSION, create_track_id);
        auto* body = req.mutable_payload()->mutable_a2p_req_create_session();
        body->set_client_name(ResolveClientName(session_options));
        body->set_client_pid(ResolveClientPid(session_options));
        auto* sdk_version = body->mutable_sdk_version();
        sdk_version->set_major(KAVIO_SDK_VERSION_MAJOR);
        sdk_version->set_minor(KAVIO_SDK_VERSION_MINOR);
        sdk_version->set_patch(KAVIO_SDK_VERSION_PATCH);
        auto* rx = body->mutable_rx_params();
        rx->CopyFrom(rx_params);
        auto* tx = body->mutable_tx_params();
        tx->CopyFrom(tx_params);

        SdkLog("INFO", "session create send", "track_id=", create_track_id,
               "client_name=", body->client_name(), "reported_pid=", body->client_pid(),
               "sdk_version=", KAVIO_SDK_VERSION_STRING,
               "rx_enabled=", rx->enabled(), "rx_sample_interval=", rx->frame_sample_interval(),
               "tx_enabled=", tx->enabled(), "tx_z_order=", tx->z_order());
        st = SendPacket(req);
        if (!st.ok()) {
            {
                std::lock_guard<std::mutex> socket_lk(socket_mutex);
                if (socket) {
                    socket->Disconnect();
                }
            }
            if (recv_thread.joinable()) {
                recv_thread.join();
            }
            {
                std::lock_guard<std::mutex> socket_lk(socket_mutex);
                socket.reset();
            }
            return st;
        }

        return Status::Ok();
    }

    void StopActiveConnection() {
        {
            std::lock_guard<std::mutex> lk(socket_mutex);
            if (socket) {
                socket->Disconnect();
            }
        }
        if (recv_thread.joinable()) {
            recv_thread.join();
        }
        {
            std::lock_guard<std::mutex> lk(socket_mutex);
            socket.reset();
        }
        transport_connected.store(false, std::memory_order_release);
        connected.store(false, std::memory_order_release);
    }

    void RunLoop() {
        constexpr auto kReconnectInterval = std::chrono::seconds(3);
        SdkLog("INFO", "worker enter");
        while (!stop_requested.load(std::memory_order_acquire)) {
            Status st;
            {
                std::lock_guard<std::mutex> lk(lifecycle_mutex);
                st = StartConnectionAttemptLocked();
            }

            if (!st.ok()) {
                std::unique_lock<std::mutex> lk(lifecycle_mutex);
                connection_cv.wait_for(lk, kReconnectInterval, [&] {
                    return stop_requested.load(std::memory_order_acquire);
                });
                continue;
            }

            bool ready_seen = false;
            std::unique_lock<std::mutex> lk(lifecycle_mutex);
            while (!stop_requested.load(std::memory_order_acquire)) {
                connection_cv.wait(lk, [&] {
                    if (stop_requested.load(std::memory_order_acquire)) {
                        return true;
                    }
                    if (!ready_seen) {
                        return connected.load(std::memory_order_acquire) ||
                               !transport_connected.load(std::memory_order_acquire);
                    }
                    return !transport_connected.load(std::memory_order_acquire);
                });

                if (stop_requested.load(std::memory_order_acquire)) {
                    break;
                }

                if (!ready_seen && connected.load(std::memory_order_acquire)) {
                    ready_seen = true;
                    continue;
                }

                break;
            }
            lk.unlock();

            if (stop_requested.load(std::memory_order_acquire)) {
                break;
            }

            StopActiveConnection();

            std::unique_lock<std::mutex> wait_lk(lifecycle_mutex);
            connection_cv.wait_for(wait_lk, kReconnectInterval, [&] {
                return stop_requested.load(std::memory_order_acquire);
            });
        }

        StopActiveConnection();

        RuntimeData runtime_snapshot;
        RxInfo prev_rx;
        RxInfo next_rx;
        bool state_changed = false;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            transport_connected.store(false, std::memory_order_release);
            connected.store(false, std::memory_order_release);
            ResetSessionDataLocked();
            next_rx = BuildRxInfoLocked();
            state_changed = SetClientStateLocked(ClientState::kInvalid);
            runtime_snapshot = runtime_data;
        }
        if (state_changed && !stop_requested.load(std::memory_order_acquire))
            EnqueueDisconnected();
        worker_running.store(false, std::memory_order_release);
        SdkLog("INFO", "worker leave");
        connection_cv.notify_all();
    }

    void JoinWorkerThread() {
        if (run_thread.joinable() && std::this_thread::get_id() != run_thread.get_id()) {
            run_thread.join();
        }
        if (recv_thread.joinable() && std::this_thread::get_id() != recv_thread.get_id()) {
            recv_thread.join();
        }
    }
};

TxSurface::TxSurface() : impl_(std::make_unique<Impl>()) {}

TxSurface::~TxSurface() = default;

TxSurface::TxSurface(TxSurface&& other) noexcept = default;

TxSurface& TxSurface::operator=(TxSurface&& other) noexcept = default;

bool TxSurface::IsValid() const {
    if (!impl_ || !impl_->shm) {
        return false;
    }
    const auto owner = impl_->owner.lock();
    return owner && owner->IsTxSurfaceCurrent(impl_->info.id(), impl_->shm) && impl_->shm->IsValid();
}

uint32_t TxSurface::id() const {
    return impl_ ? impl_->info.id() : 0;
}

uint32_t TxSurface::width() const {
    return impl_ ? impl_->info.width() : 0;
}

uint32_t TxSurface::height() const {
    return impl_ ? impl_->info.height() : 0;
}

uint32_t TxSurface::stride() const {
    return impl_ ? impl_->info.stride() : 0;
}

PixelFormat TxSurface::pixel_format() const {
    return impl_ ? FromProto(impl_->info.pixel_format()) : PixelFormat::kUnknown;
}

Status TxSurface::WriteFrame(const ImageFrame& frame) {
    if (!IsValid()) {
        return Status::Error(StatusCode::Common_NotConnected, "tx surface is not valid");
    }

    if (frame.width != impl_->info.width() ||
        frame.height != impl_->info.height() ||
        frame.stride != impl_->info.stride() ||
        frame.pixel_format != FromProto(impl_->info.pixel_format())) {
        return Status::Error(StatusCode::Common_InvalidArgument, "frame geometry does not match tx surface");
    }

    const size_t bytes = static_cast<size_t>(frame.height) *
                         static_cast<size_t>(frame.stride);
    if (frame.data.size() < bytes) {
        return Status::Error(StatusCode::Common_InvalidArgument, "frame buffer is too small");
    }

    Rect valid_rect = frame.valid_rect;
    if (valid_rect.empty()) {
        valid_rect = Rect{0, 0, frame.width, frame.height};
    }

    if (!impl_->shm->WriteLatest(frame.data.data(),
                                 static_cast<uint32_t>(bytes),
                                 frame.frame_id,
                                 frame.timestamp_ns,
                                 frame.width,
                                 frame.height,
                                 frame.stride,
                                 frame.pixel_format,
                                 frame.dest_pos_x,
                                 frame.dest_pos_y,
                                 valid_rect)) {
        return Status::Error(StatusCode::SHM_OperationFailed, "write tx surface failed");
    }

    return Status::Ok();
}

Status TxSurface::Clear() {
    if (!IsValid()) {
        return Status::Error(StatusCode::Common_NotConnected, "tx surface is not valid");
    }
    impl_->shm->ClearAllBuffers();
    return Status::Ok();
}

Status TxSurface::Destroy(uint64_t& request_id) {
    if (!impl_ || !impl_->shm)
        return Status::Error(StatusCode::Common_InvalidArgument, "tx surface is not valid");
    auto owner = impl_->owner.lock();
    if (!owner)
        return Status::Error(StatusCode::Common_NotConnected, "tx surface owner is unavailable");
    return owner->DestroyTxSurface(impl_->info.id(), impl_->shm, request_id);
}

KavioClient::KavioClient() : impl_(std::make_shared<Impl>()) {}

KavioClient::~KavioClient() {
    Stop();
    impl_->JoinWorkerThread();
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mutex);
        impl_->dispatch_stop = true;
    }
    impl_->queue_cv.notify_all();
    if (impl_->dispatch_thread.joinable() &&
        impl_->dispatch_thread.get_id() != std::this_thread::get_id())
        impl_->dispatch_thread.join();
}

Status KavioClient::Start(const kavio::ipc::proto::A2PReqCreateSessionPb& options,
                          IClientMessageHandler& handler,
                          bool auto_query_runtime_on_connect) {
    auto effective = options;
    if (!effective.has_rx_params()) {
        auto* rx = effective.mutable_rx_params();
        rx->set_enabled(true);
        rx->set_frame_sample_interval(1);
    }
    if (!effective.has_tx_params()) effective.mutable_tx_params();
    if (effective.rx_params().frame_sample_interval() == 0) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "frame_sample_interval must be >= 1");
    }

    SdkLog("INFO", "Start requested", "client_name=", ResolveClientName(options),
           "pid=", ResolveClientPid(options), "rx_enabled=", effective.rx_params().enabled(),
           "tx_enabled=", effective.tx_params().enabled(), "socket=", DefaultIpcSocketPath());
    {
        std::lock_guard<std::mutex> lifecycle_lk(impl_->lifecycle_mutex);
        if (impl_->worker_running.load(std::memory_order_acquire)) {
            return Status::Error(StatusCode::Common_AlreadyExists, "client is already running or stopping");
        }
    }

    if (impl_->run_thread.joinable()) {
        impl_->run_thread.join();
    }

    const auto config_status = impl_->app_config.Initialize();
    if (!config_status.ok()) return config_status;

    {
        std::lock_guard<std::mutex> lifecycle_lk(impl_->lifecycle_mutex);
        std::lock_guard<std::mutex> session_lk(impl_->session_mutex);
        impl_->session_options = effective;
        impl_->auto_query_runtime_on_connect = auto_query_runtime_on_connect;
        impl_->handler = &handler;
        impl_->rx_params = effective.rx_params();
        impl_->tx_params = effective.tx_params();
        impl_->stop_requested.store(false, std::memory_order_release);
        impl_->connected.store(false, std::memory_order_release);
        impl_->transport_connected.store(false, std::memory_order_release);
        impl_->worker_running.store(true, std::memory_order_release);
        impl_->session_create_track_id.store(0, std::memory_order_release);
        impl_->next_track_id.store(1, std::memory_order_relaxed);
        impl_->ResetSessionDataLocked();
        impl_->client_state = ClientState::kInvalid;
    }

    if (impl_->dispatch_thread.joinable()) impl_->dispatch_thread.join();
    {
        std::lock_guard<std::mutex> lk(impl_->queue_mutex);
        impl_->messages.clear(); impl_->queue_bytes = 0; impl_->dispatch_stop = false;
    }
    impl_->dispatch_thread = std::thread([impl = impl_] { impl->DispatchLoop(); });
    impl_->run_thread = std::thread([impl = impl_] { impl->RunLoop(); });
    return Status::Ok();
}

void KavioClient::Stop() {
    SdkLog("INFO", "Stop requested");
    {
        std::lock_guard<std::mutex> lk(impl_->session_mutex);
        impl_->stop_requested.store(true, std::memory_order_release);
        impl_->connected.store(false, std::memory_order_release);
        impl_->transport_connected.store(false, std::memory_order_release);
        impl_->ResetSessionDataLocked();
        impl_->SetClientStateLocked(ClientState::kInvalid);
    }



    {
        std::lock_guard<std::mutex> lk(impl_->socket_mutex);
        if (impl_->socket) {
            impl_->socket->Disconnect();
        }
    }

    { std::lock_guard<std::mutex> lk(impl_->queue_mutex);
      impl_->messages.clear(); impl_->queue_bytes = 0; impl_->dispatch_stop = true; }
    impl_->queue_cv.notify_all();
    impl_->connection_cv.notify_all();
}

bool KavioClient::IsClientReady() const {
    return impl_->SnapshotClientReady();
}

bool KavioClient::IsUSBHostOnline() const {
    return impl_->SnapshotUSBHostOnline();
}

bool KavioClient::IsCommChannelReady() const {
    return impl_->SnapshotCommChannelReady();
}

KavioClient::RuntimeData KavioClient::GetRuntimeData() const {
    return impl_->SnapshotRuntimeData();
}


RxInfo KavioClient::GetRxInfo() const {
    return impl_->SnapshotRxInfo();
}

Status KavioClient::SetRxParams(const kavio::ipc::proto::RxParamsPb& params, uint64_t& request_id) {
    if (!params.frame_sample_interval())
        return Status::Error(StatusCode::Common_InvalidArgument, "frame_sample_interval must be >= 1");
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    const auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_SET_RX_PARAMS, id);
    auto* p = req.mutable_payload()->mutable_a2p_req_set_rx_params()->mutable_params();
    p->CopyFrom(params);
    { std::lock_guard<std::mutex> lk(impl_->pending_mutex); impl_->pending_rx_params[id] = params; }
    auto st = impl_->SendPacket(req);
    if (!st.ok()) { std::lock_guard<std::mutex> lk(impl_->pending_mutex); impl_->pending_rx_params.erase(id); return st; }
    request_id = id;
    return Status::Ok();
}

Status KavioClient::SetTxParams(const kavio::ipc::proto::TxParamsPb& params, uint64_t& request_id) {
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    const auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_SET_TX_PARAMS, id);
    auto* p = req.mutable_payload()->mutable_a2p_req_set_tx_params()->mutable_params();
    p->CopyFrom(params);
    { std::lock_guard<std::mutex> lk(impl_->pending_mutex); impl_->pending_tx_params[id] = params; }
    auto st = impl_->SendPacket(req);
    if (!st.ok()) { std::lock_guard<std::mutex> lk(impl_->pending_mutex); impl_->pending_tx_params.erase(id); return st; }
    request_id = id;
    return Status::Ok();
}

Status KavioClient::ReadLatestRxFrame(ImageFrame& out_frame) {
    std::lock_guard<std::mutex> lk(impl_->session_mutex);
    if (!impl_->IsSessionReadyLocked() || impl_->client_state != ClientState::kReady) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    if (!impl_->rx_frame_ready || !impl_->rx_shm || !impl_->rx_shm->IsValid()) {
        return Status::Error(StatusCode::Media_VideoNotReady, "rx frame is not ready");
    }

    FrameShm::ReadView view;
    if (!impl_->rx_shm->BeginReadLatest(&view)) {
        return Status::Error(StatusCode::Media_NoFrame, "rx frame is not available");
    }

    out_frame.frame_id = view.frame_id;
    out_frame.timestamp_ns = view.timestamp_ns;
    out_frame.width = view.width;
    out_frame.height = view.height;
    out_frame.stride = view.stride;
    out_frame.pixel_format = view.pixel_format;
    out_frame.dest_pos_x = view.dest_pos_x;
    out_frame.dest_pos_y = view.dest_pos_y;
    out_frame.valid_rect = view.valid_rect;
    out_frame.data.assign(view.payload, view.payload + view.payload_size);
    impl_->rx_shm->EndRead(view.index);
    return Status::Ok();
}

Status KavioClient::CreateTxSurface(uint32_t width, uint32_t height, uint64_t& request_id) {
    if (!width || !height)
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid tx size");
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_CREATE_TX_SHM, id);
    auto* p = req.mutable_payload()->mutable_a2p_req_create_tx_shm();
    p->set_width(width); p->set_height(height);
    p->set_pixel_format(kavio::ipc::proto::PIXEL_FORMAT_BGRA8888);
    auto st = impl_->SendPacket(req);
    if (st.ok()) request_id = id;
    return st;
}

Status KavioClient::OpenTxSurface(const kavio::ipc::proto::ShmInfoPb& remote,
                                  TxSurface& surface) {
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    const auto& info = remote;
    if (!info.id() || info.name().empty())
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid tx shm info");
    if (surface.IsValid())
        return Status::Error(StatusCode::Common_InvalidState,
                             "destroy the existing surface before opening another");
    std::string error;
    FrameShm::OpenConfig cfg; cfg.name = info.name();
    auto opened = FrameShm::Open(cfg, &error);
    if (!opened || !opened->IsValid())
        return Status::Error(StatusCode::SHM_OperationFailed,
                             error.empty() ? "unable to open TX SHM" : error);
    auto shared = std::shared_ptr<FrameShm>(opened.release());
    {
        std::lock_guard<std::mutex> lk(impl_->session_mutex);
        if (!impl_->IsSessionReadyLocked())
            return Status::Error(StatusCode::Common_NotConnected, "session disconnected");
        if (impl_->tx_shms.count(info.id()))
            return Status::Error(StatusCode::Common_AlreadyExists, "TX SHM already opened");
        impl_->tx_shms[info.id()] = shared;
    }
    if (!surface.impl_) surface.impl_ = std::make_unique<TxSurface::Impl>();
    surface.impl_->info = info;
    surface.impl_->shm = shared;
    surface.impl_->owner = impl_;
    return Status::Ok();
}

Status KavioClient::DestroyTxSurface(uint32_t shm_id, uint64_t& request_id) {
    if (!shm_id)
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid tx shm id");
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_DESTROY_TX_SHM, id);
    req.mutable_payload()->mutable_a2p_req_destroy_tx_shm()->set_shm_id(shm_id);
    { std::lock_guard<std::mutex> lk(impl_->pending_mutex);
      impl_->pending_tx_destroy[id] = shm_id; }
    auto st = impl_->SendPacket(req);
    if (!st.ok()) { std::lock_guard<std::mutex> lk(impl_->pending_mutex);
                    impl_->pending_tx_destroy.erase(id); return st; }
    request_id = id;
    return Status::Ok();
}

Status KavioClient::CloseSession(uint32_t session_id, uint64_t& request_id) {
    if (!session_id)
        return Status::Error(StatusCode::Common_InvalidArgument, "session_id is zero");
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_CLOSE_SESSION, id);
    req.mutable_payload()->mutable_a2p_req_close_session()->set_session_id(session_id);
    auto st = impl_->SendPacket(req);
    if (st.ok()) request_id = id;
    return st;
}

Status KavioClient::QueryStats(uint64_t& request_id) {
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_QUERY_STATS, id);
    req.mutable_payload()->mutable_a2p_req_query_stats()->set_all_sessions(false);
    auto st = impl_->SendPacket(req);
    if (st.ok()) request_id = id;
    return st;
}

Status KavioClient::SubscribeH264Stream(uint64_t& request_id) {
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_CREATE_ENCODE_STREAM, id);
    req.mutable_payload()->mutable_a2p_req_create_encode_stream();
    auto st = impl_->SendPacket(req);
    if (st.ok()) request_id = id;
    return st;
}

Status KavioClient::OpenH264Stream(const kavio::ipc::proto::EncodedStreamShmInfoPb& remote) {
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    if (!remote.stream_id() || remote.name().empty())
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid h264 shm info");
    EncodedStreamShm::OpenConfig cfg;
    cfg.name = remote.name(); cfg.size = static_cast<size_t>(remote.size());
    cfg.access = ShmAccess::ReadOnly;
    std::string error;
    auto opened = EncodedStreamShm::Open(cfg, &error);
    if (!opened)
        return Status::Error(StatusCode::SHM_OperationFailed,
                             error.empty() ? "unable to open H264 SHM" : error);
    {
        std::lock_guard<std::mutex> lk(impl_->session_mutex);
        if (!impl_->IsSessionReadyLocked())
            return Status::Error(StatusCode::Common_NotConnected, "session disconnected");
        if (impl_->h264_shm)
            return Status::Error(StatusCode::Common_AlreadyExists, "H264 SHM already open");
        impl_->h264_stream_id = remote.stream_id();
        impl_->h264_shm = std::shared_ptr<EncodedStreamShm>(opened.release());
    }
    return Status::Ok();
}

Status KavioClient::UnsubscribeH264Stream(uint32_t stream_id, uint64_t& request_id) {
    if (!stream_id)
        return Status::Error(StatusCode::Common_InvalidArgument, "stream_id is zero");
    if (!impl_->IsSessionReady())
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    auto id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_REQ_DESTROY_ENCODE_STREAM, id);
    req.mutable_payload()->mutable_a2p_req_destroy_encode_stream()->set_stream_id(stream_id);
    { std::lock_guard<std::mutex> lk(impl_->pending_mutex);
      impl_->pending_h264_unsubscribe[id] = stream_id; }
    auto st = impl_->SendPacket(req);
    if (!st.ok()) { std::lock_guard<std::mutex> lk(impl_->pending_mutex);
                    impl_->pending_h264_unsubscribe.erase(id); return st; }
    request_id = id;
    return Status::Ok();
}

uint64_t KavioClient::GetLatestH264PacketSeq(uint32_t stream_id) const {
    std::lock_guard<std::mutex> lk(impl_->session_mutex);
    if (!impl_->IsSessionReadyLocked() || !impl_->h264_shm ||
        impl_->h264_stream_id != stream_id) return 0;
    return impl_->h264_shm->LatestSeq();
}

Status KavioClient::ReadH264Packet(uint32_t stream_id, uint64_t seq,
                                  H264Packet& packet) const {
    std::lock_guard<std::mutex> lk(impl_->session_mutex);
    if (!impl_->IsSessionReadyLocked() ||
        !impl_->h264_shm || impl_->h264_stream_id != stream_id)
        return Status::Error(StatusCode::Common_InvalidState, "H264 stream not active");
    EncodedPacket local;
    Status st = impl_->h264_shm->ReadPacket(seq, local);
    if (!st.ok()) return st;
    packet.seq = local.seq;
    packet.pts_us = local.pts_us;
    packet.dts_us = local.dts_us;
    packet.timestamp_ns = local.timestamp_ns;
    packet.flags = local.flags;
    packet.nal_type = local.nal_type;
    packet.data = std::move(local.data);
    return Status::Ok();
}

Status KavioClient::SendInputEvent(const kavio::input::proto::InputEventPb& event) {
    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_A2P_NTF_INPUT_EVENT, track_id);
    req.mutable_payload()->mutable_a2p_ntf_input_event()->CopyFrom(event);
    return impl_->SendPacket(req);
}

uint32_t KavioClient::GetCommChannelId() const {
    std::lock_guard<std::mutex> lk(impl_->session_mutex);
    return impl_->comm_channel_id;
}

Status KavioClient::SendCommChannelData(const void* data, size_t size) {
    if (!data || size == 0 || size > kCommChannelMaxPayloadSize) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "comm channel payload must be 1 byte to 1 MiB");
    }
    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    uint32_t channel_id = 0;
    {
        std::lock_guard<std::mutex> lk(impl_->session_mutex);
        if (!impl_->usb_host_online || impl_->comm_channel_id == 0 || !impl_->comm_peer_online) {
            return Status::Error(StatusCode::Common_NotConnected,
                                 "app comm channel is not ready");
        }
        channel_id = impl_->comm_channel_id;
    }

    const uint64_t track_id = impl_->AllocTrackId();
    SdkLog("DEBUG", "channel send", "track_id=", track_id,
           "channel_id=", channel_id, "payload_bytes=", size);
    auto packet = MakePackage(kavio::ipc::proto::MSG_A2P_DATA_COMM_CHANNEL, track_id);
    auto* body = packet.mutable_payload()->mutable_a2p_data_comm_channel();
    body->set_channel_id(channel_id);
    if (size > 0) {
        body->set_payload(data, size);
    }
    const Status status = impl_->SendPacket(packet);
    if (!status.ok()) {
        SdkLog("ERROR", "channel send failed", "track_id=", track_id,
               "channel_id=", channel_id, "payload_bytes=", size,
               "reason=", status.message);
    }
    return status;
}

Status KavioClient::SendCommChannelData(const std::vector<uint8_t>& data) {
    return SendCommChannelData(data.empty() ? nullptr : data.data(), data.size());
}

Json::Value KavioClient::GetAppConfig() const { return impl_->app_config.Snapshot(); }
Status KavioClient::UpdateAppConfig(const Json::Value& value) {
    return impl_->app_config.Update(value);
}
Status KavioClient::ResetAppConfigToDefaults() {
    return impl_->app_config.ResetToDefaults();
}



}  // namespace kavio
