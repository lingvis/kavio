/*
 * demo_panel_image - Device Component
 *
 * The Windows App Panel decodes a dropped PNG/GIF into BGRA frames. This Device Component
 * receives the frames, creates a TX surface with the original image dimensions,
 * preserves the alpha channel, and applies position updates sent while dragging.
 */
#include <kavio/KavioClient.h>
#include "DemoImagePanelProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// GIF 每帧保留原始 BGRA 像素和延迟。缺省 100ms 只用于发送者未提供正延迟的容错。
// 不提前将 GIF 合成为视频：面板继续按每帧 delay_ms 切换。
struct ImageFrameData {
    uint32_t delay_ms = 100;
    uint32_t received = 0;
    std::vector<uint8_t> pixels;
};

// g_pending 保存正在接收但尚未完整的图片；g_active 只指向已经通过完整性检查的图片。
struct ImageData {
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t x = 0;
    int32_t y = 0;
    std::vector<ImageFrameData> frames;
};

std::atomic<bool> g_stop(false);
std::atomic<bool> g_client_ready(false);
std::atomic<bool> g_channel_ready(false);
// SDK OnMessage 派发线程与主渲染线程共享 pending/active 图片，状态交换须持锁。
std::mutex g_mutex;
ImageData g_pending;
std::shared_ptr<const ImageData> g_active;
int32_t g_active_x = 0;
int32_t g_active_y = 0;
uint64_t g_pending_transfer = 0;
bool g_pending_valid = false;
uint64_t g_image_generation = 0;
uint64_t g_position_generation = 0;

void OnSignal(int) { g_stop.store(true); }

// 在任何 vector 分配之前先做尺寸/总内存上限检查，避免 Host 端异常输入造成过大分配。
// 总量按 width*height*4*frames 预检：4 对应 BGRA8888，每帧完整 RGBA 像素。
bool ValidDimensions(uint32_t width, uint32_t height, uint32_t frames) {
    if (width == 0 || height == 0 || width > demo_image_panel::kMaxWidth ||
        height > demo_image_panel::kMaxHeight || frames == 0 ||
        frames > demo_image_panel::kMaxFrames) return false;
    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4ull * frames;
    return bytes <= demo_image_panel::kMaxDecodedBytes;
}

// Channel 回调可能来自 SDK 工作线程；这里只更新受 mutex 保护的接收状态，不直接操作 TX Surface。
// 消息接收端只组装内存对象；必须收到 ImageEnd 且每帧完整后才原子替换当前图片。
void HandleMessage(const std::vector<uint8_t>& data) {
    if (data.size() < sizeof(demo_image_panel::MessageHeader)) return;
    demo_image_panel::MessageHeader header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (header.magic != demo_image_panel::kMagic ||
        header.version != demo_image_panel::kVersion ||
        header.size != data.size()) return;

    std::lock_guard<std::mutex> lock(g_mutex);
    const auto type = static_cast<demo_image_panel::MessageType>(header.type);
    if (type == demo_image_panel::MessageType::ImageBegin) {
        if (data.size() != sizeof(demo_image_panel::ImageBeginMessage)) return;
        demo_image_panel::ImageBeginMessage message{};
        std::memcpy(&message, data.data(), sizeof(message));
        if (!ValidDimensions(message.width, message.height, message.frame_count)) return;
        g_pending = ImageData{};
        g_pending.width = message.width;
        g_pending.height = message.height;
        g_pending.frames.resize(message.frame_count);
        g_pending_transfer = message.transfer_id;
        g_pending_valid = true;
        return;
    }
    if (type == demo_image_panel::MessageType::FrameBegin) {
        if (!g_pending_valid || data.size() != sizeof(demo_image_panel::FrameBeginMessage)) return;
        demo_image_panel::FrameBeginMessage message{};
        std::memcpy(&message, data.data(), sizeof(message));
        const uint64_t expected = static_cast<uint64_t>(g_pending.width) * g_pending.height * 4ull;
        if (message.transfer_id != g_pending_transfer ||
            message.frame_index >= g_pending.frames.size() ||
            message.data_size != expected) return;
        ImageFrameData& frame = g_pending.frames[message.frame_index];
        frame.delay_ms = message.delay_ms == 0 ? 100 : message.delay_ms;
        frame.received = 0;
        frame.pixels.assign(message.data_size, 0);
        return;
    }
    // 按连续 offset 接收块，不允许乱序写入；错误块丢弃以免污染整幅图像。
    if (type == demo_image_panel::MessageType::FrameChunk) {
        if (!g_pending_valid || data.size() < sizeof(demo_image_panel::FrameChunkMessage)) return;
        demo_image_panel::FrameChunkMessage message{};
        std::memcpy(&message, data.data(), sizeof(message));
        if (message.transfer_id != g_pending_transfer ||
            message.frame_index >= g_pending.frames.size() ||
            data.size() != sizeof(message) + message.data_size) return;
        ImageFrameData& frame = g_pending.frames[message.frame_index];
        if (message.offset != frame.received ||
            static_cast<uint64_t>(message.offset) + message.data_size > frame.pixels.size()) return;
        std::memcpy(frame.pixels.data() + message.offset,
                    data.data() + sizeof(message), message.data_size);
        frame.received += message.data_size;
        return;
    }
    // 只有 ImageEnd 到达且每一帧都完整收齐后，才把 pending 原子切换成 active。
    if (type == demo_image_panel::MessageType::ImageEnd) {
        if (!g_pending_valid || data.size() != sizeof(demo_image_panel::ImageEndMessage)) return;
        demo_image_panel::ImageEndMessage message{};
        std::memcpy(&message, data.data(), sizeof(message));
        if (message.transfer_id != g_pending_transfer) return;
        for (const ImageFrameData& frame : g_pending.frames) {
            if (frame.pixels.empty() || frame.received != frame.pixels.size()) return;
        }
        g_pending.x = g_active_x;
        g_pending.y = g_active_y;
        g_active = std::make_shared<ImageData>(std::move(g_pending));
        g_pending = ImageData{};
        g_pending_valid = false;
        ++g_image_generation;
        return;
    }
    if (type == demo_image_panel::MessageType::Position) {
        if (data.size() != sizeof(demo_image_panel::PositionMessage)) return;
        demo_image_panel::PositionMessage message{};
        std::memcpy(&message, data.data(), sizeof(message));
        g_active_x = std::max<int32_t>(0, message.x);
        g_active_y = std::max<int32_t>(0, message.y);
        ++g_position_generation;
    }
}

