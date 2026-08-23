/*
 * demo_communication
 *
 * Device Component 每两秒向 Windows Host Component 发送一次系统指标查询。Host Component 返回
 * Windows CPU、内存和固定磁盘容量；App 按 Config Definition 中的三个显示开关，把
 * 最近结果绘制在 TX 左上角。显示开关只影响渲染，不影响查询和数据返回。
 */

#include <kavio/KavioClient.h>
#include "../common/DemoMetricsProtocol.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kOverlayWidth = 760;
constexpr int kOverlayHeight = 420;
constexpr int kQueryIntervalMs = 2000;
constexpr int kFontScale = 2;

std::atomic<bool> g_stop(false);
std::atomic<bool> g_client_ready(false);
std::atomic<bool> g_channel_ready(false);
std::atomic<uint64_t> g_metrics_generation(0);
std::mutex g_mutex;

struct DisplayConfig {
    bool show_cpu = true;
    bool show_memory = true;
    bool show_disks = true;
};

// 配置由 App Configuration GET/SET 回调读写；指标数据和配置都可能从 SDK 回调线程更新。
DisplayConfig g_config;
demo_metrics::ResponseMessage g_metrics;
bool g_has_metrics = false;

void OnSignal(int) { g_stop.store(true); }

// Platform 为每个已安装 App 提供独立数据目录；不要把持久配置写到当前工作目录。
std::string DataDirectory() {
    const char* value = std::getenv("KAVIO_APP_DATA_DIR");
    return value && *value ? value : ".";
}

bool ReadBoolean(const std::string& json, const char* key, bool& value) {
    const std::string token = std::string("\"") + key + "\"";
    const size_t key_pos = json.find(token);
    if (key_pos == std::string::npos) return false;
    const size_t colon = json.find(':', key_pos + token.size());
    if (colon == std::string::npos) return false;
    const size_t first = json.find_first_not_of(" \t\r\n", colon + 1);
    if (first == std::string::npos) return false;
    if (json.compare(first, 4, "true") == 0) { value = true; return true; }
    if (json.compare(first, 5, "false") == 0) { value = false; return true; }
    return false;
}

// Demo 只解析自己关心的三个布尔字段；Config Definition 与这里的字段名必须同步修改。
bool ParseConfig(const std::string& json, DisplayConfig& config) {
    DisplayConfig next;
    if (!ReadBoolean(json, "show_cpu", next.show_cpu) ||
        !ReadBoolean(json, "show_memory", next.show_memory) ||
        !ReadBoolean(json, "show_disks", next.show_disks)) {
        return false;
    }
    config = next;
    return true;
}

std::string ConfigToJson(const DisplayConfig& config) {
    std::ostringstream out;
    out << "{\n"
        << "  \"show_cpu\": " << (config.show_cpu ? "true" : "false") << ",\n"
        << "  \"show_memory\": " << (config.show_memory ? "true" : "false") << ",\n"
        << "  \"show_disks\": " << (config.show_disks ? "true" : "false") << "\n"
        << "}\n";
    return out.str();
}

