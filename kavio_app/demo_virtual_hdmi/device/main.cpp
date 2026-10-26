/*
 * demo_virtual_hdmi - Device Component
 *
 * Windows Host Component captures the 720x720 VirtualDrivers VDD output and sends
 * a continuous I/P tile stream through the Kavio App communication channel.
 * P frames carry an explicit base_frame_id and never rely on per-frame stop-and-wait.
 * The Device validates dependency + CRC, ACKs reconstructed frames, requests a new
 * I frame whenever synchronization is lost, and publishes only the latest complete
 * frame to TX from a dedicated writer thread.
 */
#include <kavio/KavioClient.h>

#include "FrameReceiver.h"
#include "VirtualHdmiProtocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace kavio_virtual_hdmi;

// 接收队列最多 1024 条完整 Channel 数据报；超过上限计数并丢弃，随后触发重同步。
// 主循环一次最多处理 512 条，给超时、连接状态和 TX 发布留出调度机会。
// 60fps 是通过 AppReady 对 Host 宣告的上限，不是 H.264 VENC 的帧率设置。
// Ready 每秒重发便于 Host 晚接入；帧超时 2s 防止中断传输永久占用 pending 状态。
// 关键帧请求 200ms 退避，防止错误帧引起请求风暴。
constexpr std::size_t kMaxQueuedDatagrams = 1024;
constexpr std::size_t kMaxDatagramsPerLoop = 512;
constexpr std::uint32_t kAdvertisedMaxFps = 60;
constexpr auto kReadyInterval = std::chrono::seconds(1);
constexpr auto kFrameTimeout = std::chrono::seconds(2);
constexpr auto kKeyFrameRequestRetry = std::chrono::milliseconds(200);

std::atomic<bool> g_stop(false);

void OnSignal(int) {
    g_stop.store(true, std::memory_order_release);
}

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

// SDK 通道通知可能比解码快：仅缓存完整数据报，绝不让 IPC 派发线程等待解码/写屏。
class DatagramQueue {
public:
    bool Push(const std::vector<std::uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kMaxQueuedDatagrams) return false;
        queue_.push_back(data);
        return true;
    }

    bool Pop(std::vector<std::uint8_t>& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) return false;
        data = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
    }

private:
    std::mutex mutex_;
    std::deque<std::vector<std::uint8_t>> queue_;
};

template <typename Payload>
std::vector<std::uint8_t> MakeMessage(MessageType type, const Payload& payload) {
    MessageHeader header{};
    header.magic = kProtocolMagic;
    header.version = kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.payload_size = static_cast<std::uint32_t>(sizeof(Payload));

    std::vector<std::uint8_t> data(sizeof(header) + sizeof(payload));
    std::memcpy(data.data(), &header, sizeof(header));
    std::memcpy(data.data() + sizeof(header), &payload, sizeof(payload));
    return data;
}

// 发送当前设备支持的 720x720/BGRA/TileSize/FPS 能力，Host 按此决定上传格式。
bool SendAppReady(kavio::KavioClient& client) {
    AppReadyPayload payload{};
    payload.width = kFrameWidth;
    payload.height = kFrameHeight;
    payload.pixel_format = kPixelFormatBgra8888;
    payload.max_fps = kAdvertisedMaxFps;
    payload.tile_size = kTileSize;

    const std::vector<std::uint8_t> message = MakeMessage(MessageType::AppReady, payload);
    const kavio::Status status = client.SendCommChannelData(message.data(), message.size());
    if (!status.ok()) {
        std::cerr << "APP_READY send failed: " << status.message << std::endl;
        return false;
    }
    return true;
}

// ACK 表示组帧与校验结果；Host 据此确认依赖基线是否可继续用于下一帧。
void SendFrameAck(kavio::KavioClient& client,
                  std::uint64_t frame_id,
                  FrameAckStatus ack_status,
                  std::uint32_t received_tile_count,
                  std::uint32_t elapsed_us) {
    FrameAckPayload payload{};
    payload.frame_id = frame_id;
    payload.status = static_cast<std::uint32_t>(ack_status);
    payload.received_tile_count = received_tile_count;
    payload.elapsed_us = elapsed_us;

    const std::vector<std::uint8_t> message = MakeMessage(MessageType::FrameAck, payload);
    const kavio::Status status = client.SendCommChannelData(message.data(), message.size());
    if (!status.ok()) {
        std::cerr << "FRAME_ACK send failed: " << status.message << std::endl;
    }
}