// IPC 通知统一从 OnMessage 进入。TX 创建后在这里直接 OpenTxSurface，
// 而不是让主循环等待请求完成；主循环只消费已经可用的图层及图像快照。
class AppMessageHandler final : public kavio::IClientMessageHandler {
public:
    explicit AppMessageHandler(kavio::KavioClient& client, std::shared_ptr<kavio::TxSurface>& surface, std::atomic<bool>& tx_create_pending) : client(client), surface(surface), tx_create_pending(tx_create_pending) {}

    void OnDisconnected() override {
        g_client_ready.store(false);
        g_channel_ready.store(false);
        tx_create_pending.store(false);
        std::atomic_store(&surface, std::shared_ptr<kavio::TxSurface>{});
    }

    void OnMessage(const kavio::ipc::proto::IPCPackagePb& msg) override {
        namespace ip = kavio::ipc::proto;
        if (msg.header().msg_type() == ip::MSG_P2A_RSP_CREATE_TX_SHM &&
            msg.payload().has_p2a_rsp_create_tx_shm()) {
            const auto& info = msg.payload().p2a_rsp_create_tx_shm().tx_shm();
            kavio::TxSurface opened;
            auto st = client.OpenTxSurface(info, opened);
            if (st.ok()) {
                std::atomic_store(&surface,
                    std::make_shared<kavio::TxSurface>(std::move(opened)));
            } else {
                std::cerr << "OpenTxSurface: " << st.message << std::endl;
                uint64_t id = 0; client.DestroyTxSurface(info.id(), id);
            }
            tx_create_pending.store(false);
        } else if (msg.header().msg_type() == ip::MSG_P2A_RSP_ERROR && msg.payload().has_p2a_rsp_error() &&
                   msg.payload().p2a_rsp_error().failed_for() == ip::MSG_A2P_REQ_CREATE_TX_SHM) {
            std::cerr << "CreateTxSurface: " << msg.payload().p2a_rsp_error().message() << std::endl;
            tx_create_pending.store(false);
        }
        switch (msg.header().msg_type()) {
        case ip::MSG_P2A_RSP_CREATE_SESSION:
            g_client_ready.store(true);
            g_channel_ready.store(client.IsCommChannelReady()); break;
        case ip::MSG_P2A_NTF_USB_HOST_STATE:
        case ip::MSG_P2A_NTF_COMM_PEER_STATE:
            g_channel_ready.store(client.IsCommChannelReady()); break;
        case ip::MSG_P2A_DATA_COMM_CHANNEL:
            if (msg.payload().has_p2a_data_comm_channel()) {
                const auto& p = msg.payload().p2a_data_comm_channel().payload();
                HandleMessage(std::vector<uint8_t>(p.begin(), p.end()));
            }
            break;
        default: break;
        }
    }
private:
    kavio::KavioClient& client;
    std::shared_ptr<kavio::TxSurface>& surface;
    std::atomic<bool>& tx_create_pending;
};
}  // namespace

