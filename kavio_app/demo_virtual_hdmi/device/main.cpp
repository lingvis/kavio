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

constexpr std::size_t kMaxQueuedDatagrams = 1024;
constexpr std::size_t kMaxDatagramsPerLoop = 512;
constexpr std::uint32_t kAdvertisedMaxFps = 60;
constexpr auto kReadyInterval = std::chrono::seconds(1);
constexpr auto kFrameTimeout = std::chrono::seconds(2);
constexpr auto kKeyFrameRequestRetry = std::chrono::milliseconds(200);
constexpr auto kSurfaceRequestTimeout = std::chrono::seconds(2);

std::atomic<bool> g_stop(false);

void OnSignal(int) {
    g_stop.store(true, std::memory_order_release);
}

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

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

bool InitializeTxSurface(kavio::KavioClient& client, kavio::TxSurface& surface) {
    const kavio::Status create = client.CreateTxSurface(
        kFrameWidth, kFrameHeight, surface, kSurfaceRequestTimeout);
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

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::atomic<bool> client_ready(false);
    std::atomic<bool> channel_ready(false);
    std::atomic<std::uint64_t> dropped_datagrams(0);
    DatagramQueue incoming;

    kavio::KavioClient client;
    kavio::SessionOptions options;
    options.client_name = "demo_virtual_hdmi";
    options.rx_params.enabled = false;
    options.tx_params.enabled = true;
    options.tx_params.z_order = 0;

    kavio::KavioClient::Callbacks callbacks;
    callbacks.on_client_ready = [&client_ready] {
        client_ready.store(true, std::memory_order_release);
    };
    callbacks.on_client_invalid = [&client_ready, &channel_ready] {
        client_ready.store(false, std::memory_order_release);
        channel_ready.store(false, std::memory_order_release);
    };
    callbacks.on_comm_channel_ready_changed = [&channel_ready](bool ready) {
        channel_ready.store(ready, std::memory_order_release);
    };
    // SDK worker thread only enqueues complete datagrams. Decoding/CRC remains on
    // the main thread; TxSurface::WriteFrame runs on its own latest-frame thread.
    callbacks.on_comm_channel_data = [&incoming, &dropped_datagrams](
        const std::vector<std::uint8_t>& data) {
        if (!incoming.Push(data)) {
            dropped_datagrams.fetch_add(1, std::memory_order_relaxed);
        }
    };

    const kavio::Status start = client.Start(options, std::move(callbacks));
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
                surface.Destroy(std::chrono::seconds(1));
            }
        }

        if (!is_channel_ready && previous_channel_ready) {
            receiver.ResetStream();
            key_requester.ResetRetry();
            incoming.Clear();
            if (publisher) publisher->Clear();
        }

        if (is_client_ready && !surface.IsValid() && now >= next_surface_retry) {
            if (!InitializeTxSurface(client, surface)) {
                next_surface_retry = now + std::chrono::milliseconds(500);
            } else {
                publisher.reset(new TxFramePublisher(surface));
                publisher->Start();
                receiver.ResetStream();
                key_requester.ResetRetry();
                next_ready_announce = now;
            }
        }

        if (is_channel_ready && surface.IsValid() && now >= next_ready_announce) {
            if (SendAppReady(client)) {
                next_ready_announce = now + kReadyInterval;
            } else {
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
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    if (publisher) {
        publisher->Stop();
        publisher.reset();
    }
    if (surface.IsValid()) surface.Destroy(kSurfaceRequestTimeout);
    client.Stop();
    return 0;
}
