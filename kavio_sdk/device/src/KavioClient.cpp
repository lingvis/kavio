/*
 * KavioClient.cpp
 *
 * Device Component SDK client implementation for Platform IPC sessions, automatic reconnect,
 * private App Channel state and raw channel datagrams. Payload content is not logged.
 */

#include <kavio/KavioClient.h>
#include "internal/InternalTypes.h"

#include "internal/FrameShm.h"
#include "internal/IPCSocket.h"
#include <kavio/KavioSdkVersion.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <future>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <unistd.h>

namespace kavio {
namespace {

using ProtoError = kavio::ipc::proto::ErrorPb;
using ProtoHeader = kavio::ipc::proto::IPCHeaderPb;
using ProtoMessageType = kavio::ipc::proto::MessageTypePb;
using ProtoPackage = kavio::ipc::proto::IPCPackagePb;
using ProtoPixelFormat = kavio::ipc::proto::PixelFormatPb;
using ProtoStatusCode = kavio::ipc::proto::StatusCodePb;

std::mutex g_sdk_log_mutex;
thread_local const void* g_sdk_callback_owner = nullptr;

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

std::string ResolveClientName(const SessionOptions& options) {
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
    return !options.client_name.empty() ? options.client_name : "kavio_sdk_client";
}

int32_t ResolveClientPid(const SessionOptions& options) {
    return options.client_pid != 0 ? options.client_pid : static_cast<int32_t>(::getpid());
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

ShmInfo BuildShmInfo(const kavio::ipc::proto::ShmInfoPb& in) {
    ShmInfo out;
    out.id = in.id();
    out.name = in.name();
    out.size = in.size();
    out.width = in.width();
    out.height = in.height();
    out.stride = in.stride();
    out.pixel_format = FromProto(in.pixel_format());
    return out;
}

ProtoPackage MakePackage(ProtoMessageType type, uint64_t track_id) {
    ProtoPackage package;
    ProtoHeader* header = package.mutable_header();
    header->set_msg_type(type);
    header->set_track_id(track_id);
    return package;
}

StatusCode FromProtoStatusCode(ProtoStatusCode code) {
    switch (code) {
    case kavio::ipc::proto::STATUS_OK:
        return StatusCode::OK;
    case kavio::ipc::proto::STATUS_INVALID_ARGUMENT:
        return StatusCode::Common_InvalidArgument;
    case kavio::ipc::proto::STATUS_TOO_MANY_SESSIONS:
        return StatusCode::Session_TooManySessions;
    case kavio::ipc::proto::STATUS_ALREADY_EXISTS:
        return StatusCode::Common_AlreadyExists;
    case kavio::ipc::proto::STATUS_NOT_FOUND:
        return StatusCode::Common_NotFound;
    case kavio::ipc::proto::STATUS_SHM_CREATE_FAILED:
        return StatusCode::SHM_CreateFailed;
    case kavio::ipc::proto::STATUS_SOCKET_ERROR:
        return StatusCode::IPC_SocketError;
    case kavio::ipc::proto::STATUS_PROTOCOL_ERROR:
        return StatusCode::Common_ProtocolError;
    case kavio::ipc::proto::STATUS_INTERNAL_ERROR:
        return StatusCode::Common_InternalError;
    case kavio::ipc::proto::STATUS_INVALID_STATE:
        return StatusCode::Common_InvalidState;
    case kavio::ipc::proto::STATUS_UNSUPPORTED:
        return StatusCode::Common_Unsupported;
    default:
        return StatusCode::Common_InternalError;
    }
}

Status ConvertStatusReply(const kavio::ipc::proto::NtfStatusReplyPb& reply) {
    const auto code = static_cast<ProtoStatusCode>(reply.code());
    if (code == kavio::ipc::proto::STATUS_OK) {
        return Status::Ok();
    }
    return Status::Error(FromProtoStatusCode(code), reply.message());
}

Status ConvertErrorReply(const ProtoError& error) {
    const auto code = error.code();
    if (code == kavio::ipc::proto::STATUS_OK) {
        return Status::Ok();
    }
    return Status::Error(FromProtoStatusCode(code), error.message());
}

enum class ClientState {
    kInvalid = 0,
    kReady,
};

enum class PendingKind {
    kSetRxParams = 0,
    kSetTxParams,
    kCreateTxSurface,
    kDestroyTxSurface,
    kCloseSession,
    kQueryStats,
};

template <typename T>
struct PendingRequest {
    uint64_t track_id = 0;
    PendingKind kind = PendingKind::kSetRxParams;
    std::chrono::steady_clock::time_point deadline;
    std::promise<T> promise;
};

struct TxSurfaceResult {
    Status status;
    ShmInfo info;
    std::shared_ptr<FrameShm> shm;
};

struct StatsReplyResult {
    Status status;
    KavioClient::StatsReply reply;
};

struct SurfaceOwner {
    virtual ~SurfaceOwner() = default;
    virtual bool IsTxSurfaceCurrent(uint32_t shm_id,
                                    const std::shared_ptr<FrameShm>& shm) const = 0;
    virtual Status DestroyTxSurface(uint32_t shm_id,
                                    const std::shared_ptr<FrameShm>& shm,
                                    std::chrono::milliseconds timeout) = 0;
};

template <typename T>
void TrySetPromiseValue(const std::shared_ptr<PendingRequest<T>>& pending, T value) {
    if (!pending) {
        return;
    }
    try {
        pending->promise.set_value(std::move(value));
    } catch (const std::future_error&) {
    }
}

}  // namespace

struct TxSurface::Impl {
    ShmInfo info;
    std::shared_ptr<FrameShm> shm;
    std::weak_ptr<SurfaceOwner> owner;
};

struct KavioClient::Impl : public std::enable_shared_from_this<KavioClient::Impl>,
                           public SurfaceOwner {
    SessionOptions session_options;
    Callbacks callbacks;

    mutable std::mutex lifecycle_mutex;
    mutable std::mutex session_mutex;
    mutable std::mutex socket_mutex;
    mutable std::mutex pending_mutex;
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

    SessionInfo session_info;
    RuntimeData runtime_data;
    UserConfig user_config;
    bool runtime_synced_once = false;
    RxParams rx_params;
    TxParams tx_params;
    bool rx_frame_ready = false;
    uint64_t rx_generation = 0;
    ClientState client_state = ClientState::kInvalid;
    bool usb_host_online = false;
    uint32_t comm_channel_id = 0;
    bool comm_peer_online = false;
    std::unique_ptr<FrameShm> rx_shm;
    std::unordered_map<uint32_t, std::shared_ptr<FrameShm>> tx_shms;

    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest<Status>>> pending_status_requests;
    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest<TxSurfaceResult>>> pending_tx_create_requests;
    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest<StatsReplyResult>>> pending_stats_requests;

    uint64_t AllocTrackId() {
        return next_track_id.fetch_add(1, std::memory_order_relaxed);
    }

    Callbacks SnapshotCallbacks() const {
        std::lock_guard<std::mutex> lk(lifecycle_mutex);
        return callbacks;
    }

    bool IsSessionReadyLocked() const {
        return connected.load(std::memory_order_acquire) && session_info.session_id != 0;
    }

    bool IsSessionReady() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return IsSessionReadyLocked();
    }