// 轮询主循环只做异步请求提交、动画定时和 TX 写入，不等待远端创建应答。
int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::shared_ptr<kavio::TxSurface> surface;
    std::atomic<bool> tx_create_pending{false};
    std::unique_ptr<AppMessageHandler> handler;
    kavio::KavioClient client;
    handler = std::make_unique<AppMessageHandler>(client, surface, tx_create_pending);
    kavio::ipc::proto::A2PReqCreateSessionPb options;
    options.set_client_name("demo_panel_image");
    // 图片来自 Windows App Panel 的 Channel，不依赖 HDMI RX 帧，故关闭 RX 订阅。
    // TX z_order=100 让演示图片显示在相应叠放层；按实际 App 图层策略调整。
    options.mutable_rx_params()->set_enabled(false);
    options.mutable_rx_params()->set_frame_sample_interval(1);
    options.mutable_tx_params()->set_enabled(true);
    options.mutable_tx_params()->set_z_order(100);


    const kavio::Status start = client.Start(options, *handler);
    if (!start.ok()) {
        std::cerr << "Start KavioClient failed: " << start.message << std::endl;
        return 1;
    }

    uint32_t last_render_surface_id = 0;
    uint64_t rendered_image_generation = static_cast<uint64_t>(-1);
    uint64_t rendered_position_generation = static_cast<uint64_t>(-1);
    size_t frame_index = 0;
    auto next_frame = std::chrono::steady_clock::now();
    auto next_ready_announce = std::chrono::steady_clock::now();
    uint64_t tx_frame_id = 1;

    while (!g_stop.load()) {
        const auto ready_now = std::chrono::steady_clock::now();
        // DeviceReady 是业务层握手：Channel ready 之后持续通告，Host 才知道 Device 已进入接收循环。
        if (g_channel_ready.load() && ready_now >= next_ready_announce) {
            demo_image_panel::DeviceReadyMessage message{};
            demo_image_panel::Initialize(message, demo_image_panel::MessageType::DeviceReady);
            client.SendCommChannelData(&message, sizeof(message));
            // 周期性发送 Ready 用于 Host 晚启动/重连接收，不依赖首次通知恰好到达。
            next_ready_announce = ready_now + std::chrono::seconds(1);
        }
        std::shared_ptr<const ImageData> snapshot;
        int32_t position_x = 0;
        int32_t position_y = 0;
        uint64_t image_generation = 0;
        uint64_t position_generation = 0;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            snapshot = g_active;
            position_x = g_active_x;
            position_y = g_active_y;
            image_generation = g_image_generation;
            position_generation = g_position_generation;
        }

        auto current_surface = std::atomic_load(&surface);
        if (g_client_ready.load() && snapshot && !snapshot->frames.empty() &&
            !tx_create_pending.load() &&
            (!current_surface || !current_surface->IsValid() ||
             current_surface->width() != snapshot->width ||
             current_surface->height() != snapshot->height)) {
            if (current_surface) {
                auto previous = std::atomic_exchange(&surface, std::shared_ptr<kavio::TxSurface>{});
                if (previous) { uint64_t id = 0; previous->Destroy(id); }
                current_surface.reset();
            }
            tx_create_pending.store(true);
            uint64_t request_id = 0;
            // Surface 与解码图片等尺寸，避免无必要的缩放；此处仅异步提交创建请求。
            auto st = client.CreateTxSurface(snapshot->width, snapshot->height, request_id);
            if (!st.ok()) {
                tx_create_pending.store(false);
                std::cerr << "CreateTxSurface: " << st.message << std::endl;
            }
        }
        if (current_surface && current_surface->id() != last_render_surface_id) {
            last_render_surface_id = current_surface->id();
            rendered_image_generation = static_cast<uint64_t>(-1);
            frame_index = 0;
        }

        const auto now = std::chrono::steady_clock::now();
        if (image_generation != rendered_image_generation) {
            frame_index = 0;
            next_frame = now;
        } else if (snapshot && snapshot->frames.size() > 1 && now >= next_frame) {
            frame_index = (frame_index + 1) % snapshot->frames.size();
        }

        // 只有新图片、GIF 到帧时间或位置变化时才写 TX，静态图片不会被 10 ms 主循环反复重写。
        const bool animation_due = (current_surface && current_surface->IsValid()) && snapshot &&
                                   !snapshot->frames.empty() && now >= next_frame;
        const bool position_changed = position_generation != rendered_position_generation;
        if ((current_surface && current_surface->IsValid()) && snapshot && !snapshot->frames.empty() &&
            (animation_due || image_generation != rendered_image_generation || position_changed)) {
            frame_index = std::min(frame_index, snapshot->frames.size() - 1);
            const ImageFrameData& source = snapshot->frames[frame_index];
            kavio::ImageFrame frame;
            frame.frame_id = tx_frame_id++;
            frame.width = snapshot->width;
            frame.height = snapshot->height;
            frame.stride = snapshot->width * 4;
            frame.pixel_format = kavio::PixelFormat::kBGRA8888;
            frame.dest_pos_x = position_x;
            frame.dest_pos_y = position_y;
            frame.valid_rect.width = snapshot->width;
            frame.valid_rect.height = snapshot->height;
            frame.data = source.pixels;
            if (current_surface->WriteFrame(frame).ok()) {
                rendered_image_generation = image_generation;
                rendered_position_generation = position_generation;
                // GIF 使用原始帧延迟；不是固定 30fps 的 H.264 编码或 MIPI 刷新频率。
                next_frame = now + std::chrono::milliseconds(
                    std::max<uint32_t>(20, source.delay_ms));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (auto final_surface = std::atomic_exchange(&surface, std::shared_ptr<kavio::TxSurface>{})) {
        uint64_t id = 0; final_surface->Destroy(id);
    }
    client.Stop();
    return 0;
}
