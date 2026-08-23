#define NOMINMAX
#include <windows.h>

#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <cwchar>
#include <chrono>
#include <memory>
#include <new>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>
#include "VirtualHdmiProtocol.h"
#include "VirtualDisplayCapture.h"
#include "VirtualDisplayControl.h"

namespace {

using namespace kavio_virtual_hdmi;

HMODULE g_module = nullptr;
constexpr UINT_PTR kPanelTimerId = 1;
constexpr UINT kPanelTimerMs = 1000;
constexpr UINT kPanelDriverOperationDone = WM_APP + 1;
constexpr int kButtonInstall = 1001;
constexpr int kButtonUninstall = 1002;
constexpr int kButtonDisplaySettings = 1003;
constexpr DWORD kCaptureRetryMs = 1000;
constexpr std::uint32_t kDefaultMaxFps = 60;
constexpr std::uint32_t kPeriodicKeyFrameInterval = 90;
constexpr std::size_t kMaxInflightFrames = 2;

std::uint64_t NowMilliseconds() {
    return static_cast<std::uint64_t>(GetTickCount64());
}


std::wstring ModulePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            g_module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return std::wstring();
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring ModuleDirectory() {
    const std::wstring path = ModulePath();
    if (path.empty()) return L".";
    const std::wstring::size_type slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    static std::uint32_t table[256] = {};
    static std::once_flag once;
    std::call_once(once, []() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
            }
            table[i] = value;
        }
    });

    std::uint32_t result = 0xFFFFFFFFu;
    for (std::size_t index = 0; index < size; ++index) {
        result = table[(result ^ data[index]) & 0xFFu] ^ (result >> 8u);
    }
    return result ^ 0xFFFFFFFFu;
}

std::vector<std::uint8_t> EncodeRleBgra32(const std::uint8_t* pixels,
                                          std::size_t byte_count) {
    std::vector<std::uint8_t> output;
    if (!pixels || byte_count == 0 || byte_count % 4 != 0) return output;
    const std::size_t pixel_count = byte_count / 4;
    output.reserve(byte_count / 2);

    std::size_t index = 0;
    while (index < pixel_count) {
        std::size_t run = 1;
        while (run < 128 && index + run < pixel_count &&
               std::memcmp(pixels + index * 4, pixels + (index + run) * 4, 4) == 0) {
            ++run;
        }
        if (run >= 3) {
            output.push_back(static_cast<std::uint8_t>(0x80u | (run - 1u)));
            output.insert(output.end(), pixels + index * 4, pixels + index * 4 + 4);
            index += run;
            continue;
        }

        const std::size_t literal_start = index;
        std::size_t literal_count = 0;
        while (index < pixel_count && literal_count < 128) {
            run = 1;
            while (run < 128 && index + run < pixel_count &&
                   std::memcmp(pixels + index * 4,
                               pixels + (index + run) * 4, 4) == 0) {
                ++run;
            }
            if (run >= 3 && literal_count > 0) break;
            ++index;
            ++literal_count;
            if (run >= 3) break;
        }
        output.push_back(static_cast<std::uint8_t>(literal_count - 1u));
        output.insert(output.end(),
                      pixels + literal_start * 4,
                      pixels + (literal_start + literal_count) * 4);
    }
    return output;
}

struct HostComponent {
    const KavioHostApi* host = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> channel_ready{false};
    std::atomic<bool> app_ready{false};
    std::atomic<bool> source_ready{false};
    std::atomic<bool> capture_reset_requested{false};
    std::atomic<std::uint32_t> app_max_fps{kDefaultMaxFps};
    std::atomic<std::uint64_t> source_frame_id{0};
    std::atomic<std::uint64_t> captured_frames{0};
    std::atomic<std::uint64_t> sent_frames{0};
    std::atomic<std::uint64_t> acked_frames{0};
    std::atomic<std::uint64_t> sent_bytes{0};
    std::atomic<std::uint64_t> last_ack_us{0};
    std::atomic<std::uint64_t> sent_i_frames{0};
    std::atomic<std::uint64_t> sent_p_frames{0};
    std::atomic<std::uint64_t> key_frame_requests{0};
    std::atomic<bool> force_key_frame{true};
    std::atomic<std::uint64_t> last_acked_frame_id{0};
    std::atomic<std::uint32_t> in_flight_frame_count{0};
    std::atomic<std::uint64_t> window_skipped_capture_frames{0};

    std::atomic<std::uint64_t> total_tiles_sent{0};
    std::atomic<std::uint64_t> total_rle_tiles_sent{0};
    std::atomic<std::uint64_t> total_raw_tiles_sent{0};
    std::atomic<std::uint64_t> total_raw_frame_bytes{0};
    std::atomic<std::uint64_t> total_encoded_frame_bytes{0};
    std::atomic<std::uint64_t> total_build_us{0};
    std::atomic<std::uint64_t> total_send_us{0};
    std::atomic<std::uint32_t> last_frame_tiles{0};
    std::atomic<std::uint32_t> last_frame_rle_tiles{0};
    std::atomic<std::uint32_t> last_frame_raw_tiles{0};
    std::atomic<std::uint64_t> last_frame_raw_bytes{0};
    std::atomic<std::uint64_t> last_frame_encoded_bytes{0};
    std::atomic<std::uint64_t> last_build_us{0};
    std::atomic<std::uint64_t> last_send_us{0};
    std::atomic<std::uint32_t> last_ack_tile_count{0};