    void ResetSessionDataLocked() {
        session_info = {};
        runtime_data.Clear();
        user_config.Clear();
        runtime_synced_once = false;
        rx_frame_ready = false;
        rx_generation = 0;
        usb_host_online = false;
        comm_channel_id = 0;
        comm_peer_online = false;
        rx_shm.reset();
        tx_shms.clear();
    }

    RuntimeData SnapshotRuntimeData() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return runtime_data;
    }

    UserConfig SnapshotUserConfig() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return user_config;
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

        if (rx_frame_ready && rx_shm && rx_shm->IsValid() && session_info.rx_shm.id != 0) {
            out.frame_available = true;
        }

        return out;
    }

    RxInfo SnapshotRxInfo() const {
        std::lock_guard<std::mutex> lk(session_mutex);
        return BuildRxInfoLocked();
    }

    void ReportClientReady() {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_client_ready) {
            cb.on_client_ready();
        }
    }

    void ReportClientInvalid() {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_client_invalid) {
            cb.on_client_invalid();
        }
    }

    void ReportRuntimeDataChanged(const RuntimeData& runtime_snapshot) {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_runtime_changed) {
            cb.on_runtime_changed(runtime_snapshot);
        }
    }

    void ReportUserConfigChanged(const UserConfig& user_config_snapshot) {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_user_config_changed) {
            cb.on_user_config_changed(user_config_snapshot);
        }
    }

    void ReportRxChanged(const RxInfo& prev, const RxInfo& next) {
        if (prev == next) {
            return;
        }
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_rx_changed) {
            cb.on_rx_changed(prev, next);
        }
    }

    void ReportUSBHostOnlineChanged(bool online, const std::string& reason) {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_usb_host_online_changed) {
            cb.on_usb_host_online_changed(online, reason);
        }
    }

    void ReportCommChannelReadyChanged(bool ready) {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_comm_channel_ready_changed) {
            cb.on_comm_channel_ready_changed(ready);
        }
    }

    void ReportCommChannelData(const std::string& payload) {
        Callbacks cb = SnapshotCallbacks();
        if (cb.on_comm_channel_data) {
            cb.on_comm_channel_data(std::vector<uint8_t>(payload.begin(), payload.end()));
        }
    }

    bool ReportAppConfigRequest(
        const kavio::ipc::proto::NtfAppConfigRequestPb& request) {
        Callbacks cb = SnapshotCallbacks();
        if (!cb.on_app_config_request) {
            return false;
        }
        cb.on_app_config_request(request);
        return true;
    }

    void ReportClientStateChanged(ClientState state) {
        if (state == ClientState::kReady) {
            ReportClientReady();
        } else {
            ReportClientInvalid();
        }
    }

    bool SetClientStateLocked(ClientState next_state) {
        if (client_state == next_state) {
            return false;
        }
        client_state = next_state;
        return true;
    }

    void TransitionClientState(ClientState next_state) {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            changed = SetClientStateLocked(next_state);
        }
        if (changed) {
            ReportClientStateChanged(next_state);
        }
    }

    Status SendPacket(const ProtoPackage& packet) {
        std::lock_guard<std::mutex> lk(socket_mutex);
        if (!socket || !socket->IsConnected()) {
            return Status::Error(StatusCode::Common_NotConnected, "socket is not connected");
        }
        return socket->Send(packet);
    }

    Status EnsureBlockingAllowed() const {
        if (g_sdk_callback_owner == this) {
            return Status::Error(StatusCode::Common_InvalidState,
                                 "blocking SDK API cannot be called from SDK callback thread");
        }
        return Status::Ok();
    }

    void CancelPendingStatus(uint64_t track_id, const Status& status) {
        std::shared_ptr<PendingRequest<Status>> pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            const auto it = pending_status_requests.find(track_id);
            if (it == pending_status_requests.end()) {
                return;
            }
            pending = it->second;
            pending_status_requests.erase(it);
        }
        TrySetPromiseValue(pending, status);
    }

    void CancelPendingTxCreate(uint64_t track_id, const Status& status) {
        std::shared_ptr<PendingRequest<TxSurfaceResult>> pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            const auto it = pending_tx_create_requests.find(track_id);
            if (it == pending_tx_create_requests.end()) {
                return;
            }
            pending = it->second;
            pending_tx_create_requests.erase(it);
        }

        TxSurfaceResult result;
        result.status = status;
        TrySetPromiseValue(pending, std::move(result));
    }

    void CancelPendingStats(uint64_t track_id, const Status& status) {
        std::shared_ptr<PendingRequest<StatsReplyResult>> pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            const auto it = pending_stats_requests.find(track_id);
            if (it == pending_stats_requests.end()) {
                return;
            }
            pending = it->second;
            pending_stats_requests.erase(it);
        }

        StatsReplyResult result;
        result.status = status;
        TrySetPromiseValue(pending, std::move(result));
    }

    void FailAllPendingRequests(const Status& status) {
        std::unordered_map<uint64_t, std::shared_ptr<PendingRequest<Status>>> status_requests;
        std::unordered_map<uint64_t, std::shared_ptr<PendingRequest<TxSurfaceResult>>> tx_requests;
        std::unordered_map<uint64_t, std::shared_ptr<PendingRequest<StatsReplyResult>>> stats_requests;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            status_requests.swap(pending_status_requests);
            tx_requests.swap(pending_tx_create_requests);
            stats_requests.swap(pending_stats_requests);
        }

        for (auto& entry : status_requests) {
            TrySetPromiseValue(entry.second, status);
        }

        for (auto& entry : tx_requests) {
            TxSurfaceResult result;
            result.status = status;
            TrySetPromiseValue(entry.second, std::move(result));
        }

        for (auto& entry : stats_requests) {
            StatsReplyResult result;
            result.status = status;
            TrySetPromiseValue(entry.second, std::move(result));
        }
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
                            std::chrono::milliseconds timeout) override {
        Status st = EnsureBlockingAllowed();
        if (!st.ok()) {
            return st;
        }

        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (!IsSessionReadyLocked() || client_state != ClientState::kReady) {
                return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
            }
            const auto it = tx_shms.find(shm_id);
            if (it == tx_shms.end() || it->second != shm) {
                return Status::Error(StatusCode::Common_InvalidArgument, "surface does not belong to this client");
            }
        }

        const uint64_t track_id = AllocTrackId();
        auto pending = std::make_shared<PendingRequest<Status>>();
        pending->track_id = track_id;
        pending->kind = PendingKind::kDestroyTxSurface;
        pending->deadline = std::chrono::steady_clock::now() + timeout;
        auto future = pending->promise.get_future();
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            pending_status_requests[track_id] = pending;
        }

        auto req = MakePackage(kavio::ipc::proto::MSG_REQ_DESTROY_TX_SHM, track_id);
        req.mutable_payload()->mutable_req_destroy_tx_shm()->set_shm_id(shm_id);
        st = SendPacket(req);
        if (!st.ok()) {
            CancelPendingStatus(track_id, st);
            return st;
        }

        if (future.wait_for(timeout) != std::future_status::ready) {
            const Status timeout_status = Status::Error(StatusCode::Common_Timeout,
                                                        "destroy tx surface timeout");
            CancelPendingStatus(track_id, timeout_status);
            return timeout_status;
        }

        Status result = future.get();
        if (!result.ok()) {
            return result;
        }

        {
            std::lock_guard<std::mutex> lk(session_mutex);
            tx_shms.erase(shm_id);
            session_info.tx_shms.erase(
                std::remove_if(session_info.tx_shms.begin(),
                               session_info.tx_shms.end(),
                               [shm_id](const ShmInfo& info) {
                                   return info.id == shm_id;
                               }),
                session_info.tx_shms.end());
        }
        return Status::Ok();
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
        RuntimeData runtime_snapshot;
        RxInfo prev_rx;
        RxInfo next_rx;
        bool state_changed = false;
        bool old_host_online = false;
        bool old_channel_ready = false;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            old_host_online = usb_host_online;
            old_channel_ready = usb_host_online && comm_channel_id != 0 && comm_peer_online;
            transport_connected.store(false, std::memory_order_release);
            connected.store(false, std::memory_order_release);
            ResetSessionDataLocked();
            next_rx = BuildRxInfoLocked();
            state_changed = SetClientStateLocked(ClientState::kInvalid);
            runtime_snapshot = runtime_data;
        }

        FailAllPendingRequests(Status::Error(StatusCode::Common_NotConnected,
                                             "transport disconnected"));
        ReportRuntimeDataChanged(runtime_snapshot);
        ReportRxChanged(prev_rx, next_rx);
        if (old_host_online) {
            ReportUSBHostOnlineChanged(false, "IPC transport disconnected");
        }
        if (old_channel_ready) {
            ReportCommChannelReadyChanged(false);
        }
        if (state_changed) {
            ReportClientInvalid();
        }
        connection_cv.notify_all();
        (void)status;
    }

    void HandleSessionCreated(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_session_created()) {
            return;
        }

        const auto& body = packet.payload().ntf_session_created();
        if (!body.has_rx_params() || !body.has_tx_params()) {
            return;
        }

        bool state_changed = false;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            session_info.session_id = body.session_id();
            session_info.max_sessions = body.max_sessions();
            session_info.max_rx_shms_per_session = body.max_rx_shms_per_session();
            session_info.max_tx_shms_per_session = body.max_tx_shms_per_session();
            rx_params.enabled = body.rx_params().enabled();
            rx_params.frame_sample_interval = body.rx_params().frame_sample_interval();
            tx_params.enabled = body.tx_params().enabled();
            tx_params.z_order = body.tx_params().z_order();
            session_options.rx_params = rx_params;
            session_options.tx_params = tx_params;
            comm_channel_id = body.comm_channel_id();
            comm_peer_online = body.comm_peer_online();
            connected.store(true, std::memory_order_release);
            state_changed = SetClientStateLocked(ClientState::kReady);
        }

        if (state_changed) {
            ReportClientReady();
        }
        if (SnapshotCommChannelReady()) {
            ReportCommChannelReadyChanged(true);
        }
        connection_cv.notify_all();

        if (session_options.auto_query_runtime_on_connect) {
            const uint64_t track_id = AllocTrackId();
            auto req = MakePackage(kavio::ipc::proto::MSG_REQ_QUERY_RUNTIME_DATA, track_id);
            req.mutable_payload()->mutable_req_query_runtime_data();
            (void)SendPacket(req);
        }

        if (session_options.auto_query_user_config_on_connect) {
            const uint64_t track_id = AllocTrackId();
            auto req = MakePackage(kavio::ipc::proto::MSG_REQ_QUERY_USER_CONFIG, track_id);
            req.mutable_payload()->mutable_req_query_user_config();
            (void)SendPacket(req);
        }
    }

    void HandleError(const ProtoPackage& packet) {
        if (!packet.payload().has_error()) {
            return;
        }

        const ProtoError& error = packet.payload().error();
        const uint64_t track_id = error.track_id() != 0 ? error.track_id() : packet.header().track_id();
        const Status status = ConvertErrorReply(error);

        bool is_session_create_failure = false;
        if (track_id == session_create_track_id.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(session_mutex);
            is_session_create_failure = session_info.session_id == 0;
        }
        if (is_session_create_failure) {
            stop_requested.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(session_mutex);
                connected.store(false, std::memory_order_release);
                SetClientStateLocked(ClientState::kInvalid);
            }
            FailAllPendingRequests(status);
            ReportClientInvalid();
            std::lock_guard<std::mutex> socket_lk(socket_mutex);
            if (socket) {
                socket->Disconnect();
            }
            connection_cv.notify_all();
            return;
        }

        CancelPendingStatus(track_id, status);
        CancelPendingTxCreate(track_id, status);
        CancelPendingStats(track_id, status);
    }

    void HandleStatusReply(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_status_reply()) {
            return;
        }

        const uint64_t track_id = packet.header().track_id();
        if (track_id == 0) {
            return;
        }

        const Status status = ConvertStatusReply(packet.payload().ntf_status_reply());

        std::shared_ptr<PendingRequest<Status>> status_pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            auto it = pending_status_requests.find(track_id);
            if (it != pending_status_requests.end()) {
                status_pending = it->second;
                pending_status_requests.erase(it);
            }
        }
        if (status_pending) {
            TrySetPromiseValue(status_pending, status);
            return;
        }

        std::shared_ptr<PendingRequest<TxSurfaceResult>> tx_pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            const auto it = pending_tx_create_requests.find(track_id);
            if (it != pending_tx_create_requests.end()) {
                tx_pending = it->second;
                pending_tx_create_requests.erase(it);
            }
        }
        if (tx_pending) {
            TxSurfaceResult result;
            result.status = status;
            TrySetPromiseValue(tx_pending, std::move(result));
        }
    }

    void HandleRuntimeDataPacket(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_runtime_data()) {
            return;
        }

        RuntimeData runtime_snapshot;
        RxInfo prev_rx;
        RxInfo next_rx;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            MergeRuntimeDataLocked(packet.payload().ntf_runtime_data());
            runtime_snapshot = runtime_data;
            next_rx = BuildRxInfoLocked();
        }

        ReportRuntimeDataChanged(runtime_snapshot);
        ReportRxChanged(prev_rx, next_rx);
    }

    void HandleUserConfigPacket(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_user_config()) {
            return;
        }

        UserConfig user_config_snapshot;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            user_config = packet.payload().ntf_user_config().user_config();
            user_config_snapshot = user_config;
        }

        ReportUserConfigChanged(user_config_snapshot);
    }

    void HandleRxShmReady(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_rx_shm_ready()) {
            return;
        }

        const auto& body = packet.payload().ntf_rx_shm_ready();
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
            session_info.rx_shm = BuildShmInfo(body.rx_shm());

            if (!opened || !opened->IsValid()) {
                session_info.rx_shm = {};
                rx_frame_ready = false;
            } else {
                rx_shm = std::move(opened);
                rx_generation = body.generation();
                rx_frame_ready = true;
            }
            next_rx = BuildRxInfoLocked();
        }

        (void)error;
        ReportRxChanged(prev_rx, next_rx);
    }

    void HandleRxShmInvalidated(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_rx_shm_invalidated()) {
            return;
        }

        RxInfo prev_rx;
        RxInfo next_rx;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            prev_rx = BuildRxInfoLocked();
            const auto& body = packet.payload().ntf_rx_shm_invalidated();
            if (session_info.rx_shm.id == body.shm_id()) {
                rx_shm.reset();
                session_info.rx_shm = {};
                rx_frame_ready = false;
            }
            next_rx = BuildRxInfoLocked();
        }

        ReportRxChanged(prev_rx, next_rx);
    }

    void HandleTxCreated(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_tx_shm_created()) {
            return;
        }

        const uint64_t track_id = packet.header().track_id();
        if (track_id == 0) {
            return;
        }

        std::shared_ptr<PendingRequest<TxSurfaceResult>> pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            const auto it = pending_tx_create_requests.find(track_id);
            if (it == pending_tx_create_requests.end()) {
                return;
            }
            pending = it->second;
            pending_tx_create_requests.erase(it);
        }

        const ShmInfo info = BuildShmInfo(packet.payload().ntf_tx_shm_created().tx_shm());
        std::string error;
        FrameShm::OpenConfig open_config;
        open_config.name = info.name;
        std::unique_ptr<FrameShm> opened = FrameShm::Open(open_config, &error);

        TxSurfaceResult result;
        if (!opened || !opened->IsValid()) {
            result.status = Status::Error(StatusCode::SHM_OperationFailed,
                                          error.empty() ? "failed to open tx shm" : error);
            TrySetPromiseValue(pending, std::move(result));
            return;
        }

        result.status = Status::Ok();
        result.info = info;
        result.shm = std::shared_ptr<FrameShm>(opened.release());
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            tx_shms[info.id] = result.shm;
            session_info.tx_shms.push_back(info);
        }
        TrySetPromiseValue(pending, std::move(result));
    }

    void HandleStatsReply(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_stats_reply()) {
            return;
        }

        const uint64_t track_id = packet.header().track_id();
        if (track_id == 0) {
            return;
        }

        std::shared_ptr<PendingRequest<StatsReplyResult>> pending;
        {
            std::lock_guard<std::mutex> lk(pending_mutex);
            const auto it = pending_stats_requests.find(track_id);
            if (it == pending_stats_requests.end()) {
                return;
            }
            pending = it->second;
            pending_stats_requests.erase(it);
        }

        StatsReplyResult result;
        result.status = Status::Ok();
        result.reply = packet.payload().ntf_stats_reply();
        TrySetPromiseValue(pending, std::move(result));
    }

    void HandleUSBHostOnlineState(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_usb_host_online_state()) {
            return;
        }
        const auto& body = packet.payload().ntf_usb_host_online_state();
        bool host_changed = false;
        bool ready_changed = false;
        bool next_ready = false;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            const bool old_ready = usb_host_online && comm_channel_id != 0 && comm_peer_online;
            host_changed = usb_host_online != body.online();
            usb_host_online = body.online();
            next_ready = usb_host_online && comm_channel_id != 0 && comm_peer_online;
            ready_changed = old_ready != next_ready;
        }
        if (host_changed) {
            ReportUSBHostOnlineChanged(body.online(), body.reason());
        }
        if (ready_changed) {
            ReportCommChannelReadyChanged(next_ready);
        }
    }

    void HandleCommPeerState(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_comm_peer_state()) {
            return;
        }
        const auto& body = packet.payload().ntf_comm_peer_state();
        bool changed = false;
        bool next_ready = false;
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (comm_channel_id == 0 || body.channel_id() != comm_channel_id) {
                return;
            }
            const bool old_ready = usb_host_online && comm_channel_id != 0 && comm_peer_online;
            comm_peer_online = body.online();
            next_ready = usb_host_online && comm_channel_id != 0 && comm_peer_online;
            changed = old_ready != next_ready;
        }
        SdkLog("INFO", "comm peer state", "track_id=", packet.header().track_id(),
               "channel_id=", body.channel_id(), "online=", body.online(),
               "reason=", body.reason());
        if (changed) {
            ReportCommChannelReadyChanged(next_ready);
        }
    }

    void HandleCommChannelDataPacket(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_comm_channel_data()) {
            return;
        }
        const auto& body = packet.payload().ntf_comm_channel_data();
        if (body.payload().empty() ||
            body.payload().size() > kCommChannelMaxPayloadSize) {
            SdkLog("WARN", "invalid channel receive payload size",
                   "payload_bytes=", body.payload().size());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(session_mutex);
            if (comm_channel_id == 0 || body.channel_id() != comm_channel_id) {
                return;
            }
        }
        SdkLog("DEBUG", "channel receive", "track_id=", packet.header().track_id(),
               "channel_id=", body.channel_id(), "payload_bytes=", body.payload().size());
        ReportCommChannelData(body.payload());
    }

    void HandleAppConfigRequestPacket(const ProtoPackage& packet) {
        if (!packet.payload().has_ntf_app_config_request()) {
            return;
        }
        const auto& request = packet.payload().ntf_app_config_request();
        const bool valid_operation =
            request.operation() == kavio::ipc::proto::APP_CONFIG_OPERATION_GET ||
            request.operation() == kavio::ipc::proto::APP_CONFIG_OPERATION_SET;
        const bool valid_payload =
            request.config_json().size() <= kAppConfigMaxPayloadSize &&
            (request.operation() != kavio::ipc::proto::APP_CONFIG_OPERATION_GET ||
             request.config_json().empty()) &&
            (request.operation() != kavio::ipc::proto::APP_CONFIG_OPERATION_SET ||
             !request.config_json().empty());
        if (request.request_id() == 0 || !valid_operation || !valid_payload) {
            SdkLog("WARN", "invalid app config request",
                   "request_id=", request.request_id(),
                   "operation=", static_cast<int>(request.operation()),
                   "bytes=", request.config_json().size());
            return;
        }
        SdkLog("INFO", "app config request",
               "request_id=", request.request_id(),
               "operation=", static_cast<int>(request.operation()),
               "bytes=", request.config_json().size());
        if (!ReportAppConfigRequest(request)) {
            auto response_packet = MakePackage(
                kavio::ipc::proto::MSG_NTF_APP_CONFIG_RESPONSE,
                request.request_id());
            auto* response = response_packet.mutable_payload()
                ->mutable_ntf_app_config_response();
            response->set_request_id(request.request_id());
            response->set_operation(request.operation());
            response->set_error_code(static_cast<int32_t>(StatusCode::Common_Unsupported));
            response->set_error_message(
                "App did not register on_app_config_request callback");
            const Status status = SendPacket(response_packet);
            if (!status.ok()) {
                SdkLog("ERROR", "automatic app config rejection failed",
                       "request_id=", request.request_id(),
                       "reason=", status.message);
            }
        }
    }

    void HandlePacket(const ProtoPackage& packet) {
        SdkLog("DEBUG", "IPC receive", "msg_type=", static_cast<int>(packet.header().msg_type()),
               "track_id=", packet.header().track_id(),
               "serialized_bytes=", packet.ByteSizeLong());
        switch (packet.header().msg_type()) {
        case kavio::ipc::proto::MSG_ERROR:
            HandleError(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_STATUS_REPLY:
            HandleStatusReply(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_SESSION_CREATED:
            HandleSessionCreated(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_RUNTIME_DATA:
            HandleRuntimeDataPacket(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_USER_CONFIG:
            HandleUserConfigPacket(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_RX_SHM_READY:
            HandleRxShmReady(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_RX_SHM_INVALIDATED:
            HandleRxShmInvalidated(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_TX_SHM_CREATED:
            HandleTxCreated(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_STATS_REPLY:
            HandleStatsReply(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_USB_HOST_ONLINE_STATE:
            HandleUSBHostOnlineState(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_COMM_PEER_STATE:
            HandleCommPeerState(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_COMM_CHANNEL_DATA:
            HandleCommChannelDataPacket(packet);
            break;
        case kavio::ipc::proto::MSG_NTF_APP_CONFIG_REQUEST:
            HandleAppConfigRequestPacket(packet);
            break;
        default:
            break;
        }
    }

    void RecvLoop() {
        g_sdk_callback_owner = this;
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
        g_sdk_callback_owner = nullptr;
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
        auto req = MakePackage(kavio::ipc::proto::MSG_REQ_SESSION_CREATE, create_track_id);
        auto* body = req.mutable_payload()->mutable_req_session_create();
        body->set_client_name(ResolveClientName(session_options));
        body->set_client_pid(ResolveClientPid(session_options));
        auto* sdk_version = body->mutable_sdk_version();
        sdk_version->set_major(KAVIO_SDK_VERSION_MAJOR);
        sdk_version->set_minor(KAVIO_SDK_VERSION_MINOR);
        sdk_version->set_patch(KAVIO_SDK_VERSION_PATCH);
        auto* rx = body->mutable_rx_params();
        rx->set_enabled(rx_params.enabled);
        rx->set_frame_sample_interval(rx_params.frame_sample_interval);
        auto* tx = body->mutable_tx_params();
        tx->set_enabled(tx_params.enabled);
        tx->set_z_order(tx_params.z_order);

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
        FailAllPendingRequests(Status::Error(StatusCode::Common_Cancelled, "client stopped"));
        ReportRuntimeDataChanged(runtime_snapshot);
        ReportRxChanged(prev_rx, next_rx);
        if (state_changed) {
            ReportClientInvalid();
        }
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
    return owner && owner->IsTxSurfaceCurrent(impl_->info.id, impl_->shm) && impl_->shm->IsValid();
}

uint32_t TxSurface::id() const {
    return impl_ ? impl_->info.id : 0;
}

uint32_t TxSurface::width() const {
    return impl_ ? impl_->info.width : 0;
}

uint32_t TxSurface::height() const {
    return impl_ ? impl_->info.height : 0;
}

uint32_t TxSurface::stride() const {
    return impl_ ? impl_->info.stride : 0;
}

PixelFormat TxSurface::pixel_format() const {
    return impl_ ? impl_->info.pixel_format : PixelFormat::kUnknown;
}

Status TxSurface::WriteFrame(const ImageFrame& frame) {
    if (!IsValid()) {
        return Status::Error(StatusCode::Common_NotConnected, "tx surface is not valid");
    }

    if (frame.width != impl_->info.width ||
        frame.height != impl_->info.height ||
        frame.stride != impl_->info.stride ||
        frame.pixel_format != impl_->info.pixel_format) {
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

Status TxSurface::Destroy(std::chrono::milliseconds timeout) {
    if (!impl_ || !impl_->shm) {
        return Status::Error(StatusCode::Common_InvalidArgument, "tx surface is not valid");
    }

    auto owner = impl_->owner.lock();
    if (!owner) {
        return Status::Error(StatusCode::Common_NotConnected, "tx surface owner is not available");
    }

    const uint32_t shm_id = impl_->info.id;
    Status st = owner->DestroyTxSurface(shm_id, impl_->shm, timeout);
    if (!st.ok()) {
        return st;
    }

    impl_->shm.reset();
    impl_->owner.reset();
    impl_->info = {};
    return Status::Ok();
}

KavioClient::KavioClient() : impl_(std::make_shared<Impl>()) {}

KavioClient::~KavioClient() {
    Stop();
    impl_->JoinWorkerThread();
}

Status KavioClient::Start(const SessionOptions& options, Callbacks callbacks) {
    if (options.rx_params.frame_sample_interval == 0) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "frame_sample_interval must be >= 1");
    }

    SdkLog("INFO", "Start requested", "client_name=", ResolveClientName(options),
           "pid=", ResolveClientPid(options), "rx_enabled=", options.rx_params.enabled,
           "tx_enabled=", options.tx_params.enabled, "socket=", DefaultIpcSocketPath());
    {
        std::lock_guard<std::mutex> lifecycle_lk(impl_->lifecycle_mutex);
        if (impl_->worker_running.load(std::memory_order_acquire)) {
            return Status::Error(StatusCode::Common_AlreadyExists, "client is already running or stopping");
        }
    }

    if (impl_->run_thread.joinable()) {
        impl_->run_thread.join();
    }

    {
        std::lock_guard<std::mutex> lifecycle_lk(impl_->lifecycle_mutex);
        std::lock_guard<std::mutex> session_lk(impl_->session_mutex);
        impl_->session_options = options;
        impl_->callbacks = std::move(callbacks);
        impl_->rx_params = options.rx_params;
        impl_->tx_params = options.tx_params;
        impl_->stop_requested.store(false, std::memory_order_release);
        impl_->connected.store(false, std::memory_order_release);
        impl_->transport_connected.store(false, std::memory_order_release);
        impl_->worker_running.store(true, std::memory_order_release);
        impl_->session_create_track_id.store(0, std::memory_order_release);
        impl_->next_track_id.store(1, std::memory_order_relaxed);
        impl_->ResetSessionDataLocked();
        impl_->client_state = ClientState::kInvalid;
    }

    impl_->run_thread = std::thread([impl = impl_] { impl->RunLoop(); });
    return Status::Ok();
}

void KavioClient::Stop() {
    SdkLog("INFO", "Stop requested");
    bool state_changed = false;
    RuntimeData runtime_snapshot;
    RxInfo prev_rx;
    RxInfo next_rx;
    {
        std::lock_guard<std::mutex> lk(impl_->session_mutex);
        prev_rx = impl_->BuildRxInfoLocked();
        impl_->stop_requested.store(true, std::memory_order_release);
        impl_->connected.store(false, std::memory_order_release);
        impl_->transport_connected.store(false, std::memory_order_release);
        impl_->ResetSessionDataLocked();
        next_rx = impl_->BuildRxInfoLocked();
        state_changed = impl_->SetClientStateLocked(ClientState::kInvalid);
        runtime_snapshot = impl_->runtime_data;
    }

    impl_->FailAllPendingRequests(Status::Error(StatusCode::Common_Cancelled, "client stopped"));

    {
        std::lock_guard<std::mutex> lk(impl_->socket_mutex);
        if (impl_->socket) {
            impl_->socket->Disconnect();
        }
    }

    impl_->ReportRuntimeDataChanged(runtime_snapshot);
    impl_->ReportRxChanged(prev_rx, next_rx);
    if (state_changed) {
        impl_->ReportClientInvalid();
    }
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

KavioClient::UserConfig KavioClient::GetUserConfig() const {
    return impl_->SnapshotUserConfig();
}

RxInfo KavioClient::GetRxInfo() const {
    return impl_->SnapshotRxInfo();
}

Status KavioClient::SetRxParams(const RxParams& params,
                                std::chrono::milliseconds timeout) {
    if (params.frame_sample_interval == 0) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "frame_sample_interval must be >= 1");
    }

    Status st = impl_->EnsureBlockingAllowed();
    if (!st.ok()) {
        return st;
    }
    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto pending = std::make_shared<PendingRequest<Status>>();
    pending->track_id = track_id;
    pending->kind = PendingKind::kSetRxParams;
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    auto future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(impl_->pending_mutex);
        impl_->pending_status_requests[track_id] = pending;
    }

    auto req = MakePackage(kavio::ipc::proto::MSG_REQ_SET_RX_PARAMS, track_id);
    auto* body = req.mutable_payload()->mutable_req_set_rx_params()->mutable_params();
    body->set_enabled(params.enabled);
    body->set_frame_sample_interval(params.frame_sample_interval);
    st = impl_->SendPacket(req);
    if (!st.ok()) {
        impl_->CancelPendingStatus(track_id, st);
        return st;
    }

    if (future.wait_for(timeout) != std::future_status::ready) {
        const Status timeout_status = Status::Error(StatusCode::Common_Timeout,
                                                    "set rx params timeout");
        impl_->CancelPendingStatus(track_id, timeout_status);
        return timeout_status;
    }

    Status result = future.get();
    if (result.ok()) {
        RxInfo prev_rx;
        RxInfo next_rx;
        {
            std::lock_guard<std::mutex> lk(impl_->session_mutex);
            prev_rx = impl_->BuildRxInfoLocked();
            impl_->rx_params = params;
            impl_->session_options.rx_params = params;
            if (!params.enabled) {
                impl_->rx_shm.reset();
                impl_->session_info.rx_shm = {};
                impl_->rx_frame_ready = false;
            }
            next_rx = impl_->BuildRxInfoLocked();
        }
        impl_->ReportRxChanged(prev_rx, next_rx);
    }
    return result;
}

Status KavioClient::SetTxParams(const TxParams& params,
                                std::chrono::milliseconds timeout) {
    Status st = impl_->EnsureBlockingAllowed();
    if (!st.ok()) {
        return st;
    }
    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto pending = std::make_shared<PendingRequest<Status>>();
    pending->track_id = track_id;
    pending->kind = PendingKind::kSetTxParams;
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    auto future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(impl_->pending_mutex);
        impl_->pending_status_requests[track_id] = pending;
    }

    auto req = MakePackage(kavio::ipc::proto::MSG_REQ_SET_TX_PARAMS, track_id);
    auto* body = req.mutable_payload()->mutable_req_set_tx_params()->mutable_params();
    body->set_enabled(params.enabled);
    body->set_z_order(params.z_order);
    st = impl_->SendPacket(req);
    if (!st.ok()) {
        impl_->CancelPendingStatus(track_id, st);
        return st;
    }

    if (future.wait_for(timeout) != std::future_status::ready) {
        const Status timeout_status = Status::Error(StatusCode::Common_Timeout,
                                                    "set tx params timeout");
        impl_->CancelPendingStatus(track_id, timeout_status);
        return timeout_status;
    }

    Status result = future.get();
    if (result.ok()) {
        std::lock_guard<std::mutex> lk(impl_->session_mutex);
        impl_->tx_params = params;
        impl_->session_options.tx_params = params;
    }
    return result;
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

Status KavioClient::CreateTxSurface(uint32_t width,
                                    uint32_t height,
                                    TxSurface& out_surface,
                                    std::chrono::milliseconds timeout) {
    if (width == 0 || height == 0) {
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid tx surface size");
    }

    Status st = impl_->EnsureBlockingAllowed();
    if (!st.ok()) {
        return st;
    }

    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto pending = std::make_shared<PendingRequest<TxSurfaceResult>>();
    pending->track_id = track_id;
    pending->kind = PendingKind::kCreateTxSurface;
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    auto future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(impl_->pending_mutex);
        impl_->pending_tx_create_requests[track_id] = pending;
    }

    auto req = MakePackage(kavio::ipc::proto::MSG_REQ_CREATE_TX_SHM, track_id);
    auto* body = req.mutable_payload()->mutable_req_create_tx_shm();
    body->set_width(width);
    body->set_height(height);
    body->set_pixel_format(kavio::ipc::proto::PIXEL_FORMAT_BGRA8888);

    st = impl_->SendPacket(req);
    if (!st.ok()) {
        impl_->CancelPendingTxCreate(track_id, st);
        return st;
    }

    if (future.wait_for(timeout) != std::future_status::ready) {
        const Status timeout_status = Status::Error(StatusCode::Common_Timeout,
                                                    "create tx surface timeout");
        impl_->CancelPendingTxCreate(track_id, timeout_status);
        return timeout_status;
    }

    TxSurfaceResult result = future.get();
    if (!result.status.ok()) {
        return result.status;
    }

    if (!out_surface.impl_) {
        out_surface.impl_ = std::make_unique<TxSurface::Impl>();
    }
    out_surface.impl_->info = result.info;
    out_surface.impl_->shm = result.shm;
    out_surface.impl_->owner = impl_;
    return Status::Ok();
}

Status KavioClient::CloseSession(uint32_t session_id,
                                 std::chrono::milliseconds timeout) {
    if (session_id == 0) {
        return Status::Error(StatusCode::Common_InvalidArgument, "session_id must be > 0");
    }

    Status st = impl_->EnsureBlockingAllowed();
    if (!st.ok()) {
        return st;
    }

    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto pending = std::make_shared<PendingRequest<Status>>();
    pending->track_id = track_id;
    pending->kind = PendingKind::kCloseSession;
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    auto future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(impl_->pending_mutex);
        impl_->pending_status_requests[track_id] = pending;
    }

    auto req = MakePackage(kavio::ipc::proto::MSG_REQ_CLOSE_SESSION, track_id);
    req.mutable_payload()->mutable_req_close_session()->set_session_id(session_id);
    st = impl_->SendPacket(req);
    if (!st.ok()) {
        impl_->CancelPendingStatus(track_id, st);
        return st;
    }

    if (future.wait_for(timeout) != std::future_status::ready) {
        const Status timeout_status = Status::Error(StatusCode::Common_Timeout,
                                                    "close session timeout");
        impl_->CancelPendingStatus(track_id, timeout_status);
        return timeout_status;
    }

    return future.get();
}

Status KavioClient::QueryStats(StatsReply& out_stats,
                               std::chrono::milliseconds timeout) {
    Status st = impl_->EnsureBlockingAllowed();
    if (!st.ok()) {
        return st;
    }

    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto pending = std::make_shared<PendingRequest<StatsReplyResult>>();
    pending->track_id = track_id;
    pending->kind = PendingKind::kQueryStats;
    pending->deadline = std::chrono::steady_clock::now() + timeout;
    auto future = pending->promise.get_future();
    {
        std::lock_guard<std::mutex> lk(impl_->pending_mutex);
        impl_->pending_stats_requests[track_id] = pending;
    }

    auto req = MakePackage(kavio::ipc::proto::MSG_REQ_QUERY_STATS, track_id);
    req.mutable_payload()->mutable_req_query_stats()->set_all_sessions(false);
    st = impl_->SendPacket(req);
    if (!st.ok()) {
        impl_->CancelPendingStats(track_id, st);
        return st;
    }

    if (future.wait_for(timeout) != std::future_status::ready) {
        const Status timeout_status = Status::Error(StatusCode::Common_Timeout,
                                                    "query stats timeout");
        impl_->CancelPendingStats(track_id, timeout_status);
        return timeout_status;
    }

    StatsReplyResult result = future.get();
    if (!result.status.ok()) {
        return result.status;
    }
    out_stats = result.reply;
    return Status::Ok();
}

Status KavioClient::SendInputEvent(const kavio::input::proto::InputEventPb& event) {
    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }

    const uint64_t track_id = impl_->AllocTrackId();
    auto req = MakePackage(kavio::ipc::proto::MSG_REQ_INPUT_EVENT, track_id);
    req.mutable_payload()->mutable_req_input_event()->CopyFrom(event);
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
    auto packet = MakePackage(kavio::ipc::proto::MSG_NTF_COMM_CHANNEL_DATA, track_id);
    auto* body = packet.mutable_payload()->mutable_ntf_comm_channel_data();
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

Status KavioClient::SendAppConfigResponse(
    const AppConfigResponse& response) {
    if (!impl_->IsSessionReady()) {
        return Status::Error(StatusCode::Common_NotConnected, "client is not ready");
    }
    if (response.request_id() == 0) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "app config response request_id is 0");
    }
    if (response.operation() !=
            kavio::ipc::proto::APP_CONFIG_OPERATION_GET &&
        response.operation() !=
            kavio::ipc::proto::APP_CONFIG_OPERATION_SET) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "invalid app config response operation");
    }
    if (response.config_json().size() > kAppConfigMaxPayloadSize) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "app config JSON exceeds 1 MiB");
    }

    auto packet = MakePackage(
        kavio::ipc::proto::MSG_NTF_APP_CONFIG_RESPONSE,
        response.request_id());
    packet.mutable_payload()->mutable_ntf_app_config_response()->CopyFrom(response);
    const Status status = impl_->SendPacket(packet);
    if (!status.ok()) {
        SdkLog("ERROR", "app config response send failed",
               "request_id=", response.request_id(),
               "operation=", static_cast<int>(response.operation()),
               "reason=", status.message);
    }
    return status;
}


}  // namespace kavio