// 丢失依赖基线时请求 I 帧；每次失败使用重试间隔，不能按每个 Tile 连续请求。
class KeyFrameRequester {
public:
    void ResetRetry() {
        next_request_ = std::chrono::steady_clock::time_point{};
    }

    bool Request(kavio::KavioClient& client,
                 std::uint64_t last_good_frame_id,
                 std::uint64_t failed_frame_id,
                 KeyFrameRequestReason reason,
                 bool force = false) {
        const auto now = std::chrono::steady_clock::now();
        if (!force && now < next_request_) return false;

        KeyFrameRequestPayload payload{};
        payload.last_good_frame_id = last_good_frame_id;
        payload.failed_frame_id = failed_frame_id;
        payload.reason = static_cast<std::uint32_t>(reason);
        const std::vector<std::uint8_t> message =
            MakeMessage(MessageType::KeyFrameRequest, payload);
        const kavio::Status status = client.SendCommChannelData(message.data(), message.size());
        next_request_ = now + kKeyFrameRequestRetry;
        if (!status.ok()) {
            std::cerr << "KEYFRAME_REQUEST send failed: " << status.message << std::endl;
            return false;
        }
        return true;
    }

private:
    std::chrono::steady_clock::time_point next_request_{};
};

// 在 OnMessage 已收到远端创建成功后才打开 SHM 并初始化 TX 画布。
bool InitializeTxSurface(kavio::KavioClient& client, kavio::TxSurface& surface,
                         const kavio::ipc::proto::ShmInfoPb& info) {
    const kavio::Status create = client.OpenTxSurface(info, surface);
    if (!create.ok()) {
        std::cerr << "CreateTxSurface failed: " << create.message << std::endl;
        return false;
    }

    kavio::ImageFrame blank;
    blank.frame_id = 0;
    blank.timestamp_ns = NowNs();
    blank.width = kFrameWidth;
    blank.height = kFrameHeight;
    blank.stride = kFrameStride;
    blank.pixel_format = kavio::PixelFormat::kBGRA8888;
    blank.dest_pos_x = 0;
    blank.dest_pos_y = 0;
    blank.valid_rect.pos_x = 0;
    blank.valid_rect.pos_y = 0;
    blank.valid_rect.width = kFrameWidth;
    blank.valid_rect.height = kFrameHeight;
    blank.data.assign(kFrameBytes, 0);

    const kavio::Status write = surface.WriteFrame(blank);
    if (!write.ok()) {
        std::cerr << "Initial blank TX frame failed: " << write.message << std::endl;
    }

    std::cout << "Virtual HDMI receiver initialized: "
              << kFrameWidth << "x" << kFrameHeight
              << " BGRA, tx_shm_id=" << surface.id() << std::endl;
    return true;
}

// TX is intentionally decoupled from frame assembly. There is no FIFO here:
// if rendering falls behind, the newest complete frame replaces the pending
// frame so latency stays bounded instead of building a stale video backlog.
// 单独发布线程消化完整帧；保留最新有效结果，避免共享内存写入阻塞 Channel 接收。
class TxFramePublisher {
public:
    explicit TxFramePublisher(kavio::TxSurface& surface)
        : surface_(surface) {}

    ~TxFramePublisher() { Stop(); }

    void Start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        thread_ = std::thread(&TxFramePublisher::ThreadMain, this);
    }

    void Stop() {
        const bool was_running = running_.exchange(false);
        if (was_running) {
            condition_.notify_all();
        }
        if (thread_.joinable()) thread_.join();
        Clear();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        has_frame_ = false;
        latest_ = kavio::ImageFrame{};
    }

    void Submit(const std::vector<std::uint8_t>& pixels,
                const CompletedFrame& completed) {
        kavio::ImageFrame frame;
        frame.frame_id = completed.frame_id;
        frame.timestamp_ns = completed.timestamp_ns;
        frame.width = kFrameWidth;
        frame.height = kFrameHeight;
        frame.stride = kFrameStride;
        frame.pixel_format = kavio::PixelFormat::kBGRA8888;
        frame.dest_pos_x = 0;
        frame.dest_pos_y = 0;
        frame.valid_rect.pos_x = 0;
        frame.valid_rect.pos_y = 0;
        frame.valid_rect.width = kFrameWidth;
        frame.valid_rect.height = kFrameHeight;
        frame.data = pixels;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (has_frame_) {
                dropped_frames_.fetch_add(1, std::memory_order_relaxed);
            }
            latest_ = std::move(frame);
            has_frame_ = true;
        }
        condition_.notify_one();
    }

    std::uint64_t DroppedFrames() const {
        return dropped_frames_.load(std::memory_order_relaxed);
    }