std::string ReadStoredConfig() {
    std::ifstream input(DataDirectory() + "/config.json", std::ios::binary);
    if (!input) return std::string();
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

bool SaveConfig(const std::string& json) {
    std::ofstream output(DataDirectory() + "/config.json",
                         std::ios::binary | std::ios::trunc);
    output.write(json.data(), static_cast<std::streamsize>(json.size()));
    return output.good();
}

const uint8_t* Glyph(char value) {
    static const uint8_t blank[7] = {};
#define G(name,a,b,c,d,e,f,g) static const uint8_t name[7]={a,b,c,d,e,f,g}
    G(g0,14,17,19,21,25,17,14); G(g1,4,12,4,4,4,4,14);
    G(g2,14,17,1,2,4,8,31); G(g3,30,1,1,14,1,1,30);
    G(g4,2,6,10,18,31,2,2); G(g5,31,16,16,30,1,1,30);
    G(g6,14,16,16,30,17,17,14); G(g7,31,1,2,4,8,8,8);
    G(g8,14,17,17,14,17,17,14); G(g9,14,17,17,15,1,1,14);
    G(A,14,17,17,31,17,17,17); G(B,30,17,17,30,17,17,30);
    G(C,14,17,16,16,16,17,14); G(D,30,17,17,17,17,17,30);
    G(E,31,16,16,30,16,16,31); G(F,31,16,16,30,16,16,16);
    G(GG,14,17,16,23,17,17,15); G(H,17,17,17,31,17,17,17);
    G(I,14,4,4,4,4,4,14); G(K,17,18,20,24,20,18,17);
    G(L,16,16,16,16,16,16,31); G(M,17,27,21,21,17,17,17);
    G(N,17,25,21,19,17,17,17); G(O,14,17,17,17,17,17,14);
    G(P,30,17,17,30,16,16,16); G(R,30,17,17,30,20,18,17);
    G(S,15,16,16,14,1,1,30); G(T,31,4,4,4,4,4,4);
    G(U,17,17,17,17,17,17,14); G(W,17,17,17,21,21,21,10);
    G(Y,17,17,10,4,4,4,4);
    G(colon,0,4,4,0,4,4,0); G(dot,0,0,0,0,0,4,4);
    G(slash,1,2,2,4,8,8,16); G(percent,17,2,4,8,16,17,0);
    G(dash,0,0,0,31,0,0,0);
#undef G
    if (value >= '0' && value <= '9') {
        static const uint8_t* numbers[] = {g0,g1,g2,g3,g4,g5,g6,g7,g8,g9};
        return numbers[value-'0'];
    }
    switch (value) {
    case 'A': return A; case 'B': return B; case 'C': return C; case 'D': return D;
    case 'E': return E; case 'F': return F; case 'G': return GG; case 'H': return H;
    case 'I': return I; case 'K': return K; case 'L': return L; case 'M': return M;
    case 'N': return N; case 'O': return O; case 'P': return P; case 'R': return R;
    case 'S': return S; case 'T': return T; case 'U': return U; case 'W': return W;
    case 'Y': return Y;
    case ':': return colon; case '.': return dot; case '/': return slash;
    case '%': return percent; case '-': return dash; default: return blank;
    }
}

void PutPixel(std::vector<uint8_t>& image, int x, int y,
              uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    if (x < 0 || y < 0 || x >= kOverlayWidth || y >= kOverlayHeight) return;
    const size_t offset = (static_cast<size_t>(y) * kOverlayWidth + x) * 4;
    image[offset] = b; image[offset+1] = g; image[offset+2] = r; image[offset+3] = a;
}

void FillRect(std::vector<uint8_t>& image, int x, int y, int width, int height,
              uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    for (int row = 0; row < height; ++row)
        for (int col = 0; col < width; ++col)
            PutPixel(image, x + col, y + row, b, g, r, a);
}

void DrawText(std::vector<uint8_t>& image, int x, int y, const std::string& text) {
    for (char raw : text) {
        const char ch = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
        const uint8_t* glyph = Glyph(ch);
        for (int row = 0; row < 7; ++row) {
            for (int col = 0; col < 5; ++col) {
                if ((glyph[row] & (1u << (4-col))) == 0) continue;
                FillRect(image, x + col*kFontScale, y + row*kFontScale,
                         kFontScale, kFontScale, 255, 255, 255, 255);
            }
        }
        x += 6 * kFontScale;
    }
}

std::string BytesText(uint64_t value) {
    const double gb = static_cast<double>(value) / (1024.0 * 1024.0 * 1024.0);
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << gb << " GB";
    return out.str();
}

// BuildOverlay 只消费加锁后的状态快照，绘制过程不持有全局 mutex，避免阻塞 SDK 回调。
kavio::ImageFrame BuildOverlay(uint64_t frame_id) {
    DisplayConfig config;
    demo_metrics::ResponseMessage metrics;
    bool has_metrics = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        config = g_config;
        metrics = g_metrics;
        has_metrics = g_has_metrics;
    }

    kavio::ImageFrame frame;
    frame.frame_id = frame_id;
    frame.width = kOverlayWidth;
    frame.height = kOverlayHeight;
    frame.stride = kOverlayWidth * 4;
    frame.pixel_format = kavio::PixelFormat::kBGRA8888;
    frame.dest_pos_x = 0;
    frame.dest_pos_y = 0;
    frame.valid_rect.width = frame.width;
    frame.valid_rect.height = frame.height;
    frame.data.assign(static_cast<size_t>(frame.stride) * frame.height, 0);

    int y = 16;
    if (!has_metrics) {
        FillRect(frame.data, 8, 8, 330, 34, 0, 0, 0, 160);
        DrawText(frame.data, 16, y, "WAITING FOR HOST");
        return frame;
    }

    std::vector<std::string> lines;
    if (config.show_cpu) {
        std::ostringstream out;
        out << "CPU: " << std::fixed << std::setprecision(1)
            << metrics.cpu_usage_percent << "%";
        lines.push_back(out.str());
    }
    if (config.show_memory) {
        lines.push_back("MEMORY: " + BytesText(metrics.memory_used_bytes) + " / " +
                        BytesText(metrics.memory_total_bytes));
    }
    if (config.show_disks) {
        for (uint32_t i = 0; i < metrics.disk_count && i < demo_metrics::kMaxDisks; ++i) {
            lines.push_back(std::string(metrics.disks[i].name) + ": " +
                BytesText(metrics.disks[i].used_bytes) + " / " +
                BytesText(metrics.disks[i].total_bytes));
        }
    }
    if (lines.empty()) return frame;

    FillRect(frame.data, 8, 8, kOverlayWidth - 16,
             static_cast<int>(lines.size()) * 24 + 16, 0, 0, 0, 160);
    for (const std::string& line : lines) {
        DrawText(frame.data, 16, y, line);
        y += 24;
    }
    return frame;
}

}  // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    const std::string stored = ReadStoredConfig();
    DisplayConfig loaded;
    if (!stored.empty() && ParseConfig(stored, loaded)) g_config = loaded;

    kavio::KavioClient client;
    kavio::SessionOptions options;
    options.client_name = "demo_communication";
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
    // Channel 是原始数据报通道：每次回调对应 Host 发来的一条完整业务消息。
    callbacks.on_comm_channel_data = [](const std::vector<uint8_t>& data) {
        if (data.size() != sizeof(demo_metrics::ResponseMessage)) return;
        demo_metrics::ResponseMessage message;
        std::memcpy(&message, data.data(), sizeof(message));
        if (!demo_metrics::IsValid(message.header,
                demo_metrics::MessageType::Response, sizeof(message)) ||
            message.disk_count > demo_metrics::kMaxDisks) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_metrics = message;
            g_has_metrics = true;
        }
        g_metrics_generation.fetch_add(1);
    };
    // App Configuration 请求来自 Windows 端。SET 只有在本地持久化成功后才更新实际生效配置。
    callbacks.on_app_config_request = [&client](
        const kavio::KavioClient::AppConfigRequest& request) {
        kavio::KavioClient::AppConfigResponse response;
        response.set_request_id(request.request_id());
        response.set_operation(request.operation());
        if (request.operation() == kavio::ipc::proto::APP_CONFIG_OPERATION_GET) {
            std::lock_guard<std::mutex> lock(g_mutex);
            response.set_config_json(ConfigToJson(g_config));
        } else if (request.operation() == kavio::ipc::proto::APP_CONFIG_OPERATION_SET) {
            DisplayConfig next;
            if (!ParseConfig(request.config_json(), next)) {
                response.set_error_code(static_cast<int32_t>(
                    kavio::StatusCode::Common_InvalidArgument));
                response.set_error_message("invalid display configuration JSON");
            } else {
                const std::string effective = ConfigToJson(next);
                if (!SaveConfig(effective)) {
                    response.set_error_code(static_cast<int32_t>(
                        kavio::StatusCode::Common_InternalError));
                    response.set_error_message("save configuration failed");
                } else {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_config = next;
                    response.set_config_json(effective);
                    g_metrics_generation.fetch_add(1);
                }
            }
        } else {
            response.set_error_code(static_cast<int32_t>(
                kavio::StatusCode::Common_Unsupported));
            response.set_error_message("unsupported App Configuration operation");
        }
        client.SendAppConfigResponse(response);
    };

    const kavio::Status start = client.Start(options, std::move(callbacks));
    if (!start.ok()) {
        std::cerr << "Start KavioClient failed: " << start.message << std::endl;
        return 1;
    }

    kavio::TxSurface surface;
    uint64_t sequence = 1;
    uint64_t rendered_generation = static_cast<uint64_t>(-1);
    auto next_query = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        if (g_client_ready.load() && !surface.IsValid()) {
            const kavio::Status status = client.CreateTxSurface(
                kOverlayWidth, kOverlayHeight, surface, std::chrono::seconds(2));
            if (!status.ok()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        const auto now = std::chrono::steady_clock::now();
        // 查询节拍与渲染节拍解耦：没有 Channel 时不发送，请求周期仍从当前时刻重新计时。
        if (now >= next_query) {
            if (g_channel_ready.load()) {
                demo_metrics::QueryMessage query;
                demo_metrics::Initialize(query, sequence++);
                client.SendCommChannelData(&query, sizeof(query));
            }
            next_query = now + std::chrono::milliseconds(kQueryIntervalMs);
        }

        const uint64_t generation = g_metrics_generation.load();
        // generation 变化才重绘，避免 30 ms 主循环无意义地重复写同一 TX 帧。
        if (surface.IsValid() && generation != rendered_generation) {
            kavio::ImageFrame frame = BuildOverlay(generation + 1);
            if (surface.WriteFrame(frame).ok()) rendered_generation = generation;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (surface.IsValid()) surface.Destroy(std::chrono::seconds(2));
    client.Stop();
    return 0;
}