    // Panel-side one-second rate sampler. These fields are only read/written
    // by the panel thread, while the cumulative counters above are atomics
    // updated by the capture / channel threads. Rates are stored as x10 to
    // avoid floating-point synchronization between threads.
    std::uint64_t panel_rate_sample_ms = 0;
    std::uint64_t panel_last_captured_frames = 0;
    std::uint64_t panel_last_sent_frames = 0;
    std::uint64_t panel_last_acked_frames = 0;
    std::uint64_t panel_last_sent_bytes = 0;
    std::uint64_t panel_capture_fps_x10 = 0;
    std::uint64_t panel_push_fps_x10 = 0;
    std::uint64_t panel_ack_fps_x10 = 0;
    std::uint64_t panel_push_kib_per_sec_x10 = 0;
    std::uint64_t panel_last_total_tiles_sent = 0;
    std::uint64_t panel_last_rle_tiles_sent = 0;
    std::uint64_t panel_last_raw_tiles_sent = 0;
    std::uint64_t panel_last_total_raw_frame_bytes = 0;
    std::uint64_t panel_last_total_encoded_frame_bytes = 0;
    std::uint64_t panel_last_total_build_us = 0;
    std::uint64_t panel_last_total_send_us = 0;
    std::uint64_t panel_avg_tiles_per_frame_x10 = 0;
    std::uint64_t panel_rle_tile_ratio_x10 = 0;
    std::uint64_t panel_raw_tile_ratio_x10 = 0;
    std::uint64_t panel_raw_kib_per_frame_x10 = 0;
    std::uint64_t panel_encoded_kib_per_frame_x10 = 0;
    std::uint64_t panel_build_ms_x10 = 0;
    std::uint64_t panel_send_ms_x10 = 0;

    std::thread capture_thread;
    std::mutex stream_mutex;
    // Host P-frame baseline advances immediately after a complete frame has been
    // accepted by the communication channel. ACK is cumulative flow feedback:
    // it releases slots in the bounded two-frame window. If Device loses the
    // dependency chain it requests a new I frame.
    std::vector<std::uint8_t> baseline;
    std::uint64_t baseline_frame_id = 0;
    std::uint32_t frames_since_key_frame = 0;
    std::uint64_t stream_generation = 1;
    std::uint64_t next_transfer_id = 1;
    std::uint64_t last_send_ms = 0;
    std::deque<std::uint64_t> in_flight_frames;

    std::mutex panel_mutex;
    std::thread panel_thread;
    HWND panel_window = nullptr;
    HANDLE panel_ready = nullptr;

    std::atomic<bool> driver_installed{false};
    std::atomic<bool> driver_operation_running{false};
    std::mutex driver_operation_mutex;
    std::thread driver_operation_thread;
    std::wstring driver_operation_status = L"No driver operation has been requested.";
};

void Log(HostComponent* component, std::uint32_t level, const std::string& text) {
    if (component && component->host && component->host->log) {
        component->host->log(component->host->context, level, text.c_str());
    }
}

bool SendDatagram(HostComponent* component, const void* data, std::size_t size) {
    if (!component || !data || size == 0 || size > kMaxDatagramBytes ||
        !component->running.load() || !component->channel_ready.load() ||
        !component->app_ready.load() || !component->host || !component->host->send) {
        return false;
    }
    for (int attempt = 0; attempt < 250; ++attempt) {
        const KavioResult result = component->host->send(
            component->host->context, data, size);
        if (result == KavioResult_Success) {
            component->sent_bytes.fetch_add(size);
            return true;
        }
        if (result != KavioResult_QueueFull) return false;
        Sleep(2);
    }
    return false;
}

std::vector<std::uint8_t> MakeMessage(MessageType type,
                                      const void* payload,
                                      std::size_t payload_size) {
    std::vector<std::uint8_t> output(sizeof(MessageHeader) + payload_size);
    MessageHeader header{};
    header.magic = kProtocolMagic;
    header.version = kProtocolVersion;
    header.type = static_cast<std::uint16_t>(type);
    header.payload_size = static_cast<std::uint32_t>(payload_size);
    std::memcpy(output.data(), &header, sizeof(header));
    if (payload_size > 0) {
        std::memcpy(output.data() + sizeof(header), payload, payload_size);
    }
    return output;
}

template <typename T>
std::vector<std::uint8_t> MakeMessage(MessageType type, const T& payload) {
    return MakeMessage(type, &payload, sizeof(payload));
}

bool TileChanged(const std::vector<std::uint8_t>& frame,
                 const std::vector<std::uint8_t>& baseline,
                 std::uint16_t x, std::uint16_t y,
                 std::uint16_t width, std::uint16_t height) {
    if (baseline.size() != frame.size()) return true;
    const std::size_t row_bytes = static_cast<std::size_t>(width) * kBytesPerPixel;
    for (std::uint16_t row = 0; row < height; ++row) {
        const std::size_t offset =
            static_cast<std::size_t>(y + row) * kFrameStride +
            static_cast<std::size_t>(x) * kBytesPerPixel;
        if (std::memcmp(frame.data() + offset, baseline.data() + offset, row_bytes) != 0) {
            return true;
        }
    }
    return false;
}

std::vector<std::uint8_t> ExtractTile(const std::vector<std::uint8_t>& frame,
                                      std::uint16_t x, std::uint16_t y,
                                      std::uint16_t width, std::uint16_t height) {
    const std::size_t row_bytes = static_cast<std::size_t>(width) * kBytesPerPixel;
    std::vector<std::uint8_t> tile(row_bytes * height);
    for (std::uint16_t row = 0; row < height; ++row) {
        const std::size_t source_offset =
            static_cast<std::size_t>(y + row) * kFrameStride +
            static_cast<std::size_t>(x) * kBytesPerPixel;
        std::memcpy(tile.data() + static_cast<std::size_t>(row) * row_bytes,
                    frame.data() + source_offset, row_bytes);
    }
    return tile;
}

struct TileDescriptor {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
};

struct BuiltFrame {
    std::vector<std::vector<std::uint8_t>> datagrams;
    bool key_frame = false;
    std::uint32_t tile_count = 0;
    std::uint32_t rle_tile_count = 0;
    std::uint32_t raw_tile_count = 0;
    std::uint64_t raw_bytes = 0;
    std::uint64_t encoded_bytes = 0;
};

