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
bool ValidDimensions(uint32_t width, uint32_t height, uint32_t frames) {
    if (width == 0 || height == 0 || width > demo_image_panel::kMaxWidth ||
        height > demo_image_panel::kMaxHeight || frames == 0 ||
        frames > demo_image_panel::kMaxFrames) return false;
    const uint64_t bytes = static_cast<uint64_t>(width) * height * 4ull * frames;
    return bytes <= demo_image_panel::kMaxDecodedBytes;
}

// Channel 回调可能来自 SDK 工作线程；这里只更新受 mutex 保护的接收状态，不直接操作 TX Surface。
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

}  // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    kavio::KavioClient client;
    kavio::SessionOptions options;
    options.client_name = "demo_panel_image";
    options.rx_params.enabled = false;
    options.tx_params.enabled = true;
    options.tx_params.z_order = 100;

    kavio::KavioClient::Callbacks callbacks;
    callbacks.on_client_ready = [] { g_client_ready.store(true); };
    callbacks.on_client_invalid = [] {
        g_client_ready.store(false);
        g_channel_ready.store(false);
    };
    callbacks.on_comm_channel_ready_changed = [](bool ready) {
        g_channel_ready.store(ready);
    };
    callbacks.on_comm_channel_data = HandleMessage;

    const kavio::Status start = client.Start(options, std::move(callbacks));
    if (!start.ok()) {
        std::cerr << "Start KavioClient failed: " << start.message << std::endl;
        return 1;
    }

    kavio::TxSurface surface;
    uint32_t surface_width = 0;
    uint32_t surface_height = 0;
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

        // TX Surface 尺寸必须与当前图片一致；换成不同尺寸图片时先销毁旧 Surface 再重建。
        if (g_client_ready.load() && snapshot && !snapshot->frames.empty() &&
            (!surface.IsValid() || surface_width != snapshot->width ||
             surface_height != snapshot->height)) {
            if (surface.IsValid()) surface.Destroy(std::chrono::seconds(2));
            const kavio::Status status = client.CreateTxSurface(
                snapshot->width, snapshot->height, surface, std::chrono::seconds(2));
            if (status.ok()) {
                surface_width = snapshot->width;
                surface_height = snapshot->height;
                frame_index = 0;
                rendered_image_generation = static_cast<uint64_t>(-1);
                next_frame = std::chrono::steady_clock::now();
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (image_generation != rendered_image_generation) {
            frame_index = 0;
            next_frame = now;
        } else if (snapshot && snapshot->frames.size() > 1 && now >= next_frame) {
            frame_index = (frame_index + 1) % snapshot->frames.size();
        }

        // 只有新图片、GIF 到帧时间或位置变化时才写 TX，静态图片不会被 10 ms 主循环反复重写。
        const bool animation_due = surface.IsValid() && snapshot &&
                                   !snapshot->frames.empty() && now >= next_frame;
        const bool position_changed = position_generation != rendered_position_generation;
        if (surface.IsValid() && snapshot && !snapshot->frames.empty() &&
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
            if (surface.WriteFrame(frame).ok()) {
                rendered_image_generation = image_generation;
                rendered_position_generation = position_generation;
                next_frame = now + std::chrono::milliseconds(
                    std::max<uint32_t>(20, source.delay_ms));
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (surface.IsValid()) surface.Destroy(std::chrono::seconds(2));
    client.Stop();
    return 0;
}