private:
    void ThreadMain() {
        while (running_.load(std::memory_order_acquire)) {
            kavio::ImageFrame frame;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] {
                    return !running_.load(std::memory_order_acquire) || has_frame_;
                });
                if (!running_.load(std::memory_order_acquire)) break;
                frame = std::move(latest_);
                latest_ = kavio::ImageFrame{};
                has_frame_ = false;
            }

            const kavio::Status status = surface_.WriteFrame(frame);
            if (!status.ok()) {
                std::cerr << "TX frame write failed: frame=" << frame.frame_id
                          << " error=" << status.message << std::endl;
            }
        }
    }

    kavio::TxSurface& surface_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool has_frame_ = false;
    kavio::ImageFrame latest_;
};

// 任何 Tile/CRC/依赖错误都显式反馈并请求恢复，不能直接把不完整 P 帧显示出去。
void HandleAssemblyError(kavio::KavioClient& client,
                         FrameReceiver& receiver,
                         KeyFrameRequester& key_requester,
                         const FrameAssemblyError& error) {
    const std::uint64_t pending_frame_id = receiver.PendingFrameId();
    const std::uint64_t failed_frame_id =
        error.failed_frame_id != 0 ? error.failed_frame_id : pending_frame_id;
    std::cerr << "Frame assembly failed: frame=" << failed_frame_id
              << " error=" << error.message << std::endl;
    if (failed_frame_id != 0) {
        SendFrameAck(client, failed_frame_id, error.status, 0, 0);
    }
    const std::uint64_t last_good = receiver.LastGoodFrameId();
    receiver.EnterResync();
    key_requester.Request(client, last_good, failed_frame_id,
                          error.key_frame_reason, false);
}