BuiltFrame BuildFrameDatagrams(
    std::uint64_t frame_id,
    std::uint64_t timestamp_ns,
    const std::vector<std::uint8_t>& frame,
    const std::vector<std::uint8_t>& baseline,
    std::uint64_t baseline_frame_id,
    bool force_key_frame) {
    BuiltFrame result;
    std::vector<TileDescriptor> changed;

    if (!force_key_frame) {
        for (std::uint32_t y = 0; y < kFrameHeight; y += kTileSize) {
            for (std::uint32_t x = 0; x < kFrameWidth; x += kTileSize) {
                TileDescriptor tile{};
                tile.x = static_cast<std::uint16_t>(x);
                tile.y = static_cast<std::uint16_t>(y);
                tile.width = static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(kTileSize, kFrameWidth - x));
                tile.height = static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(kTileSize, kFrameHeight - y));
                if (TileChanged(frame, baseline, tile.x, tile.y, tile.width, tile.height)) {
                    changed.push_back(tile);
                }
            }
        }
        if (changed.empty()) return result;
    }

    result.key_frame = force_key_frame || baseline.size() != frame.size() ||
                       baseline_frame_id == 0 ||
                       changed.size() * 100u >= kMaxTilesPerFrame * 80u;
    if (result.key_frame) {
        changed.clear();
        for (std::uint32_t y = 0; y < kFrameHeight; y += kTileSize) {
            for (std::uint32_t x = 0; x < kFrameWidth; x += kTileSize) {
                TileDescriptor tile{};
                tile.x = static_cast<std::uint16_t>(x);
                tile.y = static_cast<std::uint16_t>(y);
                tile.width = static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(kTileSize, kFrameWidth - x));
                tile.height = static_cast<std::uint16_t>(
                    std::min<std::uint32_t>(kTileSize, kFrameHeight - y));
                changed.push_back(tile);
            }
        }
    }

    result.datagrams.reserve(changed.size() + 2);
    result.tile_count = static_cast<std::uint32_t>(changed.size());
    FrameBeginPayload begin{};
    begin.frame_id = frame_id;
    begin.base_frame_id = result.key_frame ? 0 : baseline_frame_id;
    begin.timestamp_ns = timestamp_ns;
    begin.width = kFrameWidth;
    begin.height = kFrameHeight;
    begin.stride = kFrameStride;
    begin.changed_tile_count = static_cast<std::uint32_t>(changed.size());
    begin.frame_type = static_cast<std::uint8_t>(
        result.key_frame ? FrameType::IFrame : FrameType::PFrame);
    result.datagrams.push_back(MakeMessage(MessageType::FrameBegin, begin));

    for (const TileDescriptor& descriptor : changed) {
        std::vector<std::uint8_t> raw = ExtractTile(
            frame, descriptor.x, descriptor.y, descriptor.width, descriptor.height);
        std::vector<std::uint8_t> encoded = EncodeRleBgra32(raw.data(), raw.size());
        const bool use_rle = !encoded.empty() && encoded.size() + 8 < raw.size();
        const std::vector<std::uint8_t>& data = use_rle ? encoded : raw;
        result.raw_bytes += raw.size();
        result.encoded_bytes += data.size();
        if (use_rle) {
            ++result.rle_tile_count;
        } else {
            ++result.raw_tile_count;
        }

        TilePayloadHeader tile{};
        tile.frame_id = frame_id;
        tile.x = descriptor.x;
        tile.y = descriptor.y;
        tile.width = descriptor.width;
        tile.height = descriptor.height;
        tile.codec = static_cast<std::uint8_t>(
            use_rle ? TileCodec::RleBgra32 : TileCodec::RawBgra);
        tile.raw_size = static_cast<std::uint32_t>(raw.size());
        tile.encoded_size = static_cast<std::uint32_t>(data.size());

        std::vector<std::uint8_t> payload(sizeof(tile) + data.size());
        std::memcpy(payload.data(), &tile, sizeof(tile));
        std::memcpy(payload.data() + sizeof(tile), data.data(), data.size());
        result.datagrams.push_back(MakeMessage(
            MessageType::Tile, payload.data(), payload.size()));
    }

    FrameEndPayload end{};
    end.frame_id = frame_id;
    end.frame_crc32 = Crc32(frame.data(), frame.size());
    result.datagrams.push_back(MakeMessage(MessageType::FrameEnd, end));
    return result;
}

void ResetStreamState(HostComponent* component, bool reset_baseline) {
    std::lock_guard<std::mutex> lock(component->stream_mutex);
    ++component->stream_generation;
    component->frames_since_key_frame = 0;
    component->force_key_frame.store(true);
    component->in_flight_frames.clear();
    component->in_flight_frame_count.store(0);
    if (reset_baseline) {
        component->baseline.clear();
        component->baseline_frame_id = 0;
    }
}

