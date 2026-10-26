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
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// 以下数值只用于本 Demo 的 TX 信息面板，不是 SDK 或 Platform 的全局限制。
// 760x420 为三类指标预留展示空间；若调整，DrawText/FillRect 使用的画布尺寸需保持一致。
// 2 秒查询周期足以演示 Host 指标更新，同时避免每一帧都走 USB Channel 请求。
// 字体放大 2 倍用于小尺寸 5x7 点阵字，避免引入图形字体依赖。
constexpr int kOverlayWidth = 760;
constexpr int kOverlayHeight = 420;
constexpr int kQueryIntervalMs = 2000;
constexpr int kFontScale = 2;

std::atomic<bool> g_stop(false);
std::atomic<bool> g_client_ready(false);
std::atomic<bool> g_channel_ready(false);
std::atomic<uint64_t> g_metrics_generation(0);
std::mutex g_mutex;

// 只控制哪些指标显示，不影响 Host 查询内容；字段名须对应 package/config 中的定义。
struct DisplayConfig {
    // 当用户配置、备份和包内默认配置均不可用时，使用全关闭的安全状态。
    bool show_cpu = false;
    bool show_memory = false;
    bool show_disks = false;
};

// 配置由 App Configuration GET/SET 回调读写；指标数据和配置都可能从 SDK 回调线程更新。
DisplayConfig g_config;
demo_metrics::ResponseMessage g_metrics;
bool g_has_metrics = false;

void OnSignal(int) { g_stop.store(true); }

// Convert the SDK-owned Json::Value snapshot to this Demo's rendering switches.
void ApplyConfig(const Json::Value& json) {
    DisplayConfig next;
    next.show_cpu = json.get("show_cpu", false).asBool();
    next.show_memory = json.get("show_memory", false).asBool();
    next.show_disks = json.get("show_disks", false).asBool();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = next;
    }
    g_metrics_generation.fetch_add(1);
}

// 使用内置 5x7 位图字形，演示 App 无字体库时也可直接绘制 BGRA 图层。
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

// 字体直接写入同一 BGRA8888 缓冲，横向字距由 kFontScale 放大。
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

// 仅在展示层做字节单位换算，通信协议仍携带整数原始字节数。
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

// SDK 将 IPCPackagePb 在单条派发线程中顺序回调；此处仅做协议判断和必要的 TX SHM 打开。
// CreateTxSurface() 的返回值只是本地提交结果，不能据此认为图层可写。
// 图层收到 MSG_P2A_RSP_CREATE_TX_SHM 后才调用 OpenTxSurface()；Open 失败须请求远端销毁。
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
        case ip::MSG_P2A_REQ_SET_APP_CONFIG:
            // The SDK has already committed the value before delivering this event.
            ApplyConfig(client.GetAppConfig());
            break;
        case ip::MSG_P2A_RSP_CREATE_SESSION:
            g_client_ready.store(true);
            g_channel_ready.store(client.IsCommChannelReady());
            break;
        case ip::MSG_P2A_NTF_USB_HOST_STATE:
        case ip::MSG_P2A_NTF_COMM_PEER_STATE:
            g_channel_ready.store(client.IsCommChannelReady());
            break;
        case ip::MSG_P2A_DATA_COMM_CHANNEL: {
            if (!msg.payload().has_p2a_data_comm_channel()) break;
            const auto& bytes = msg.payload().p2a_data_comm_channel().payload();
            if (bytes.size() != sizeof(demo_metrics::ResponseMessage)) break;
            demo_metrics::ResponseMessage message;
            std::memcpy(&message, bytes.data(), sizeof(message));
            if (!demo_metrics::IsValid(message.header,
                    demo_metrics::MessageType::Response, sizeof(message)) ||
                message.disk_count > demo_metrics::kMaxDisks) break;
            { std::lock_guard<std::mutex> lock(g_mutex);
              g_metrics = message; g_has_metrics = true; }
            g_metrics_generation.fetch_add(1);
            break;
        }
        default: break;
        }
    }
private:
    kavio::KavioClient& client;
    std::shared_ptr<kavio::TxSurface>& surface;
    std::atomic<bool>& tx_create_pending;
};
}  // namespace

// 启动连接后从 MSG_P2A_RSP_CREATE_SESSION 获取 Session 可用状态，再异步申请 TX 图层。
int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::shared_ptr<kavio::TxSurface> surface;
    std::atomic<bool> tx_create_pending{false};
    std::unique_ptr<AppMessageHandler> handler;
    kavio::KavioClient client;
    handler = std::make_unique<AppMessageHandler>(client, surface, tx_create_pending);
    kavio::ipc::proto::A2PReqCreateSessionPb options;
    options.set_client_name("demo_communication");
    // 本示例只绘制 TX 信息面板，不消费 HDMI RX 原始帧，所以关闭 RX SHM 订阅。
    // interval=1 为协议合法默认值；RX 关闭时不会实际采样。
    // TX z_order=100 是演示面板的叠放层级，不是所有 App 的推荐固定层级。
    options.mutable_rx_params()->set_enabled(false);
    options.mutable_rx_params()->set_frame_sample_interval(1);
    options.mutable_tx_params()->set_enabled(true);
    options.mutable_tx_params()->set_z_order(100);


    kavio::Status start = client.Start(options, *handler);
    // Demonstration policy: exit on invalid saved configuration, unless the operator
    // explicitly allows recovery by setting KAVIO_DEMO_RESTORE_CONFIG=1.
    if (!start.ok() && std::getenv("KAVIO_DEMO_RESTORE_CONFIG") &&
        std::string(std::getenv("KAVIO_DEMO_RESTORE_CONFIG")) == "1") {
        const auto reset = client.ResetAppConfigToDefaults();
        if (reset.ok()) start = client.Start(options, *handler);
    }
    if (start.ok()) ApplyConfig(client.GetAppConfig());
    if (!start.ok()) {
        std::cerr << "Start KavioClient failed: " << start.message << std::endl;
        return 1;
    }

    uint64_t sequence = 1;
    uint64_t rendered_generation = static_cast<uint64_t>(-1);
    auto next_query = std::chrono::steady_clock::now();

    while (!g_stop.load()) {
        if (g_client_ready.load() && !tx_create_pending.load() &&
            !std::atomic_load(&surface)) {
            tx_create_pending.store(true);
            uint64_t request_id = 0;
            auto status = client.CreateTxSurface(kOverlayWidth, kOverlayHeight, request_id);
            if (!status.ok()) {
                tx_create_pending.store(false);
                std::cerr << "CreateTxSurface: " << status.message << std::endl;
            }
        }
        auto current_surface = std::atomic_load(&surface);

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

        // 只在接收到新指标后重绘，减少无意义的 TX SHM 写入。
        const uint64_t generation = g_metrics_generation.load();
        // generation 变化才重绘，避免 30 ms 主循环无意义地重复写同一 TX 帧。
        if (current_surface && current_surface->IsValid() && generation != rendered_generation) {
            kavio::ImageFrame frame = BuildOverlay(generation + 1);
            if (current_surface->WriteFrame(frame).ok()) rendered_generation = generation;
        }
        // 30ms 仅限制 Demo 主循环空闲轮询频率，不代表设备视频帧率。
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    if (auto final_surface = std::atomic_exchange(&surface, std::shared_ptr<kavio::TxSurface>{})) {
        uint64_t id = 0; final_surface->Destroy(id);
    }
    client.Stop();
    return 0;
}