// 按业务 MessageType 分发 Host 数据报；I/P 增量组装由 FrameReceiver 负责。
// 只解析 Device 端自定义 FrameBegin/Tile/End，非标准 H.264 NAL 数据。
void HandleDatagram(kavio::KavioClient& client,
                    TxFramePublisher& publisher,
                    FrameReceiver& receiver,
                    KeyFrameRequester& key_requester,
                    const std::vector<std::uint8_t>& data) {
    if (data.size() < sizeof(MessageHeader)) {
        std::cerr << "Invalid Host Component message: datagram is smaller than header"
                  << std::endl;
        return;
    }
    if (data.size() > kMaxDatagramBytes) {
        std::cerr << "Invalid Host Component message: datagram exceeds protocol maximum"
                  << std::endl;
        return;
    }

    MessageHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.magic != kProtocolMagic) {
        std::cerr << "Invalid Host Component message: invalid protocol magic" << std::endl;
        return;
    }
    if (header.version != kProtocolVersion) {
        std::cerr << "Invalid Host Component message: unsupported protocol version="
                  << header.version << std::endl;
        return;
    }
    if (header.payload_size != data.size() - sizeof(MessageHeader)) {
        std::cerr << "Invalid Host Component message: payload size mismatch" << std::endl;
        return;
    }

    const std::uint8_t* payload = data.data() + sizeof(MessageHeader);
    const std::size_t payload_size = data.size() - sizeof(MessageHeader);
    const MessageType type = static_cast<MessageType>(header.type);
    FrameAssemblyError error;

    switch (type) {
    case MessageType::FrameBegin: {
        if (payload_size != sizeof(FrameBeginPayload)) {
            std::cerr << "Invalid Host Component message: invalid FRAME_BEGIN size"
                      << std::endl;
            return;
        }
        FrameBeginPayload begin{};
        std::memcpy(&begin, payload, sizeof(begin));

        // A new frame boundary lets us abandon a damaged/incomplete previous frame.
        // If the new frame is an I frame it can immediately recover the stream;
        // otherwise Begin() will reject its broken dependency and request an I frame.
        if (receiver.HasPendingFrame()) {
            const std::uint64_t interrupted = receiver.PendingFrameId();
            std::cerr << "Frame interrupted by new FRAME_BEGIN: old=" << interrupted
                      << " new=" << begin.frame_id << std::endl;
            SendFrameAck(client, interrupted, FrameAckStatus::InvalidFrame, 0, 0);
            receiver.EnterResync();
        }

        if (!receiver.Begin(begin, error)) {
            HandleAssemblyError(client, receiver, key_requester, error);
        }
        return;
    }
    case MessageType::Tile: {
        // Once a P dependency is broken, the rest of that frame (and later P
        // frames) may already be queued. Drop payload datagrams quietly until a
        // valid I FRAME_BEGIN starts a new assembly instead of emitting one
        // negative ACK per tile.
        if (receiver.NeedsKeyFrame() && !receiver.HasPendingFrame()) return;
        if (payload_size < sizeof(TilePayloadHeader)) {
            std::cerr << "Invalid Host Component message: invalid TILE size" << std::endl;
            return;
        }
        TilePayloadHeader tile{};
        std::memcpy(&tile, payload, sizeof(tile));
        const std::size_t encoded_size = payload_size - sizeof(TilePayloadHeader);
        if (tile.encoded_size != encoded_size) {
            std::cerr << "Invalid Host Component message: TILE encoded size mismatch"
                      << std::endl;
            return;
        }
        if (encoded_size > kMaxTilePayloadBytes) {
            std::cerr << "Invalid Host Component message: TILE payload too large"
                      << std::endl;
            return;
        }
        const std::uint8_t* encoded = payload + sizeof(TilePayloadHeader);
        if (!receiver.ApplyTile(tile, encoded, encoded_size, error)) {
            HandleAssemblyError(client, receiver, key_requester, error);
        }
        return;
    }
    case MessageType::FrameEnd: {
        if (receiver.NeedsKeyFrame() && !receiver.HasPendingFrame()) return;
        if (payload_size != sizeof(FrameEndPayload)) {
            std::cerr << "Invalid Host Component message: invalid FRAME_END size"
                      << std::endl;
            return;
        }
        FrameEndPayload end{};
        std::memcpy(&end, payload, sizeof(end));
        CompletedFrame completed;
        if (!receiver.End(end, completed, error)) {
            HandleAssemblyError(client, receiver, key_requester, error);
            return;
        }

        // ACK confirms that dependency validation, tile reconstruction and CRC are complete.
        // TX rendering is asynchronous and is not part of Host flow-control completion.
        SendFrameAck(client,
                     completed.frame_id,
                     FrameAckStatus::Ok,
                     completed.received_tile_count,
                     completed.elapsed_us);
        publisher.Submit(receiver.Pixels(), completed);
        if (completed.frame_type == FrameType::IFrame) {
            key_requester.ResetRetry();
        }
        return;
    }
    case MessageType::AppReady:
    case MessageType::FrameAck:
    case MessageType::KeyFrameRequest:
        // Device -> Host messages are ignored if echoed back by the channel.
        return;
    default:
        std::cerr << "Invalid Host Component message: unknown type="
                  << header.type << std::endl;
        return;
    }
}

// OnMessage 只接收 Channel 数据报、更新状态，以及在 TX 创建回复中打开 SHM。
// 数据解码由主循环消费 DatagramQueue；TX 对象跨线程交接使用 ready_tx_mutex。
class AppMessageHandler final : public kavio::IClientMessageHandler {
public:
    explicit AppMessageHandler(kavio::KavioClient& client, std::mutex& ready_tx_mutex, std::unique_ptr<kavio::TxSurface>& ready_tx, std::atomic<bool>& tx_create_pending, std::atomic<bool>& client_ready, std::atomic<bool>& channel_ready, DatagramQueue& incoming, std::atomic<std::uint64_t>& dropped_datagrams) : client(client), ready_tx_mutex(ready_tx_mutex), ready_tx(ready_tx), tx_create_pending(tx_create_pending), client_ready(client_ready), channel_ready(channel_ready), incoming(incoming), dropped_datagrams(dropped_datagrams) {}

    void OnDisconnected() override {
        client_ready.store(false, std::memory_order_release);
        channel_ready.store(false, std::memory_order_release);
        tx_create_pending.store(false);
        std::lock_guard<std::mutex> lock(ready_tx_mutex);
        ready_tx.reset();
    }