void CaptureLoop(HostComponent* component) {
    VirtualDisplayCapture capture;
    std::uint64_t next_capture_retry = 0;
    std::vector<std::uint8_t> frame;
    std::wstring last_capture_error;

    while (component->running.load()) {
        if (component->capture_reset_requested.exchange(false)) {
            capture.Close();
            component->source_ready.store(false);
            next_capture_retry = 0;
        }

        if (!capture.IsOpen()) {
            const std::uint64_t now = NowMilliseconds();
            if (now < next_capture_retry) {
                Sleep(50);
                continue;
            }

            std::wstring detail;
            if (!capture.TryOpen(detail)) {
                component->source_ready.store(false);
                next_capture_retry = now + kCaptureRetryMs;
                if (!detail.empty() && detail != last_capture_error) {
                    std::string text(detail.begin(), detail.end());
                    Log(component, KavioHostLogLevel_Warning,
                        "VirtualDrivers VDD capture is not ready: " + text);
                    last_capture_error = detail;
                }
                Sleep(100);
                continue;
            }

            component->source_ready.store(true);
            last_capture_error.clear();
            std::string display_name(capture.DisplayName().begin(), capture.DisplayName().end());
            Log(component, KavioHostLogLevel_Info,
                "VirtualDrivers VDD desktop capture connected to " + display_name);
        }

        if (!component->channel_ready.load() || !component->app_ready.load()) {
            Sleep(20);
            continue;
        }

        std::uint64_t source_frame_id = 0;
        std::uint64_t timestamp_ns = 0;
        std::wstring detail;
        if (!capture.WaitAndRead(
                frame, source_frame_id, timestamp_ns, 100, detail)) {
            if (!capture.IsOpen()) {
                component->source_ready.store(false);
                next_capture_retry = NowMilliseconds() + kCaptureRetryMs;
                if (!detail.empty() && detail != last_capture_error) {
                    std::string text(detail.begin(), detail.end());
                    Log(component, KavioHostLogLevel_Warning,
                        "VirtualDrivers VDD capture lost: " + text);
                    last_capture_error = detail;
                }
            }
            continue;
        }

        component->source_ready.store(true);
        component->source_frame_id.store(source_frame_id);
        component->captured_frames.fetch_add(1);

        const std::uint64_t now = NowMilliseconds();
        const std::uint32_t max_fps = std::max<std::uint32_t>(1, component->app_max_fps.load());
        const std::uint64_t min_interval = std::max<std::uint64_t>(1, 1000u / max_fps);

        BuiltFrame built;
        std::uint64_t transfer_id = 0;
        std::uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lock(component->stream_mutex);
            // Keep at most two complete frames accepted by the Host channel but not
            // cumulatively ACKed by Device. When the window is full we intentionally
            // drop this captured desktop frame instead of queueing stale video.
            if (component->in_flight_frames.size() >= kMaxInflightFrames) {
                component->window_skipped_capture_frames.fetch_add(1);
                continue;
            }
            if (now - component->last_send_ms < min_interval) continue;

            const bool periodic_key =
                component->baseline_frame_id == 0 ||
                component->frames_since_key_frame >= (kPeriodicKeyFrameInterval - 1u);
            const bool force_key = component->force_key_frame.exchange(false) || periodic_key;

            transfer_id = component->next_transfer_id++;
            generation = component->stream_generation;
            const auto build_begin = std::chrono::steady_clock::now();
            built = BuildFrameDatagrams(
                transfer_id,
                timestamp_ns,
                frame,
                component->baseline,
                component->baseline_frame_id,
                force_key);
            const auto build_end = std::chrono::steady_clock::now();
            component->last_build_us.store(
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    build_end - build_begin).count()));
            if (built.datagrams.empty()) continue;
            component->last_send_ms = now;
        }

        const auto send_begin = std::chrono::steady_clock::now();
        bool sent = true;
        for (const std::vector<std::uint8_t>& datagram : built.datagrams) {
            if (!SendDatagram(component, datagram.data(), datagram.size())) {
                sent = false;
                break;
            }
        }
        const auto send_end = std::chrono::steady_clock::now();
        const std::uint64_t send_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(send_end - send_begin).count());
        component->last_send_us.store(send_us);
        if (!sent) {
            Log(component, KavioHostLogLevel_Warning,
                "Virtual HDMI frame send failed; forcing a new I frame");
            ResetStreamState(component, true);
            continue;
        }

        // Advance the Host dependency baseline as soon as all datagrams for this
        // frame were accepted by the channel. The next frame may be sent without
        // waiting for this exact ACK, but the two-frame window bounds how far Host
        // may run ahead of Device. Dependency loss is recovered with a new I frame.
        {
            std::lock_guard<std::mutex> lock(component->stream_mutex);
            if (generation == component->stream_generation &&
                component->channel_ready.load() && component->app_ready.load()) {
                component->baseline = frame;
                component->baseline_frame_id = transfer_id;
                if (built.key_frame) {
                    component->frames_since_key_frame = 0;
                } else {
                    ++component->frames_since_key_frame;
                }

                // ACK may race with the final datagram send. If Device already
                // cumulatively ACKed this frame, do not put an already-completed
                // frame back into the in-flight window.
                if (transfer_id > component->last_acked_frame_id.load()) {
                    component->in_flight_frames.push_back(transfer_id);
                }
                component->in_flight_frame_count.store(
                    static_cast<std::uint32_t>(component->in_flight_frames.size()));
            } else {
                component->force_key_frame.store(true);
            }
        }

        component->sent_frames.fetch_add(1);
        component->total_tiles_sent.fetch_add(built.tile_count);
        component->total_rle_tiles_sent.fetch_add(built.rle_tile_count);
        component->total_raw_tiles_sent.fetch_add(built.raw_tile_count);
        component->total_raw_frame_bytes.fetch_add(built.raw_bytes);
        component->total_encoded_frame_bytes.fetch_add(built.encoded_bytes);
        component->total_build_us.fetch_add(component->last_build_us.load());
        component->total_send_us.fetch_add(send_us);
        component->last_frame_tiles.store(built.tile_count);
        component->last_frame_rle_tiles.store(built.rle_tile_count);
        component->last_frame_raw_tiles.store(built.raw_tile_count);
        component->last_frame_raw_bytes.store(built.raw_bytes);
        component->last_frame_encoded_bytes.store(built.encoded_bytes);
        if (built.key_frame) {
            component->sent_i_frames.fetch_add(1);
        } else {
            component->sent_p_frames.fetch_add(1);
        }
    }

    capture.Close();
    component->source_ready.store(false);
}

bool ParseMessage(const void* data, std::size_t size,
                  MessageType& type, const std::uint8_t*& payload,
                  std::size_t& payload_size) {
    if (!data || size < sizeof(MessageHeader)) return false;
    MessageHeader header{};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != kProtocolMagic || header.version != kProtocolVersion ||
        header.payload_size != size - sizeof(MessageHeader)) {
        return false;
    }
    type = static_cast<MessageType>(header.type);
    payload = static_cast<const std::uint8_t*>(data) + sizeof(MessageHeader);
    payload_size = header.payload_size;
    return true;
}

void HandleAppReady(HostComponent* component, const std::uint8_t* payload,
                    std::size_t payload_size) {
    if (payload_size != sizeof(AppReadyPayload)) return;
    AppReadyPayload ready{};
    std::memcpy(&ready, payload, sizeof(ready));
    if (ready.width != kFrameWidth || ready.height != kFrameHeight ||
        ready.pixel_format != kPixelFormatBgra8888 ||
        ready.tile_size != kTileSize) {
        Log(component, KavioHostLogLevel_Error,
            "Device App rejected: incompatible virtual HDMI geometry or pixel format");
        component->app_ready.store(false);
        return;
    }
    component->app_max_fps.store(std::min<std::uint32_t>(60, std::max<std::uint32_t>(1, ready.max_fps)));
    const bool was_ready = component->app_ready.exchange(true);
    if (!was_ready) {
        ResetStreamState(component, true);
        component->capture_reset_requested.store(true);
        Log(component, KavioHostLogLevel_Info,
            "Device App is ready for virtual HDMI frames");
    }
}

void HandleFrameAck(HostComponent* component, const std::uint8_t* payload,
                    std::size_t payload_size) {
    if (payload_size != sizeof(FrameAckPayload)) return;
    FrameAckPayload ack{};
    std::memcpy(&ack, payload, sizeof(ack));

    if (ack.status == static_cast<std::uint32_t>(FrameAckStatus::Ok)) {
        component->acked_frames.fetch_add(1);
        component->last_ack_us.store(ack.elapsed_us);
        component->last_ack_tile_count.store(ack.received_tile_count);
        {
            std::lock_guard<std::mutex> lock(component->stream_mutex);
            if (ack.frame_id > component->last_acked_frame_id.load()) {
                component->last_acked_frame_id.store(ack.frame_id);
            }
            while (!component->in_flight_frames.empty() &&
                   component->in_flight_frames.front() <= ack.frame_id) {
                component->in_flight_frames.pop_front();
            }
            component->in_flight_frame_count.store(
                static_cast<std::uint32_t>(component->in_flight_frames.size()));
        }
        return;
    }

    // A negative ACK invalidates the Device dependency chain.
    // Clear outstanding window state and force a self-contained I frame. Any
    // P datagrams already queued in the channel will be discarded by Device while
    // it is in resync state.
    ResetStreamState(component, true);
    std::ostringstream stream;
    stream << "Device rejected virtual HDMI frame " << ack.frame_id
           << " status=" << ack.status << "; forcing a new I frame";
    Log(component, KavioHostLogLevel_Warning, stream.str());
}

void HandleKeyFrameRequest(HostComponent* component,
                           const std::uint8_t* payload,
                           std::size_t payload_size) {
    if (payload_size != sizeof(KeyFrameRequestPayload)) return;
    KeyFrameRequestPayload request{};
    std::memcpy(&request, payload, sizeof(request));

    component->key_frame_requests.fetch_add(1);
    ResetStreamState(component, true);

    std::ostringstream stream;
    stream << "Device requested I frame: last_good=" << request.last_good_frame_id
           << " failed=" << request.failed_frame_id
           << " reason=" << request.reason;
    Log(component, KavioHostLogLevel_Warning, stream.str());
}

void SetDriverOperationStatus(HostComponent* component, const std::wstring& status) {
    std::lock_guard<std::mutex> lock(component->driver_operation_mutex);
    component->driver_operation_status = status;
}

std::wstring GetDriverOperationStatus(HostComponent* component) {
    std::lock_guard<std::mutex> lock(component->driver_operation_mutex);
    return component->driver_operation_status;
}

void NotifyPanelDriverOperationDone(HostComponent* component) {
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (window) PostMessageW(window, kPanelDriverOperationDone, 0, 0);
}

void DriverOperationThreadMain(HostComponent* component, std::wstring action) {
    if (_wcsicmp(action.c_str(), L"install") == 0) {
        std::wstring missing_file;
        if (!kavio_virtual_hdmi_control::ValidatePackage(missing_file)) {
            SetDriverOperationStatus(
                component, L"Install failed: missing staged VDD file: " + missing_file);
            component->driver_operation_running.store(false);
            NotifyPanelDriverOperationDone(component);
            return;
        }
    }

    const std::wstring root = ModuleDirectory();
    const std::wstring module_path = ModulePath();
    if (module_path.empty()) {
        SetDriverOperationStatus(component, L"Operation failed: host.dll path is unavailable.");
        component->driver_operation_running.store(false);
        NotifyPanelDriverOperationDone(component);
        return;
    }

    wchar_t system_directory[MAX_PATH] = {};
    const UINT system_length = GetSystemDirectoryW(system_directory, ARRAYSIZE(system_directory));
    if (system_length == 0 || system_length >= ARRAYSIZE(system_directory)) {
        SetDriverOperationStatus(component, L"Operation failed: Windows system directory is unavailable.");
        component->driver_operation_running.store(false);
        NotifyPanelDriverOperationDone(component);
        return;
    }
    const std::wstring rundll32 = std::wstring(system_directory) + L"\\rundll32.exe";
    const std::wstring parameters =
        L"\"" + module_path + L"\",KavioVirtualDisplayAdmin " + action;

    SetDriverOperationStatus(
        component, action == L"install"
                    ? L"Waiting for administrator approval and installing/configuring VirtualDrivers VDD..."
                    : L"Waiting for administrator approval and removing the Kavio VDD setup...");

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = rundll32.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = root.c_str();
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        std::wostringstream status;
        status << L"Operation was not started. Win32 error=" << error;
        if (error == ERROR_CANCELLED) status << L" (administrator approval was cancelled)";
        SetDriverOperationStatus(component, status.str());
        component->driver_operation_running.store(false);
        NotifyPanelDriverOperationDone(component);
        return;
    }

    DWORD exit_code = 1;
    if (info.hProcess) {
        WaitForSingleObject(info.hProcess, INFINITE);
        GetExitCodeProcess(info.hProcess, &exit_code);
        CloseHandle(info.hProcess);
    }

    component->driver_installed.store(kavio_virtual_hdmi_control::IsInstalled());
    component->capture_reset_requested.store(true);
    component->source_ready.store(false);

    std::wstring elevated_detail;
    unsigned long detail_code = 0;
    const bool have_elevated_detail =
        kavio_virtual_hdmi_control::GetLastOperationDetail(
            elevated_detail, &detail_code) && !elevated_detail.empty();

    std::wostringstream status;
    if (have_elevated_detail && detail_code == exit_code) {
        status << elevated_detail;
        if (exit_code == 3) status << L" Windows restart is required.";
    } else if (exit_code == 0) {
        status << (action == L"install"
                       ? L"VirtualDrivers VDD installation/configuration completed successfully."
                       : L"Kavio VDD integration was removed/restored successfully.");
    } else if (exit_code == 3) {
        status << (action == L"install"
                       ? L"VirtualDrivers VDD installation/configuration completed; Windows restart is required."
                       : L"Kavio VDD removal completed; Windows restart is required.");
    } else {
        status << (action == L"install" ? L"VirtualDrivers VDD installation/configuration failed." : L"Kavio VDD removal failed.")
               << L" Elevated VDD operation exit code=" << exit_code;
    }
    SetDriverOperationStatus(component, status.str());
    component->driver_operation_running.store(false);
    NotifyPanelDriverOperationDone(component);
}