    void OnMessage(const kavio::ipc::proto::IPCPackagePb& msg) override {
        namespace ip = kavio::ipc::proto;
        if (msg.header().msg_type() == ip::MSG_P2A_RSP_CREATE_TX_SHM &&
            msg.payload().has_p2a_rsp_create_tx_shm()) {
            const auto& info = msg.payload().p2a_rsp_create_tx_shm().tx_shm();
            auto opened = std::make_unique<kavio::TxSurface>();
            if (InitializeTxSurface(client, *opened, info)) {
                std::lock_guard<std::mutex> lock(ready_tx_mutex);
                ready_tx = std::move(opened);
            } else {
                uint64_t request_id = 0;
                client.DestroyTxSurface(info.id(), request_id);
            }
            tx_create_pending.store(false);
        } else if (msg.header().msg_type() == ip::MSG_P2A_RSP_ERROR && msg.payload().has_p2a_rsp_error() &&
                   msg.payload().p2a_rsp_error().failed_for() == ip::MSG_A2P_REQ_CREATE_TX_SHM) {
            std::cerr << "CreateTxSurface: " << msg.payload().p2a_rsp_error().message() << std::endl;
            tx_create_pending.store(false);
        }
        switch (msg.header().msg_type()) {
        case ip::MSG_P2A_RSP_CREATE_SESSION:
            client_ready.store(true, std::memory_order_release);
            channel_ready.store(client.IsCommChannelReady(), std::memory_order_release);
            break;
        case ip::MSG_P2A_NTF_USB_HOST_STATE:
        case ip::MSG_P2A_NTF_COMM_PEER_STATE:
            channel_ready.store(client.IsCommChannelReady(), std::memory_order_release); break;
        case ip::MSG_P2A_DATA_COMM_CHANNEL:
            if (msg.payload().has_p2a_data_comm_channel()) {
                const auto& p = msg.payload().p2a_data_comm_channel().payload();
                if (!incoming.Push(std::vector<uint8_t>(p.begin(), p.end())))
                    dropped_datagrams.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        default: break;
        }
    }
private:
    kavio::KavioClient& client;
    std::mutex& ready_tx_mutex;
    std::unique_ptr<kavio::TxSurface>& ready_tx;
    std::atomic<bool>& tx_create_pending;
    std::atomic<bool>& client_ready;
    std::atomic<bool>& channel_ready;
    DatagramQueue& incoming;
    std::atomic<std::uint64_t>& dropped_datagrams;
};
} // namespace