void StartDriverOperation(HostComponent* component, const wchar_t* action) {
    if (!component || !action) return;
    bool expected = false;
    if (!component->driver_operation_running.compare_exchange_strong(expected, true)) return;

    if (component->driver_operation_thread.joinable()) {
        component->driver_operation_thread.join();
    }
    SetDriverOperationStatus(component, L"Preparing driver operation...");
    component->driver_operation_thread = std::thread(
        DriverOperationThreadMain, component, std::wstring(action));
}

void UpdatePanelButtons(HWND window, HostComponent* component) {
    const BOOL enabled = component && !component->driver_operation_running.load();
    EnableWindow(GetDlgItem(window, kButtonInstall), enabled);
    EnableWindow(GetDlgItem(window, kButtonUninstall), enabled);
}

std::uint64_t RateX10(std::uint64_t delta, std::uint64_t elapsed_ms) {
    if (elapsed_ms == 0) return 0;
    return delta * 10000ull / elapsed_ms;
}

void ResetPanelRates(HostComponent* component) {
    if (!component) return;
    component->panel_rate_sample_ms = NowMilliseconds();
    component->panel_last_captured_frames = component->captured_frames.load();
    component->panel_last_sent_frames = component->sent_frames.load();
    component->panel_last_acked_frames = component->acked_frames.load();
    component->panel_last_sent_bytes = component->sent_bytes.load();
    component->panel_last_total_tiles_sent = component->total_tiles_sent.load();
    component->panel_last_rle_tiles_sent = component->total_rle_tiles_sent.load();
    component->panel_last_raw_tiles_sent = component->total_raw_tiles_sent.load();
    component->panel_last_total_raw_frame_bytes = component->total_raw_frame_bytes.load();
    component->panel_last_total_encoded_frame_bytes = component->total_encoded_frame_bytes.load();
    component->panel_last_total_build_us = component->total_build_us.load();
    component->panel_last_total_send_us = component->total_send_us.load();
    component->panel_capture_fps_x10 = 0;
    component->panel_push_fps_x10 = 0;
    component->panel_ack_fps_x10 = 0;
    component->panel_push_kib_per_sec_x10 = 0;
    component->panel_avg_tiles_per_frame_x10 = 0;
    component->panel_rle_tile_ratio_x10 = 0;
    component->panel_raw_tile_ratio_x10 = 0;
    component->panel_raw_kib_per_frame_x10 = 0;
    component->panel_encoded_kib_per_frame_x10 = 0;
    component->panel_build_ms_x10 = 0;
    component->panel_send_ms_x10 = 0;
}

void UpdatePanelRates(HostComponent* component) {
    if (!component) return;

    const std::uint64_t now = NowMilliseconds();
    if (component->panel_rate_sample_ms == 0) {
        ResetPanelRates(component);
        return;
    }

    const std::uint64_t elapsed_ms = now - component->panel_rate_sample_ms;
    if (elapsed_ms < 250) return;

    const std::uint64_t captured = component->captured_frames.load();
    const std::uint64_t sent = component->sent_frames.load();
    const std::uint64_t acked = component->acked_frames.load();
    const std::uint64_t bytes = component->sent_bytes.load();
    const std::uint64_t total_tiles = component->total_tiles_sent.load();
    const std::uint64_t total_rle_tiles = component->total_rle_tiles_sent.load();
    const std::uint64_t total_raw_tiles = component->total_raw_tiles_sent.load();
    const std::uint64_t total_raw_bytes = component->total_raw_frame_bytes.load();
    const std::uint64_t total_encoded_bytes = component->total_encoded_frame_bytes.load();
    const std::uint64_t total_build_us = component->total_build_us.load();
    const std::uint64_t total_send_us = component->total_send_us.load();

    component->panel_capture_fps_x10 =
        RateX10(captured - component->panel_last_captured_frames, elapsed_ms);
    component->panel_push_fps_x10 =
        RateX10(sent - component->panel_last_sent_frames, elapsed_ms);
    component->panel_ack_fps_x10 =
        RateX10(acked - component->panel_last_acked_frames, elapsed_ms);

    // KiB/s x10 = bytes * 10 * 1000 / elapsed_ms / 1024.
    const std::uint64_t byte_delta = bytes - component->panel_last_sent_bytes;
    component->panel_push_kib_per_sec_x10 =
        byte_delta * 10000ull / (elapsed_ms * 1024ull);

    const std::uint64_t sent_delta = sent - component->panel_last_sent_frames;
    const std::uint64_t tiles_delta = total_tiles - component->panel_last_total_tiles_sent;
    const std::uint64_t rle_tiles_delta = total_rle_tiles - component->panel_last_rle_tiles_sent;
    const std::uint64_t raw_tiles_delta = total_raw_tiles - component->panel_last_raw_tiles_sent;
    const std::uint64_t raw_bytes_delta = total_raw_bytes - component->panel_last_total_raw_frame_bytes;
    const std::uint64_t encoded_bytes_delta = total_encoded_bytes - component->panel_last_total_encoded_frame_bytes;
    const std::uint64_t build_us_delta = total_build_us - component->panel_last_total_build_us;
    const std::uint64_t send_us_delta = total_send_us - component->panel_last_total_send_us;

    if (sent_delta != 0) {
        component->panel_avg_tiles_per_frame_x10 = tiles_delta * 10ull / sent_delta;
        component->panel_raw_kib_per_frame_x10 = raw_bytes_delta * 10ull / (sent_delta * 1024ull);
        component->panel_encoded_kib_per_frame_x10 = encoded_bytes_delta * 10ull / (sent_delta * 1024ull);
        component->panel_build_ms_x10 = build_us_delta / (sent_delta * 100ull);
        component->panel_send_ms_x10 = send_us_delta / (sent_delta * 100ull);
    } else {
        component->panel_avg_tiles_per_frame_x10 = 0;
        component->panel_raw_kib_per_frame_x10 = 0;
        component->panel_encoded_kib_per_frame_x10 = 0;
        component->panel_build_ms_x10 = 0;
        component->panel_send_ms_x10 = 0;
    }
    if (tiles_delta != 0) {
        component->panel_rle_tile_ratio_x10 = rle_tiles_delta * 1000ull / tiles_delta;
        component->panel_raw_tile_ratio_x10 = raw_tiles_delta * 1000ull / tiles_delta;
    } else {
        component->panel_rle_tile_ratio_x10 = 0;
        component->panel_raw_tile_ratio_x10 = 0;
    }

    component->panel_rate_sample_ms = now;
    component->panel_last_captured_frames = captured;
    component->panel_last_sent_frames = sent;
    component->panel_last_acked_frames = acked;
    component->panel_last_sent_bytes = bytes;
    component->panel_last_total_tiles_sent = total_tiles;
    component->panel_last_rle_tiles_sent = total_rle_tiles;
    component->panel_last_raw_tiles_sent = total_raw_tiles;
    component->panel_last_total_raw_frame_bytes = total_raw_bytes;
    component->panel_last_total_encoded_frame_bytes = total_encoded_bytes;
    component->panel_last_total_build_us = total_build_us;
    component->panel_last_total_send_us = total_send_us;
}

void AppendRateX10(std::wostringstream& text, std::uint64_t value_x10) {
    text << (value_x10 / 10ull) << L'.' << (value_x10 % 10ull);
}

std::wstring PanelStatusText(HostComponent* component) {
    std::wostringstream text;
    text << L"VirtualDrivers VDD: "
         << (component->driver_installed.load() ? L"installed" : L"not installed") << L"\r\n";
    text << L"DXGI VDD capture: "
         << (component->source_ready.load() ? L"active" : L"inactive") << L"\r\n";
    text << L"Kavio device channel: "
         << (component->channel_ready.load() ? L"online" : L"offline") << L"\r\n";
    text << L"Device App: "
         << (component->app_ready.load() ? L"ready" : L"waiting") << L"\r\n\r\n";
    text << L"Stream target: 720 x 720 BGRA8888\r\n";
    std::wstring vdd_mode;
    text << L"VDD desktop mode: ";
    if (kavio_virtual_hdmi_control::GetCurrentDisplayMode(vdd_mode)) {
        text << vdd_mode;
    } else {
        text << L"not active";
    }
    text << L"\r\n";
    text << L"Source frame: " << component->source_frame_id.load() << L"\r\n";
    text << L"FPS capture / push / ACK: ";
    AppendRateX10(text, component->panel_capture_fps_x10);
    text << L" / ";
    AppendRateX10(text, component->panel_push_fps_x10);
    text << L" / ";
    AppendRateX10(text, component->panel_ack_fps_x10);
    text << L"  (device limit " << component->app_max_fps.load() << L")\r\n";
    text << L"Sent / ACKed: " << component->sent_frames.load() << L" / "
         << component->acked_frames.load() << L"\r\n";
    text << L"In-flight / window: " << component->in_flight_frame_count.load()
         << L" / " << kMaxInflightFrames
         << L"  Capture skips: " << component->window_skipped_capture_frames.load() << L"\r\n";
    text << L"Frames I / P / key requests: " << component->sent_i_frames.load()
         << L" / " << component->sent_p_frames.load() << L" / "
         << component->key_frame_requests.load() << L"\r\n";
    text << L"Host build / send: ";
    AppendRateX10(text, component->panel_build_ms_x10);
    text << L" / ";
    AppendRateX10(text, component->panel_send_ms_x10);
    text << L" ms\r\n";
    text << L"Tiles/frame avg: ";
    AppendRateX10(text, component->panel_avg_tiles_per_frame_x10);
    text << L"  (RLE ";
    AppendRateX10(text, component->panel_rle_tile_ratio_x10);
    text << L"%, raw ";
    AppendRateX10(text, component->panel_raw_tile_ratio_x10);
    text << L"%)\r\n";
    text << L"Bytes/frame raw / encoded: ";
    AppendRateX10(text, component->panel_raw_kib_per_frame_x10);
    text << L" / ";
    AppendRateX10(text, component->panel_encoded_kib_per_frame_x10);
    text << L" KiB\r\n";
    text << L"Last frame tiles RLE/raw: " << component->last_frame_tiles.load()
         << L" (" << component->last_frame_rle_tiles.load() << L"/"
         << component->last_frame_raw_tiles.load() << L")\r\n";
    text << L"Push bandwidth: ";
    AppendRateX10(text, component->panel_push_kib_per_sec_x10);
    text << L" KiB/s\r\n";
    text << L"Transferred: " << (component->sent_bytes.load() / 1024) << L" KiB\r\n";
    text << L"Last device assembly: " << component->last_ack_us.load() << L" us\r\n";
    text << L"Last device tiles: " << component->last_ack_tile_count.load() << L"\r\n\r\n";
    text << L"Driver operation: " << GetDriverOperationStatus(component);
    return text.str();
}