// 主循环依次处理 Channel 队列、组帧超时、TX 申请和发布；避免在 OnMessage 执行繁重组帧。
int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::atomic<bool> client_ready(false);
    std::atomic<bool> channel_ready(false);
    std::atomic<std::uint64_t> dropped_datagrams(0);
    DatagramQueue incoming;

    std::mutex ready_tx_mutex;
    std::unique_ptr<kavio::TxSurface> ready_tx;
    std::atomic<bool> tx_create_pending{false};
    std::unique_ptr<AppMessageHandler> handler;
    kavio::KavioClient client;
    handler = std::make_unique<AppMessageHandler>(client, ready_tx_mutex, ready_tx,
                                                  tx_create_pending, client_ready,
                                                  channel_ready, incoming, dropped_datagrams);
    kavio::ipc::proto::A2PReqCreateSessionPb options;
    options.set_client_name("demo_virtual_hdmi");
    // 虚拟副屏的画面来自 USB Host Component，而非 HDMI RX，故关闭 RX 订阅。
    // TX 使用 z_order=0 作为示例默认层级，720x720 图像大小由共用协议固定。
    options.mutable_rx_params()->set_enabled(false);
    options.mutable_rx_params()->set_frame_sample_interval(1);
    options.mutable_tx_params()->set_enabled(true);
    options.mutable_tx_params()->set_z_order(0);


    const kavio::Status start = client.Start(options, *handler);
    if (!start.ok()) {
        std::cerr << "Start KavioClient failed: " << start.message << std::endl;
        return 1;
    }

    kavio::TxSurface surface;
    std::unique_ptr<TxFramePublisher> publisher;
    FrameReceiver receiver;
    KeyFrameRequester key_requester;
    bool previous_client_ready = false;
    bool previous_channel_ready = false;
    auto next_surface_retry = std::chrono::steady_clock::now();
    auto next_ready_announce = std::chrono::steady_clock::now();

    while (!g_stop.load(std::memory_order_acquire)) {
        const bool is_client_ready = client_ready.load(std::memory_order_acquire);
        const bool is_channel_ready = channel_ready.load(std::memory_order_acquire);
        const auto now = std::chrono::steady_clock::now();

        if (!is_client_ready && previous_client_ready) {
            receiver.ResetStream();
            key_requester.ResetRetry();
            incoming.Clear();
            if (publisher) {
                publisher->Stop();
                publisher.reset();
            }
            if (surface.IsValid()) {
                { uint64_t id = 0; surface.Destroy(id); }
            }
        }

        if (!is_channel_ready && previous_channel_ready) {
            receiver.ResetStream();
            key_requester.ResetRetry();
            incoming.Clear();
            if (publisher) publisher->Clear();
        }

        std::unique_ptr<kavio::TxSurface> opened;
        {
            std::lock_guard<std::mutex> lock(ready_tx_mutex);
            opened = std::move(ready_tx);
        }
        if (is_client_ready && opened) {
            surface = std::move(*opened);
            publisher.reset(new TxFramePublisher(surface));
            publisher->Start();
            receiver.ResetStream();
            key_requester.ResetRetry();
            next_ready_announce = now;
        }
        if (is_client_ready && !tx_create_pending.load() &&
            !surface.IsValid() && now >= next_surface_retry) {
            tx_create_pending.store(true);
            uint64_t request_id = 0;
            auto st = client.CreateTxSurface(kFrameWidth, kFrameHeight, request_id);
            if (!st.ok()) {
                tx_create_pending.store(false);
                // TX 申请失败后延迟 500ms 再尝试，防止异常资源状态下忙循环。
                next_surface_retry = now + std::chrono::milliseconds(500);
            }
        }

        if (is_channel_ready && surface.IsValid() && now >= next_ready_announce) {
            if (SendAppReady(client)) {
                next_ready_announce = now + kReadyInterval;
            } else {
                // 图层首次准备阶段缩短 Ready 广播间隔，让 Host 尽早开始推送。
                next_ready_announce = now + std::chrono::milliseconds(250);
            }
        }

        const std::uint64_t dropped = dropped_datagrams.exchange(0, std::memory_order_relaxed);
        if (dropped != 0) {
            std::cerr << "Incoming Host Component queue is full; dropped "
                      << dropped << " datagram(s); requesting a new I frame" << std::endl;
            incoming.Clear();
            const std::uint64_t last_good = receiver.LastGoodFrameId();
            const std::uint64_t failed = receiver.PendingFrameId();
            receiver.EnterResync();
            if (is_channel_ready) {
                key_requester.Request(client, last_good, failed,
                    KeyFrameRequestReason::QueueOverflow, true);
            }
        }

        std::size_t processed = 0;
        std::vector<std::uint8_t> datagram;
        while (publisher && surface.IsValid() && is_channel_ready &&
               processed < kMaxDatagramsPerLoop && incoming.Pop(datagram)) {
            HandleDatagram(client, *publisher, receiver, key_requester, datagram);
            ++processed;
        }

        if (receiver.HasPendingFrame() && receiver.PendingAgeSeconds() >=
            std::chrono::duration<double>(kFrameTimeout).count()) {
            const std::uint64_t frame_id = receiver.PendingFrameId();
            const std::uint64_t last_good = receiver.LastGoodFrameId();
            std::cerr << "Frame timed out: frame=" << frame_id
                      << "; requesting a new I frame" << std::endl;
            SendFrameAck(client, frame_id, FrameAckStatus::Timeout, 0, 0);
            receiver.EnterResync();
            key_requester.Request(client, last_good, frame_id,
                                  KeyFrameRequestReason::FrameTimeout, true);
        }

        // Request retry is independent of incoming P traffic. Once synchronization
        // is lost, a lost KEYFRAME_REQUEST cannot leave the Device stuck forever.
        if (is_channel_ready && surface.IsValid() && receiver.NeedsKeyFrame()) {
            key_requester.Request(client,
                                  receiver.LastGoodFrameId(),
                                  receiver.PendingFrameId(),
                                  KeyFrameRequestReason::StreamReset,
                                  false);
        }

        previous_client_ready = is_client_ready;
        previous_channel_ready = is_channel_ready;

        // If data is already queued, keep draining it immediately. Sleeping only
        // while idle avoids adding 5 ms gaps inside I frames with 100+ tiles.
        if (processed == 0) {
            // 2ms 是无事件时的主循环休眠，不等同于视频输出或编码间隔。
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (publisher) {
        publisher->Stop();
        publisher.reset();
    }
    if (surface.IsValid()) { uint64_t id = 0; surface.Destroy(id); }
    client.Stop();
    return 0;
}