LRESULT CALLBACK PanelWindowProc(HWND window, UINT message,
                                 WPARAM wparam, LPARAM lparam) {
    HostComponent* component = reinterpret_cast<HostComponent*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE: {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    case WM_CREATE: {
        HWND install_button = CreateWindowW(
            L"BUTTON", L"Install / configure VDD",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            20, 455, 205, 32, window,
            reinterpret_cast<HMENU>(kButtonInstall), g_module, nullptr);
        HWND uninstall_button = CreateWindowW(
            L"BUTTON", L"Remove Kavio VDD setup",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            235, 455, 165, 32, window,
            reinterpret_cast<HMENU>(kButtonUninstall), g_module, nullptr);
        HWND settings_button = CreateWindowW(
            L"BUTTON", L"Open display settings",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            410, 455, 190, 32, window,
            reinterpret_cast<HMENU>(kButtonDisplaySettings), g_module, nullptr);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(install_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(uninstall_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(settings_button, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        if (component) {
            component->driver_installed.store(kavio_virtual_hdmi_control::IsInstalled());
            ResetPanelRates(component);
        }
        UpdatePanelButtons(window, component);
        SetTimer(window, kPanelTimerId, kPanelTimerMs, nullptr);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kButtonInstall:
            StartDriverOperation(component, L"install");
            UpdatePanelButtons(window, component);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        case kButtonUninstall:
            if (MessageBoxW(
                    window,
                    L"Remove the Kavio VDD setup? If VDD existed before Kavio, the shared driver is kept and its previous settings are restored.",
                    L"Remove Kavio VDD setup",
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES) {
                StartDriverOperation(component, L"uninstall");
                UpdatePanelButtons(window, component);
                InvalidateRect(window, nullptr, TRUE);
            }
            return 0;
        case kButtonDisplaySettings:
            ShellExecuteW(window, L"open", L"ms-settings:display", nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        default:
            break;
        }
        break;
    case WM_TIMER:
        if (component) {
            component->driver_installed.store(kavio_virtual_hdmi_control::IsInstalled());
            UpdatePanelRates(component);
        }
        UpdatePanelButtons(window, component);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case kPanelDriverOperationDone:
        UpdatePanelButtons(window, component);
        InvalidateRect(window, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        SetBkMode(dc, TRANSPARENT);
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HFONT old_font = static_cast<HFONT>(SelectObject(dc, font));
        RECT text_rect{20, 20, client.right - 20, 430};
        const std::wstring text = component ? PanelStatusText(component) : L"HostComponent unavailable";
        DrawTextW(dc, text.c_str(), -1, &text_rect,
                  DT_LEFT | DT_TOP | DT_NOPREFIX | DT_WORDBREAK);
        SelectObject(dc, old_font);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kPanelTimerId);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void PanelThreadMain(HostComponent* component) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = PanelWindowProc;
    window_class.hInstance = g_module;
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = L"KavioVirtualHdmiStreamPanel";
    RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        0, window_class.lpszClassName, L"Kavio Virtual HDMI Stream",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 700, 545,
        nullptr, nullptr, g_module, component);
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        component->panel_window = window;
    }
    if (component->panel_ready) SetEvent(component->panel_ready);
    if (!window) return;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        component->panel_window = nullptr;
    }
}

void ClosePanel(HostComponent* component) {
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (window) PostMessageW(window, WM_CLOSE, 0, 0);
    if (component->panel_thread.joinable()) component->panel_thread.join();
    if (component->panel_ready) {
        CloseHandle(component->panel_ready);
        component->panel_ready = nullptr;
    }
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        kavio_virtual_hdmi_control::SetModuleHandle(instance);
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
KavioHostCreate(const KavioHostCreateParams* params, KavioHostHandle* output) {
    if (!params || !output || !params->host ||
        params->struct_size < KAVIO_HOST_CREATE_PARAMS_BASE_SIZE ||
        params->host->struct_size < KAVIO_HOST_API_BASE_SIZE ||
        !params->host->send || !params->host->is_device_online) {
        return KavioResult_InvalidArgument;
    }
    std::unique_ptr<HostComponent> component(new (std::nothrow) HostComponent());
    if (!component) return KavioResult_InternalError;
    component->host = params->host;
    component->channel_ready.store(
        component->host->is_device_online(component->host->context) != 0);
    *output = component.release();
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
KavioHostStart(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    bool expected = false;
    if (!component->running.compare_exchange_strong(expected, true)) {
        return KavioResult_Success;
    }
    component->capture_thread = std::thread(CaptureLoop, component);
    Log(component, KavioHostLogLevel_Info,
        "Virtual HDMI Host Component started; waiting for VirtualDrivers VDD, DXGI capture, and the Device App");
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
KavioHostPrepareStop(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
KavioHostStop(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);
    component->app_ready.store(false);
    if (component->capture_thread.joinable()) component->capture_thread.join();
    ClosePanel(component);
    if (component->driver_operation_thread.joinable()) {
        component->driver_operation_thread.join();
    }
    ResetStreamState(component, true);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL
KavioHostDestroy(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return;
    KavioHostStop(component);
    delete component;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
KavioHostOnData(KavioHostHandle handle, const void* data, std::size_t size) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component || !data || size == 0) return KavioResult_InvalidArgument;
    MessageType type{};
    const std::uint8_t* payload = nullptr;
    std::size_t payload_size = 0;
    if (!ParseMessage(data, size, type, payload, payload_size)) {
        return KavioResult_InvalidArgument;
    }
    switch (type) {
    case MessageType::AppReady:
        HandleAppReady(component, payload, payload_size);
        return KavioResult_Success;
    case MessageType::FrameAck:
        HandleFrameAck(component, payload, payload_size);
        return KavioResult_Success;
    case MessageType::KeyFrameRequest:
        HandleKeyFrameRequest(component, payload, payload_size);
        return KavioResult_Success;
    default:
        return KavioResult_NotSupported;
    }
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL
KavioHostOnDeviceState(KavioHostHandle handle, std::uint32_t ready) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return;
    component->channel_ready.store(ready != 0);
    if (!ready) {
        component->app_ready.store(false);
        component->capture_reset_requested.store(true);
        ResetStreamState(component, true);
    }
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL
KavioHostOpenPanel(KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component || !component->running.load()) return KavioResult_NotReady;

    HWND existing_window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        existing_window = component->panel_window;
    }
    if (existing_window) {
        ShowWindow(existing_window, SW_RESTORE);
        SetForegroundWindow(existing_window);
        return KavioResult_Success;
    }

    // A closed panel can leave a joinable thread. Join it without holding
    // panel_mutex because PanelThreadMain takes the same mutex while
    // clearing panel_window during shutdown.
    if (component->panel_thread.joinable()) component->panel_thread.join();

    HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ready_event) return KavioResult_InternalError;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        if (component->panel_ready) CloseHandle(component->panel_ready);
        component->panel_ready = ready_event;
        component->panel_thread = std::thread(PanelThreadMain, component);
    }
    const DWORD wait = WaitForSingleObject(ready_event, 3000);
    return wait == WAIT_OBJECT_0 ? KavioResult_Success : KavioResult_InternalError;
}
