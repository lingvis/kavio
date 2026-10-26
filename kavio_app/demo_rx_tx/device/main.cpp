/*
 * demo_rx_tx - Kavio C++ SDK 综合设备端示例。
 *
 * 这个文件刻意把生命周期、RX 主动拉取、双 TX Surface、终端输入和一个简单
 * FSM 放在同一可运行程序中，便于新开发者从端到端流程开始调试。实际产品通常
 * 应按职责拆分这些模块，而不是继续扩展这个单文件 Demo。
 */

#include <kavio/KavioClient.h>


#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <csignal>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <random>
#include <string>
#include <termios.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <vector>
#include <deque>
#include <functional>
#include <map>
#include <utility>

/*
================================================================================
Kavio KavioClient Demo
--------------------------------------------------------------------------------
本 Demo 展示了如何使用 KavioClient 与平台建立会话，并通过 RX/TX SHM 完成：
1. 等待 RX SHM ready 后读取图像
2. 生成简单的 BGRA overlay
3. 通过 TxSurface 将 overlay 写入平台

重要说明：
- KavioClient 对外提供 ClientReady / ClientInvalid 生命周期回调，以及 runtime / RX 变化回调。
- 本 Demo 在 ClientReady 后进入轻量轮询拉帧；runtime / RX 回调用于状态观测和快速唤醒。

Demo 工作流程：
1. 启动 KavioClient
2. 会话 ready 后显式启用 RX frame，并查询 stats
3. 创建两个 TX surface 用于显示 overlay
4. 应用周期性调用 ReadLatestRxFrame()
5. 生成 overlay 并写入 TX surface
6. 收到 Ctrl+C / SIGTERM 后清理退出
================================================================================
*/

/*
================================================================================
编译宏说明
--------------------------------------------------------------------------------
1. KAVIO_DEMO_RX_POLL_INTERVAL_MS
    - 作用：主动拉取模式下，两次 RX 图像轮询之间的最小间隔

================================================================================
*/

#ifndef KAVIO_DEMO_RX_POLL_INTERVAL_MS
// 33ms 是本 Demo 主动拉取 RX 最新帧的近似周期（约 30 次/秒），
// 不是 Platform VENC 的编码帧率，也不影响 RX 原始输入节奏。
#define KAVIO_DEMO_RX_POLL_INTERVAL_MS 33
#endif


namespace {

// 以下 Badge 速度、尺寸、数量及圆角只决定两个演示 TX 图层的视觉效果。
// 速度是每次状态更新的位移基数，不是 SDK 的图层更新限速。
// 两个图层用于演示多图层叠放和独立 SHM 资源生命周期。
constexpr float kDemoBadgeSpeedPx = 7.0f;
constexpr int kDemoLabelCount = 2;
constexpr int kDemoBadgeBaseWidth = 320;
constexpr int kDemoBadgeBaseHeight = 104;
constexpr int kDemoBadgeCornerRadius = 18;


struct Size {
    int width = 0;
    int height = 0;
};

struct PointF {
    float x = 0.0f;
    float y = 0.0f;

    PointF& operator+=(const PointF& other) {
        x += other.x;
        y += other.y;
        return *this;
    }
};

PointF operator*(const PointF& value, float scale) {
    return PointF{value.x * scale, value.y * scale};
}

struct BgraColor {
    uint8_t b = 0;
    uint8_t g = 0;
    uint8_t r = 0;
    uint8_t a = 0;
};

struct DemoBadgeState {
    bool initialized = false;
    int frame_width = 0;
    int frame_height = 0;
    PointF center;
    PointF velocity;
    std::mt19937 rng{std::random_device{}()};
};

std::atomic<bool> g_stop{false};

DemoBadgeState g_demo_badge_state;
DemoBadgeState g_demo_badge_state_aux;

// 交互输入：50ms 等待用于降低终端空闲轮询；10ms 用于收齐转义序列尾部。
// 鼠标步长 40px 和默认 1920x1080 是终端演示操作参数，不代表真实屏幕分辨率。
// 实际 RX 边界可用时应以 GetRxInfo() 更新鼠标活动范围。
constexpr int kInteractiveInputPollMs = 50;
constexpr int kInteractiveSequenceTailPollMs = 10;
constexpr int kInteractiveMouseStepPx = 40;
constexpr int kInteractiveMouseDefaultWidth = 1920;
constexpr int kInteractiveMouseDefaultHeight = 1080;
constexpr size_t kInteractiveEscapeMaxBytes = 16;

enum class TerminalActionKind {
    kNone,
    kKeyPress,
    kMouseMove,
};

struct TerminalAction {
    TerminalActionKind kind = TerminalActionKind::kNone;
    kavio::input::proto::InputKeyCodePb key_code = kavio::input::proto::KC_RESERVED;
    bool shift = false;
    int32_t mouse_dx = 0;
    int32_t mouse_dy = 0;
};

// RAII 维护非规范终端输入；退出时恢复 termios，避免 Demo 异常结束后终端回显失效。
class TerminalInputGuard {
public:
    bool Enable(int fd, std::string* error) {
        if (!::isatty(fd)) {
            if (error) {
                *error = "stdin is not a TTY";
            }
            return false;
        }

        fd_ = fd;
        if (::tcgetattr(fd_, &saved_) != 0) {
            if (error) {
                *error = std::strerror(errno);
            }
            return false;
        }

        struct termios raw = saved_;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;

        if (::tcsetattr(fd_, TCSANOW, &raw) != 0) {
            if (error) {
                *error = std::strerror(errno);
            }
            return false;
        }

        active_ = true;
        return true;
    }

    ~TerminalInputGuard() {
        Restore();
    }

    void Restore() {
        if (!active_) {
            return;
        }
        ::tcsetattr(fd_, TCSANOW, &saved_);
        active_ = false;
    }

private:
    int fd_ = -1;
    bool active_ = false;
    struct termios saved_ {};
};

void OnSignal(int) {
    g_stop.store(true);
}

const char* RuntimeVideoStatusName(kavio::runtime::proto::RuntimeVideoStatusPb status) {
    switch (status) {
        case kavio::runtime::proto::RUNTIME_VIDEO_STATUS_NOT_CONNECTED: return "not-connected";
        case kavio::runtime::proto::RUNTIME_VIDEO_STATUS_CONNECTED: return "connected";
        case kavio::runtime::proto::RUNTIME_VIDEO_STATUS_RUNNING: return "running";
        case kavio::runtime::proto::RUNTIME_VIDEO_STATUS_STOPPED: return "stopped";
        case kavio::runtime::proto::RUNTIME_VIDEO_STATUS_ERROR: return "error";
        case kavio::runtime::proto::RUNTIME_VIDEO_STATUS_UNKNOWN:
        default:
            return "unknown";
    }
}

const char* InputModeName(kavio::input::proto::InputModePb mode) {
    switch (mode) {
        case kavio::input::proto::INPUT_MODE_BLE: return "ble";
        case kavio::input::proto::INPUT_MODE_RF24: return "2.4g";
        case kavio::input::proto::INPUT_MODE_USB: return "usb";
        default: return "unknown";
    }
}

const char* InputChannelName(kavio::input::proto::InputChannelPb channel) {
    switch (channel) {
        case kavio::input::proto::INPUT_CHANNEL_1: return "channel1";
        case kavio::input::proto::INPUT_CHANNEL_2: return "channel2";
        case kavio::input::proto::INPUT_CHANNEL_3: return "channel3";
        default: return "unknown";
    }
}

const char* InputConnectionStatusName(kavio::input::proto::InputConnectionStatusPb state) {
    switch (state) {
        case kavio::input::proto::INPUT_CONNECTION_STATUS_SYNCING: return "syncing";
        case kavio::input::proto::INPUT_CONNECTION_STATUS_PAIRING: return "pairing";
        case kavio::input::proto::INPUT_CONNECTION_STATUS_CONNECT: return "connected";
        case kavio::input::proto::INPUT_CONNECTION_STATUS_DISCONNECT: return "disconnected";
        default: return "unknown";
    }
}

const char* ClientAvailabilityName(bool ready) {
    return ready ? "ready" : "invalid";
}

const char* RxShmInvalidReasonName(kavio::ipc::proto::RxShmInvalidReasonPb reason) {
    switch (reason) {
        case kavio::ipc::proto::RX_SHM_INVALID_REASON_DISABLED: return "disabled";
        case kavio::ipc::proto::RX_SHM_INVALID_REASON_SESSION_CLOSED: return "session-closed";
        case kavio::ipc::proto::RX_SHM_INVALID_REASON_UNKNOWN:
        default:
            return "unknown";
    }
}

void PrintRuntimeData(const kavio::KavioClient::RuntimeData& runtime_data) {
    if (runtime_data.has_rx_info()) {
        const auto& rx = runtime_data.rx_info();
        std::cout << "[RX] status=" << RuntimeVideoStatusName(rx.status())
                  << " size=" << rx.width() << "x" << rx.height()
                  << " fps=" << rx.fps() << std::endl;
    }
    if (runtime_data.has_tx_info()) {
        const auto& tx = runtime_data.tx_info();
        std::cout << "[TX] status=" << RuntimeVideoStatusName(tx.status())
                  << " size=" << tx.width() << "x" << tx.height()
                  << " fps=" << tx.fps() << std::endl;
    }
    if (runtime_data.has_input()) {
        const auto& input = runtime_data.input();
        std::cout << "[INPUT] mode=" << InputModeName(input.mode())
                  << " channel=" << InputChannelName(input.channel())
                  << " state=" << InputConnectionStatusName(input.connection_status())
                  << std::endl;
    }
}

void PrintStatsReply(const kavio::KavioClient::StatsReply& stats) {
    std::cout << "[STATS]"
              << " sessions=" << stats.global().current_session_count()
              << " input=" << stats.global().current_input_width() << "x"
              << stats.global().current_input_height()
              << " output=" << stats.global().current_output_width() << "x"
              << stats.global().current_output_height()
              << " rx_frames=" << stats.global().rx_frames()
              << " tx_frames=" << stats.global().tx_frames()
              << std::endl;

    for (const auto& session : stats.sessions()) {
        std::cout << "  session id=" << session.session_id()
                  << " name=" << session.client_name()
                  << " pid=" << session.client_pid()
                  << " z=" << session.z_order()
                  << " rx/tx=" << (session.rx_stream_enable() ? "Rx" : "-")
                  << "/" << (session.tx_stream_enable() ? "Tx" : "-")
                  << " rx_n=" << session.rx_frame_sample_interval()
                  << " rx_written=" << session.rx_frames_written()
                  << " tx_updates=" << session.tx_updates_seen()
                  << std::endl;
    }
}

Size GetDemoBadgeSize(const Size& frame_size);

Size GetInitialTxSurfaceSize(const kavio::KavioClient::RuntimeData& runtime_data) {
    if (runtime_data.has_rx_info() &&
        runtime_data.rx_info().width() > 0 &&
        runtime_data.rx_info().height() > 0) {
        return GetDemoBadgeSize(Size{static_cast<int>(runtime_data.rx_info().width()),
                                     static_cast<int>(runtime_data.rx_info().height())});
    }

    if (runtime_data.has_tx_info() &&
        runtime_data.tx_info().width() > 0 &&
        runtime_data.tx_info().height() > 0) {
        return GetDemoBadgeSize(Size{static_cast<int>(runtime_data.tx_info().width()),
                                     static_cast<int>(runtime_data.tx_info().height())});
    }

    return Size{kDemoBadgeBaseWidth, kDemoBadgeBaseHeight};
}

PointF NormalizeVector(const PointF& value) {
    const float length = std::sqrt(value.x * value.x + value.y * value.y);
    if (length < 1e-4f) {
        return PointF{1.0f, 0.0f};
    }
    return PointF{value.x / length, value.y / length};
}

PointF RandomUnitVector(std::mt19937& rng) {
    constexpr float kPi = 3.14159265358979323846f;
    std::uniform_real_distribution<float> angle_dist(0.0f, 2.0f * kPi);
    const float angle = angle_dist(rng);
    return PointF{std::cos(angle), std::sin(angle)};
}

PointF RandomInboundDirection(std::mt19937& rng,
                              bool require_positive_x,
                              bool require_negative_x,
                              bool require_positive_y,
                              bool require_negative_y) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const auto dir = RandomUnitVector(rng);
        if (require_positive_x && dir.x <= 0.15f) continue;
        if (require_negative_x && dir.x >= -0.15f) continue;
        if (require_positive_y && dir.y <= 0.15f) continue;
        if (require_negative_y && dir.y >= -0.15f) continue;
        return dir;
    }

    PointF fallback;
    fallback.x = require_positive_x ? 1.0f : (require_negative_x ? -1.0f : 0.6f);
    fallback.y = require_positive_y ? 1.0f : (require_negative_y ? -1.0f : 0.4f);
    return NormalizeVector(fallback);
}

// 取 RX 尺寸的约 1/4 宽、1/8 高，但限制为 220~320 和 72~104，
// 目的在于小分辨率下仍可读，大分辨率下不让 Badge 无限放大。
Size GetDemoBadgeSize(const Size& frame_size) {
    const int width = std::min(kDemoBadgeBaseWidth, std::max(220, frame_size.width / 4));
    const int height = std::min(kDemoBadgeBaseHeight, std::max(72, frame_size.height / 8));
    return Size{width, height};
}

// 根据 RX 帧大小限制 Badge 尺寸，防止在小分辨率下遮挡整张画面。
void ResetDemoBadgeState(DemoBadgeState& state, const Size& frame_size) {
    state.initialized = true;
    state.frame_width = frame_size.width;
    state.frame_height = frame_size.height;
    std::uniform_real_distribution<float> x_dist(frame_size.width * 0.2f,
                                                 frame_size.width * 0.8f);
    std::uniform_real_distribution<float> y_dist(frame_size.height * 0.2f,
                                                 frame_size.height * 0.8f);
    state.center = PointF{x_dist(state.rng), y_dist(state.rng)};
    state.velocity = RandomUnitVector(state.rng) * kDemoBadgeSpeedPx;
}

// 每次更新先计算合法活动区域再反弹；不依赖固定输入分辨率。
void AdvanceDemoBadgeState(DemoBadgeState& state,
                           const Size& frame_size,
                           const Size& badge_size) {
    if (!state.initialized ||
        state.frame_width != frame_size.width ||
        state.frame_height != frame_size.height) {
        ResetDemoBadgeState(state, frame_size);
    }

    const float half_w = badge_size.width * 0.5f;
    const float half_h = badge_size.height * 0.5f;
    state.center += state.velocity;

    bool hit_left = false;
    bool hit_right = false;
    bool hit_top = false;
    bool hit_bottom = false;

    if (state.center.x < half_w) {
        state.center.x = half_w;
        hit_left = true;
    } else if (state.center.x > frame_size.width - half_w) {
        state.center.x = static_cast<float>(frame_size.width) - half_w;
        hit_right = true;
    }
    if (state.center.y < half_h) {
        state.center.y = half_h;
        hit_top = true;
    } else if (state.center.y > frame_size.height - half_h) {
        state.center.y = static_cast<float>(frame_size.height) - half_h;
        hit_bottom = true;
    }

    if (hit_left || hit_right || hit_top || hit_bottom) {
        state.velocity = RandomInboundDirection(
            state.rng, hit_left, hit_right, hit_top, hit_bottom) * kDemoBadgeSpeedPx;
    }
}

bool IsInsideRoundedRect(int x, int y, int width, int height, int radius) {
    if (x < 0 || y < 0 || x >= width || y >= height) return false;
    const int safe_radius = std::min(radius, std::min(width / 2, height / 2));
    if (x >= safe_radius && x < width - safe_radius) return true;
    if (y >= safe_radius && y < height - safe_radius) return true;

    const int center_x = x < safe_radius ? safe_radius : width - safe_radius - 1;
    const int center_y = y < safe_radius ? safe_radius : height - safe_radius - 1;
    const int dx = x - center_x;
    const int dy = y - center_y;
    return dx * dx + dy * dy <= safe_radius * safe_radius;
}

void SetBgraPixel(kavio::ImageFrame& image, int x, int y, const BgraColor& color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(image.width) ||
        y >= static_cast<int>(image.height)) {
        return;
    }
    const size_t offset = static_cast<size_t>(y) * image.stride +
                          static_cast<size_t>(x) * 4u;
    if (offset + 3 >= image.data.size()) return;
    image.data[offset + 0] = color.b;
    image.data[offset + 1] = color.g;
    image.data[offset + 2] = color.r;
    image.data[offset + 3] = color.a;
}

// BGRA8888 软件绘制演示：valid_rect/dest_pos 与完整 TX 画布是两套坐标语义。
void DrawBadgeCanvas(kavio::ImageFrame& image,
                     uint64_t tick,
                     const BgraColor& fill_color) {
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    constexpr int kOutlineWidth = 2;
    const BgraColor outline{255, 255, 255, 220};
    const BgraColor accent{255, 255, 255, 255};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!IsInsideRoundedRect(x, y, width, height, kDemoBadgeCornerRadius)) continue;
            const bool inner = IsInsideRoundedRect(x - kOutlineWidth,
                                                    y - kOutlineWidth,
                                                    width - 2 * kOutlineWidth,
                                                    height - 2 * kOutlineWidth,
                                                    std::max(0, kDemoBadgeCornerRadius - kOutlineWidth));
            SetBgraPixel(image, x, y, inner ? fill_color : outline);
        }
    }

    // A moving white marker keeps the TX surface visibly changing every frame
    // without depending on an image/font library.
    if (width > 32 && height > 20) {
        const int track_left = 16;
        const int track_right = width - 16;
        const int marker_x = track_left + static_cast<int>(tick %
            static_cast<uint64_t>(std::max(1, track_right - track_left)));
        const int marker_y = height / 2;
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                if (dx * dx + dy * dy <= 9) {
                    SetBgraPixel(image, marker_x + dx, marker_y + dy, accent);
                }
            }
        }
    }
}

kavio::Rect ComputeMovingBadgeRect(DemoBadgeState& state,
                                   const Size& motion_bounds,
                                   const Size& badge_size) {
    AdvanceDemoBadgeState(state, motion_bounds, badge_size);

    kavio::Rect rect;
    rect.pos_x = static_cast<int32_t>(std::round(state.center.x - badge_size.width * 0.5f));
    rect.pos_y = static_cast<int32_t>(std::round(state.center.y - badge_size.height * 0.5f));
    rect.width = static_cast<uint32_t>(badge_size.width);
    rect.height = static_cast<uint32_t>(badge_size.height);
    return rect;
}

kavio::ImageFrame MakeOverlay(const kavio::TxSurface& tx_surface,
                              const kavio::ImageFrame& src,
                              uint64_t tick,
                              DemoBadgeState& badge_state,
                              const BgraColor& fill_color) {
    kavio::ImageFrame overlay;
    overlay.frame_id = src.frame_id;
    overlay.timestamp_ns = src.timestamp_ns;
    overlay.width = tx_surface.width();
    overlay.height = tx_surface.height();
    overlay.stride = tx_surface.stride();
    overlay.pixel_format = tx_surface.pixel_format();
    overlay.data.assign(static_cast<size_t>(overlay.stride) * overlay.height, 0);

    const Size output_size{static_cast<int>(src.width), static_cast<int>(src.height)};
    const Size badge_size{static_cast<int>(overlay.width), static_cast<int>(overlay.height)};
    const kavio::Rect dst_rect = ComputeMovingBadgeRect(badge_state, output_size, badge_size);
    overlay.dest_pos_x = dst_rect.pos_x;
    overlay.dest_pos_y = dst_rect.pos_y;
    overlay.valid_rect = kavio::Rect{0, 0, overlay.width, overlay.height};

    DrawBadgeCanvas(overlay, tick, fill_color);
    return overlay;
}

struct DemoTxLayer {
    kavio::TxSurface tx_surface;
    DemoBadgeState* badge_state = nullptr;
    BgraColor fill_color;
};


// FSM 将连接准备、异步图层 Setup、主循环和清理拆开：
// 提交 IPC 创建请求之后必须等待 OnMessage 的创建结果，不能直接转入 Running。
enum FSM_State {
    FSM_State_Start = 0,
    FSM_State_WaitClientReady,
    FSM_State_Setup,
    FSM_State_Running,
    FSM_State_Cleanup,
    FSM_State_Exit,
};

enum FSM_Event {
    FSM_Event_None = 0,
    FSM_Event_StartSubmitted,
    FSM_Event_SetupOk,
    FSM_Event_SetupFailed,
    FSM_Event_StopRequested,
    FSM_Event_ClientReady,
    FSM_Event_ClientInvalid,
    FSM_Event_CleanupDone,
};

const char* FsmStateName(FSM_State state) {
    switch (state) {
        case FSM_State_Start: return "Start";
        case FSM_State_WaitClientReady: return "WaitClientReady";
        case FSM_State_Setup: return "Setup";
        case FSM_State_Running: return "Running";
        case FSM_State_Cleanup: return "Cleanup";
        case FSM_State_Exit: return "Exit";
        default: return "Unknown";
    }
}

const char* FsmEventName(FSM_Event event) {
    switch (event) {
        case FSM_Event_None: return "None";
        case FSM_Event_StartSubmitted: return "StartSubmitted";
        case FSM_Event_SetupOk: return "SetupOk";
        case FSM_Event_SetupFailed: return "SetupFailed";
        case FSM_Event_StopRequested: return "StopRequested";
        case FSM_Event_ClientReady: return "ClientReady";
        case FSM_Event_ClientInvalid: return "ClientInvalid";
        case FSM_Event_CleanupDone: return "CleanupDone";
        default: return "Unknown";
    }
}

struct FSMEventPayload {
    FSM_Event type = FSM_Event_None;

    explicit FSMEventPayload(FSM_Event t = FSM_Event_None) : type(t) {}
};

// 单线程消费 FSM 事件，OnMessage 只投递事件以减少 IPC 派发线程中的状态机重入。
class FiniteStateMachine {
public:
    struct StateHooks {
        std::function<void()> OnEnter;
        std::function<void()> OnLoop;
        std::function<void()> OnExit;
    };

    void RegisterState(FSM_State state, StateHooks hooks) {
        states_[state] = std::move(hooks);
    }

    bool RegisterTransition(FSM_State from, FSM_Event event, FSM_State to) {
        if (from == to) {
            std::cerr << "[FSM][ERROR] same-state transition is not allowed"
                      << " state=" << FsmStateName(from)
                      << " event=" << FsmEventName(event) << std::endl;
            return false;
        }
        transitions_[std::make_pair(from, event)] = to;
        return true;
    }

    void Start(FSM_State initial_state) {
        started_ = true;
        stop_requested_ = false;
        current_state_ = initial_state;
        CallEnter(current_state_);
    }

    void Stop() {
        if (!started_ || stop_requested_) {
            return;
        }

        stop_requested_ = true;
        PostEvent(FSM_Event_StopRequested);
    }

    void PostEvent(FSM_Event event) {
        PostEvent(FSMEventPayload(event));
    }

    void PostEvent(FSMEventPayload event) {
        {
            std::lock_guard<std::mutex> lk(event_mutex_);
            events_.push_back(std::move(event));
        }
    }

    void OnLoop() {
        if (!started_) {
            return;
        }

        for (;;) {
            FSMEventPayload event;
            {
                std::lock_guard<std::mutex> lk(event_mutex_);
                if (events_.empty()) {
                    break;
                }
                event = std::move(events_.front());
                events_.pop_front();
            }

            DispatchTransition(event.type);
        }

        auto it = states_.find(current_state_);
        if (it != states_.end() && it->second.OnLoop) {
            it->second.OnLoop();
        }
    }

    FSM_State CurrentState() const {
        return current_state_;
    }

    bool IsInState(FSM_State state) const {
        return started_ && current_state_ == state;
    }

private:
    void DispatchTransition(FSM_Event event) {
        const auto key = std::make_pair(current_state_, event);
        const auto it = transitions_.find(key);
        if (it == transitions_.end()) {
            return;
        }

        const FSM_State next = it->second;
        if (next == current_state_) {
            return;
        }

        std::cout << "[FSM][TRANSITION] event=" << FsmEventName(event)
                  << " path=" << FsmStateName(current_state_)
                  << " -> " << FsmStateName(next) << std::endl;

        CallExit(current_state_);
        current_state_ = next;
        CallEnter(current_state_);
    }

    void CallEnter(FSM_State state) {
        const auto it = states_.find(state);
        if (it != states_.end() && it->second.OnEnter) {
            it->second.OnEnter();
        }
    }

    void CallExit(FSM_State state) {
        const auto it = states_.find(state);
        if (it != states_.end() && it->second.OnExit) {
            it->second.OnExit();
        }
    }

private:
    bool started_ = false;
    bool stop_requested_ = false;
    FSM_State current_state_ = FSM_State_Start;
    std::map<FSM_State, StateHooks> states_;
    std::map<std::pair<FSM_State, FSM_Event>, FSM_State> transitions_;
    std::mutex event_mutex_;
    std::deque<FSMEventPayload> events_;
};

// 长等待分成 20ms 片段，以便 Ctrl+C/终止信号能及时退出。
bool SleepWithStopCheck(std::chrono::milliseconds duration,
                        const std::atomic<bool>* local_stop = nullptr) {
    constexpr auto kSlice = std::chrono::milliseconds(20);
    auto elapsed = std::chrono::milliseconds(0);
    while (elapsed < duration) {
        if (g_stop.load()) {
            return false;
        }
        if (local_stop && local_stop->load()) {
            return false;
        }
        const auto wait = std::min(kSlice, duration - elapsed);
        std::this_thread::sleep_for(wait);
        elapsed += wait;
    }
    return !g_stop.load() && !(local_stop && local_stop->load());
}

// 使用单调时钟生成输入事件时间戳；不要用系统日历时间以免时间跳变。
uint64_t NowNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// 把终端 ASCII 字符映射到协议物理按键；需要 Shift 的符号另行标记。
bool MapAsciiToKey(char ch,
                   kavio::input::proto::InputKeyCodePb* key_code,
                   bool* needs_shift) {
    if (!key_code || !needs_shift) {
        return false;
    }

    *needs_shift = false;

    if (ch >= 'a' && ch <= 'z') {
        *key_code = static_cast<kavio::input::proto::InputKeyCodePb>(
            static_cast<int>(kavio::input::proto::KC_A) + (ch - 'a'));
        return true;
    }

    if (ch >= 'A' && ch <= 'Z') {
        *key_code = static_cast<kavio::input::proto::InputKeyCodePb>(
            static_cast<int>(kavio::input::proto::KC_A) + (ch - 'A'));
        *needs_shift = true;
        return true;
    }

    if (ch >= '1' && ch <= '9') {
        *key_code = static_cast<kavio::input::proto::InputKeyCodePb>(
            static_cast<int>(kavio::input::proto::KC_1) + (ch - '1'));
        return true;
    }

    switch (ch) {
    case '0': *key_code = kavio::input::proto::KC_0; return true;
    case ' ': *key_code = kavio::input::proto::KC_SPACE; return true;
    case '-': *key_code = kavio::input::proto::KC_MINUS; return true;
    case '=': *key_code = kavio::input::proto::KC_EQUAL; return true;
    case '[': *key_code = kavio::input::proto::KC_LBRACKET; return true;
    case ']': *key_code = kavio::input::proto::KC_RBRACKET; return true;
    case '\\': *key_code = kavio::input::proto::KC_BSLASH; return true;
    case ';': *key_code = kavio::input::proto::KC_SCOLON; return true;
    case '\'': *key_code = kavio::input::proto::KC_QUOTE; return true;
    case '`': *key_code = kavio::input::proto::KC_GRAVE; return true;
    case ',': *key_code = kavio::input::proto::KC_COMMA; return true;
    case '.': *key_code = kavio::input::proto::KC_PERIOD; return true;
    case '/': *key_code = kavio::input::proto::KC_SLASH; return true;
    case '!': *key_code = kavio::input::proto::KC_1; *needs_shift = true; return true;
    case '@': *key_code = kavio::input::proto::KC_2; *needs_shift = true; return true;
    case '#': *key_code = kavio::input::proto::KC_3; *needs_shift = true; return true;
    case '$': *key_code = kavio::input::proto::KC_4; *needs_shift = true; return true;
    case '%': *key_code = kavio::input::proto::KC_5; *needs_shift = true; return true;
    case '^': *key_code = kavio::input::proto::KC_6; *needs_shift = true; return true;
    case '&': *key_code = kavio::input::proto::KC_7; *needs_shift = true; return true;
    case '*': *key_code = kavio::input::proto::KC_8; *needs_shift = true; return true;
    case '(': *key_code = kavio::input::proto::KC_9; *needs_shift = true; return true;
    case ')': *key_code = kavio::input::proto::KC_0; *needs_shift = true; return true;
    case '_': *key_code = kavio::input::proto::KC_MINUS; *needs_shift = true; return true;
    case '+': *key_code = kavio::input::proto::KC_EQUAL; *needs_shift = true; return true;
    case '{': *key_code = kavio::input::proto::KC_LBRACKET; *needs_shift = true; return true;
    case '}': *key_code = kavio::input::proto::KC_RBRACKET; *needs_shift = true; return true;
    case '|': *key_code = kavio::input::proto::KC_BSLASH; *needs_shift = true; return true;
    case ':': *key_code = kavio::input::proto::KC_SCOLON; *needs_shift = true; return true;
    case '"': *key_code = kavio::input::proto::KC_QUOTE; *needs_shift = true; return true;
    case '~': *key_code = kavio::input::proto::KC_GRAVE; *needs_shift = true; return true;
    case '<': *key_code = kavio::input::proto::KC_COMMA; *needs_shift = true; return true;
    case '>': *key_code = kavio::input::proto::KC_PERIOD; *needs_shift = true; return true;
    case '?': *key_code = kavio::input::proto::KC_SLASH; *needs_shift = true; return true;
    default:
        return false;
    }
}

// 仅用于诊断不可识别的终端转义字节，不作为输入协议数据。
std::string BytesToHex(const std::string& bytes) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (size_t index = 0; index < bytes.size(); ++index) {
        const auto value = static_cast<unsigned char>(bytes[index]);
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.push_back(kHex[(value >> 4) & 0x0F]);
        out.push_back(kHex[value & 0x0F]);
    }
    return out;
}

// 普通方向键模拟键盘方向键；Ctrl+方向键用于演示固定步长鼠标移动。
bool ParseArrowKey(char final_byte, bool ctrl_modifier, TerminalAction* action) {
    if (!action) {
        return false;
    }

    *action = TerminalAction{};
    if (ctrl_modifier) {
        action->kind = TerminalActionKind::kMouseMove;
        switch (final_byte) {
        case 'A': action->mouse_dy = -kInteractiveMouseStepPx; return true;
        case 'B': action->mouse_dy = kInteractiveMouseStepPx; return true;
        case 'C': action->mouse_dx = kInteractiveMouseStepPx; return true;
        case 'D': action->mouse_dx = -kInteractiveMouseStepPx; return true;
        default:
            return false;
        }
    }

    action->kind = TerminalActionKind::kKeyPress;
    switch (final_byte) {
    case 'A': action->key_code = kavio::input::proto::KC_UP; return true;
    case 'B': action->key_code = kavio::input::proto::KC_DOWN; return true;
    case 'C': action->key_code = kavio::input::proto::KC_RIGHT; return true;
    case 'D': action->key_code = kavio::input::proto::KC_LEFT; return true;
    default:
        return false;
    }
}

// 从 ANSI CSI 转义序列解析修饰键，避免把多字节方向键误当作普通字符。
bool ParseCsiModifier(const std::string& params, int* modifier) {
    if (!modifier || params.empty()) {
        return false;
    }

    int current = 0;
    bool in_number = false;
    bool has_value = false;
    int last_value = 0;
    for (const char ch : params) {
        if (ch == ';') {
            if (in_number) {
                last_value = current;
                has_value = true;
                current = 0;
                in_number = false;
            }
            continue;
        }

        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            return false;
        }

        current = current * 10 + (ch - '0');
        in_number = true;
    }

    if (in_number) {
        last_value = current;
        has_value = true;
    }

    if (!has_value) {
        return false;
    }

    *modifier = last_value;
    return true;
}

bool ParseTerminalBytes(const std::string& bytes, TerminalAction* action) {
    if (!action || bytes.empty()) {
        return false;
    }

    *action = TerminalAction{};

    if (bytes[0] == '\x1b') {
        if (bytes.size() == 1) {
            action->kind = TerminalActionKind::kKeyPress;
            action->key_code = kavio::input::proto::KC_ESCAPE;
            return true;
        }

        if (bytes.size() == 3 && bytes[1] == 'O') {
            return ParseArrowKey(bytes[2], false, action);
        }

        if (bytes.size() >= 3 && bytes[1] == '[') {
            const char final_byte = bytes.back();
            if (final_byte != 'A' && final_byte != 'B' &&
                final_byte != 'C' && final_byte != 'D') {
                return false;
            }

            const std::string params = bytes.substr(2, bytes.size() - 3);
            int modifier = 1;
            if (!params.empty() && !ParseCsiModifier(params, &modifier)) {
                return false;
            }

            return ParseArrowKey(final_byte, modifier >= 5 && modifier <= 8, action);
        }

        return false;
    }

    if (bytes.size() != 1) {
        return false;
    }

    const auto ch = static_cast<unsigned char>(bytes[0]);
    switch (ch) {
    case '\r':
    case '\n':
        action->kind = TerminalActionKind::kKeyPress;
        action->key_code = kavio::input::proto::KC_ENTER;
        return true;
    case '\t':
        action->kind = TerminalActionKind::kKeyPress;
        action->key_code = kavio::input::proto::KC_TAB;
        return true;
    case 0x7F:
    case 0x08:
        action->kind = TerminalActionKind::kKeyPress;
        action->key_code = kavio::input::proto::KC_BSPACE;
        return true;
    default:
        return MapAsciiToKey(static_cast<char>(ch), &action->key_code, &action->shift)
            ? (action->kind = TerminalActionKind::kKeyPress, true)
            : false;
    }
}

bool ReadNextTerminalBytes(int fd,
                           std::string* bytes,
                           const std::atomic<bool>* local_stop) {
    if (!bytes) {
        return false;
    }

    bytes->clear();
    while (!g_stop.load(std::memory_order_acquire) &&
           !(local_stop && local_stop->load(std::memory_order_acquire))) {
        struct pollfd descriptor {
            fd,
            POLLIN,
            0,
        };

        const int poll_result = ::poll(&descriptor, 1, kInteractiveInputPollMs);
        if (poll_result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[INPUT] poll(stdin) failed: " << std::strerror(errno) << std::endl;
            return false;
        }

        if (poll_result == 0) {
            return true;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            std::cerr << "[INPUT] stdin is unavailable; stopping terminal input" << std::endl;
            return false;
        }

        if ((descriptor.revents & POLLIN) == 0) {
            continue;
        }

        char ch = 0;
        const ssize_t read_size = ::read(fd, &ch, 1);
        if (read_size < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            std::cerr << "[INPUT] read(stdin) failed: " << std::strerror(errno) << std::endl;
            return false;
        }

        if (read_size == 0) {
            std::cerr << "[INPUT] stdin reached EOF" << std::endl;
            return false;
        }

        bytes->push_back(ch);
        if (ch != '\x1b') {
            return true;
        }

        while (bytes->size() < kInteractiveEscapeMaxBytes &&
               !g_stop.load(std::memory_order_acquire) &&
               !(local_stop && local_stop->load(std::memory_order_acquire))) {
            struct pollfd tail_descriptor {
                fd,
                POLLIN,
                0,
            };

            const int tail_poll_result = ::poll(&tail_descriptor, 1, kInteractiveSequenceTailPollMs);
            if (tail_poll_result <= 0 || (tail_descriptor.revents & POLLIN) == 0) {
                return true;
            }

            const ssize_t tail_read_size = ::read(fd, &ch, 1);
            if (tail_read_size <= 0) {
                return true;
            }

            bytes->push_back(ch);
            if (ch >= '@' && ch <= '~') {
                return true;
            }
        }

        return true;
    }

    return false;
}

bool SendKeyboardInput(kavio::KavioClient& client,
                       kavio::input::proto::KeyboardEventPb_KeyOptionPb option,
                       kavio::input::proto::InputKeyCodePb key_code) {
    kavio::input::proto::InputEventPb event;
    event.set_timestamp_ns(NowNs());
    auto* keyboard = event.mutable_keyboard();
    keyboard->set_key_option(option);
    keyboard->set_key_code(key_code);

    const auto st = client.SendInputEvent(event);
    if (!st.ok()) {
        std::cerr << "SendInputEvent(keyboard) failed: " << st.message << std::endl;
        return false;
    }
    return true;
}

bool SendMouseInput(kavio::KavioClient& client,
                    kavio::input::proto::MouseEventPb_MouseOptionPb option,
                    int32_t x,
                    int32_t y,
                    int32_t wheel) {
    kavio::input::proto::InputEventPb event;
    event.set_timestamp_ns(NowNs());
    auto* mouse = event.mutable_mouse();
    mouse->set_mouse_option(option);
    mouse->set_x(x);
    mouse->set_y(y);
    mouse->set_wheel(wheel);

    const auto st = client.SendInputEvent(event);
    if (!st.ok()) {
        std::cerr << "SendInputEvent(mouse) failed: " << st.message << std::endl;
        return false;
    }
    return true;
}

bool SendKeyPress(kavio::KavioClient& client,
                  kavio::input::proto::InputKeyCodePb key_code,
                  bool shift) {
    if (shift && !SendKeyboardInput(client,
                                   kavio::input::proto::KeyboardEventPb_KeyOptionPb_KEY_OPTION_DOWN,
                                   kavio::input::proto::KC_LSHIFT)) {
        return false;
    }

    if (!SendKeyboardInput(client,
                           kavio::input::proto::KeyboardEventPb_KeyOptionPb_KEY_OPTION_DOWN,
                           key_code)) {
        if (shift) {
            SendKeyboardInput(client,
                              kavio::input::proto::KeyboardEventPb_KeyOptionPb_KEY_OPTION_UP,
                              kavio::input::proto::KC_LSHIFT);
        }
        return false;
    }

    if (!SendKeyboardInput(client,
                           kavio::input::proto::KeyboardEventPb_KeyOptionPb_KEY_OPTION_UP,
                           key_code)) {
        if (shift) {
            SendKeyboardInput(client,
                              kavio::input::proto::KeyboardEventPb_KeyOptionPb_KEY_OPTION_UP,
                              kavio::input::proto::KC_LSHIFT);
        }
        return false;
    }

    if (shift && !SendKeyboardInput(client,
                                   kavio::input::proto::KeyboardEventPb_KeyOptionPb_KEY_OPTION_UP,
                                   kavio::input::proto::KC_LSHIFT)) {
        return false;
    }

    return true;
}

// 一份 RX 图像驱动两个独立 TX 图层：每个图层各自构造 BGRA 和 WriteFrame。
void ProcessFrameAndOverlay(std::vector<DemoTxLayer>& tx_layers,
                            const kavio::ImageFrame& frame,
                            uint64_t tick,
                            const char* mode_tag) {
    for (auto& layer : tx_layers) {
        auto overlay = MakeOverlay(layer.tx_surface,
                                   frame,
                                   tick,
                                   *layer.badge_state,
                                   layer.fill_color);
        auto wst = layer.tx_surface.WriteFrame(overlay);
        if (!wst.ok()) {
            std::cerr << "WriteFrame failed for shm_id=" << layer.tx_surface.id()
                      << ": " << wst.message << std::endl;
            return;
        }
    }

    std::cout << mode_tag
              << " frame_id=" << frame.frame_id
              << " timestamp=" << frame.timestamp_ns
              << " size=" << frame.width << "x" << frame.height << std::endl;
}

// 开启 RX 原始帧、sample_interval=1 表示演示每帧可读；QueryStats 异步返回，不阻塞启动流程。
bool SendInitialRequests(kavio::KavioClient& client) {
    kavio::ipc::proto::RxParamsPb rx_params;
    rx_params.set_enabled(true);
    rx_params.set_frame_sample_interval(1);
    uint64_t request_id = 0;
    auto st = client.SetRxParams(rx_params, request_id);
    if (!st.ok()) {
        std::cerr << "SetRxParams submit failed: " << st.message << std::endl;
        return false;
    }
    st = client.QueryStats(request_id);
    if (!st.ok()) {
        std::cerr << "QueryStats submit failed: " << st.message << std::endl;
        return false;
    }
    return true;
}

bool ToRxAvailable(const kavio::RxInfo& rx_info) {
    return rx_info.frame_available;
}

bool RxInfoEqual(const kavio::RxInfo& lhs, const kavio::RxInfo& rhs) {
    return lhs.connected == rhs.connected &&
           lhs.frame_available == rhs.frame_available &&
           lhs.width == rhs.width &&
           lhs.height == rhs.height &&
           lhs.stride == rhs.stride &&
           lhs.pixel_format == rhs.pixel_format;
}

// 主状态机负责 session 生命周期；SDK 回调只投递事件/更新原子状态，不在回调里做重业务。
class KavioDemoApp : public kavio::IClientMessageHandler {
public:
    // 初始只启用 TX：先完成会话协商和两个图层的异步 Setup，
    // 再通过 SetRxParams() 打开 RX 原始帧，避免在 Setup 阶段持续复制 RX 图像。
    KavioDemoApp() {
        session_options_.set_client_name("kavio_demo");
        session_options_.set_client_pid(static_cast<int32_t>(::getpid()));
        session_options_.mutable_rx_params()->set_enabled(false);
        session_options_.mutable_rx_params()->set_frame_sample_interval(1);
        session_options_.mutable_tx_params()->set_enabled(true);
    }

    void InitFsm() {
        fsm_.RegisterState(FSM_State_Start,
                           FiniteStateMachine::StateHooks{
                               [this]() { OnStartEnter(); },
                               nullptr,
                               nullptr,
                           });

        fsm_.RegisterState(FSM_State_WaitClientReady,
                           FiniteStateMachine::StateHooks{
                               [this]() { OnWaitClientReadyEnter(); },
                               [this]() { OnWaitClientReadyLoop(); },
                               [this]() { OnWaitClientReadyExit(); },
                           });

        fsm_.RegisterState(FSM_State_Setup,
                           FiniteStateMachine::StateHooks{
                               [this]() { OnSetupEnter(); },
                               nullptr,
                               nullptr,
                           });

        fsm_.RegisterState(FSM_State_Running,
                           FiniteStateMachine::StateHooks{
                               [this]() { OnRunningEnter(); },
                               [this]() { OnRunningLoop(); },
                               [this]() { OnRunningExit(); },
                           });

        fsm_.RegisterState(FSM_State_Cleanup,
                           FiniteStateMachine::StateHooks{
                               [this]() { OnCleanupEnter(); },
                               nullptr,
                               nullptr,
                           });

        fsm_.RegisterState(FSM_State_Exit,
                           FiniteStateMachine::StateHooks{
                               [this]() { OnExitEnter(); },
                               nullptr,
                               nullptr,
                           });

        RegisterTransitions();
        fsm_.Start(FSM_State_Start);
    }

    void OnLoop()
    {
        fsm_.OnLoop();
    }

    bool IsFinished() const {
        return fsm_.IsInState(FSM_State_Exit);
    }

    void Stop() {
        fsm_.Stop();
    }

private:
    void RegisterTransitions() {
        fsm_.RegisterTransition(FSM_State_Start, FSM_Event_StartSubmitted, FSM_State_WaitClientReady);
        fsm_.RegisterTransition(FSM_State_Start, FSM_Event_SetupFailed, FSM_State_Cleanup);
        fsm_.RegisterTransition(FSM_State_Start, FSM_Event_StopRequested, FSM_State_Cleanup);

        fsm_.RegisterTransition(FSM_State_WaitClientReady, FSM_Event_ClientReady, FSM_State_Setup);
        fsm_.RegisterTransition(FSM_State_WaitClientReady, FSM_Event_StopRequested, FSM_State_Cleanup);

        fsm_.RegisterTransition(FSM_State_Setup, FSM_Event_SetupOk, FSM_State_Running);
        fsm_.RegisterTransition(FSM_State_Setup, FSM_Event_SetupFailed, FSM_State_Cleanup);
        fsm_.RegisterTransition(FSM_State_Setup, FSM_Event_ClientInvalid, FSM_State_WaitClientReady);
        fsm_.RegisterTransition(FSM_State_Setup, FSM_Event_StopRequested, FSM_State_Cleanup);

        fsm_.RegisterTransition(FSM_State_Running, FSM_Event_ClientInvalid, FSM_State_WaitClientReady);
        fsm_.RegisterTransition(FSM_State_Running, FSM_Event_StopRequested, FSM_State_Cleanup);

        fsm_.RegisterTransition(FSM_State_Cleanup, FSM_Event_CleanupDone, FSM_State_Exit);
    }

    // 断线会使旧 Session 的 SHM 失效；旧 Setup 不能继续接受迟到回复。
    void OnDisconnected() override {
        tx_setup_active_.store(false);
        rx_available_.store(false, std::memory_order_release);
        std::cout << "[CLIENT] state=" << ClientAvailabilityName(false) << std::endl;
        fsm_.PostEvent(FSM_Event_ClientInvalid);
    }

    // TX SHM 创建成功仅代表 Platform 已分配资源：此处需明确 OpenTxSurface。
    // 收齐两个图层才 PostEvent(SetupOk)；打开失败通过 SetupFailed 清理。
    // OnMessage 属于 SDK 单派发线程，不能据此推断 FSM 主线程已经切换状态。
    void OnMessage(const kavio::ipc::proto::IPCPackagePb& msg) override {
        namespace ip = kavio::ipc::proto;
        if (msg.header().msg_type() == ip::MSG_P2A_RSP_CREATE_TX_SHM &&
            msg.payload().has_p2a_rsp_create_tx_shm() && tx_setup_active_.load()) {
            const auto& info = msg.payload().p2a_rsp_create_tx_shm().tx_shm();
            kavio::TxSurface surface;
            auto st = client_.OpenTxSurface(info, surface);
            if (!st.ok()) {
                std::cerr << "OpenTxSurface failed: " << st.message << std::endl;
                uint64_t id = 0;
                client_.DestroyTxSurface(info.id(), id);
                tx_setup_active_.store(false);
                fsm_.PostEvent(FSM_Event_SetupFailed);
                return;
            }
            const size_t index = demo_layers_.size();
            if (index >= kDemoLabelCount) return;
            demo_layers_.emplace_back();
            auto& layer = demo_layers_.back();
            layer.tx_surface = std::move(surface);
            layer.badge_state = index == 0 ? &g_demo_badge_state : &g_demo_badge_state_aux;
            layer.fill_color = index == 0 ? BgraColor{48, 178, 255, 150}
                                         : BgraColor{255, 174, 54, 150};
            if (demo_layers_.size() == kDemoLabelCount) {
                tx_setup_active_.store(false);
                fsm_.PostEvent(FSM_Event_SetupOk);
            }
            return;
        }
        if (msg.header().msg_type() == ip::MSG_P2A_RSP_ERROR && msg.payload().has_p2a_rsp_error() &&
            msg.payload().p2a_rsp_error().failed_for() == ip::MSG_A2P_REQ_CREATE_TX_SHM &&
            tx_setup_active_.exchange(false)) {
            std::cerr << "CreateTxSurface failed: " << msg.payload().p2a_rsp_error().message() << std::endl;
            fsm_.PostEvent(FSM_Event_SetupFailed);
            return;
        }
            switch (msg.header().msg_type()) {
            case ip::MSG_P2A_RSP_CREATE_SESSION:
                std::cout << "[CLIENT] state=" << ClientAvailabilityName(true) << std::endl;
                fsm_.PostEvent(FSM_Event_ClientReady);
                break;
            case ip::MSG_P2A_RSP_RUNTIME_DATA:
            case ip::MSG_P2A_NTF_RUNTIME_DATA:
                PrintRuntimeData(client_.GetRuntimeData());
                // fall through, RX state may have changed.
            case ip::MSG_P2A_NTF_RX_SHM_READY:
            case ip::MSG_P2A_NTF_RX_SHM_INVALIDATED: {
                const auto next = client_.GetRxInfo();
                const bool available = ToRxAvailable(next);
                rx_available_.store(available, std::memory_order_release);
                UpdateInteractiveMouseBounds(next.width, next.height);
                break;
            }
            case ip::MSG_P2A_RSP_QUERY_STATS:
                if (msg.payload().has_p2a_rsp_query_stats())
                    PrintStatsReply(msg.payload().p2a_rsp_query_stats());
                break;
            case ip::MSG_P2A_RSP_ERROR:
                if (msg.payload().has_p2a_rsp_error())
                    std::cerr << "[IPC] request failed: " << msg.payload().p2a_rsp_error().message() << std::endl;
                break;
            default: break;
            }

    }

    void OnStartEnter() {
        std::cout << "============================================================\n"
                  << "Kavio KavioClient Demo\n"
                  << "  Network mode : FSM + CLIENT READY EVENTS\n"
                  << "  RX mode      : FSM EVENT + READY-AWARE POLL\n"
                  << "============================================================"
                  << std::endl;

        auto st = client_.Start(session_options_, *this);
        if (!st.ok()) {
            std::cerr << "Start failed: " << st.message << std::endl;
            fsm_.PostEvent(FSM_Event_SetupFailed);
            return;
        }

        std::cout << "Start submitted, socket=" << kavio::DefaultIpcSocketPath() << std::endl;
        fsm_.PostEvent(FSM_Event_StartSubmitted);
    }

    void OnWaitClientReadyEnter() {
        tx_setup_active_.store(false);
        ClearSessionResources(false);
        StopInteractiveInput();
        ResetInteractiveMouseState();
        rx_available_.store(false, std::memory_order_release);
        last_processed_frame_id_ = 0;
        std::cout << "[FSM] waiting for ClientReady" << std::endl;
    }

    void OnWaitClientReadyLoop() {
        if (client_.IsClientReady()) {
            fsm_.PostEvent(FSM_Event_ClientReady);
        }
    }

    void OnWaitClientReadyExit() {
    }

    // Setup 提交异步创建请求；OnMessage 完成 SHM 打开并通知 FSM。
    // 异步创建两个相同大小但各自独立的 TX Surface；成功事件由 OnMessage 汇总。
    void OnSetupEnter() {
        std::cout << "[FSM] setup: async RX, stats and two TX surfaces" << std::endl;
        if (!client_.IsClientReady()) {
            fsm_.PostEvent(FSM_Event_ClientInvalid);
            return;
        }
        if (!SendInitialRequests(client_)) {
            fsm_.PostEvent(FSM_Event_SetupFailed);
            return;
        }
        const auto initial_runtime = client_.GetRuntimeData();
        if (initial_runtime.has_rx_info())
            UpdateInteractiveMouseBounds(initial_runtime.rx_info().width(),
                                         initial_runtime.rx_info().height());
        const Size demo_badge_size = GetInitialTxSurfaceSize(initial_runtime);
        demo_layers_.clear();
        demo_layers_.reserve(kDemoLabelCount);
        tx_setup_active_.store(true);
        for (int index = 0; index < kDemoLabelCount; ++index) {
            uint64_t id = 0;
            auto st = client_.CreateTxSurface(
                static_cast<uint32_t>(demo_badge_size.width),
                static_cast<uint32_t>(demo_badge_size.height), id);
            if (!st.ok()) {
                std::cerr << "CreateTxSurface submit failed: " << st.message << std::endl;
                tx_setup_active_.store(false);
                fsm_.PostEvent(FSM_Event_SetupFailed);
                return;
            }
        }
    }

    // Running 重新初始化读取节奏与 frame_id，避免重连后继续沿用旧会话帧号。
    void OnRunningEnter() {
        tick_ = 0;
        last_processed_frame_id_ = 0;
        last_rx_poll_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(KAVIO_DEMO_RX_POLL_INTERVAL_MS);
        StartInteractiveInput();
        std::cout << "[FSM] running: pull RX frames only when rx_available=true, write two TX overlays" << std::endl;
    }

    // RX 没有逐帧回调；Running 按固定间隔主动读取“最新帧”，并用 frame_id 去重。
    // 按配置间隔拉取 RX 最新帧；相同 frame_id 不重复绘制，防止空转刷新。
    void OnRunningLoop() {
        if (!rx_available_.load(std::memory_order_acquire)) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const bool poll_due = now - last_rx_poll_ >= std::chrono::milliseconds(KAVIO_DEMO_RX_POLL_INTERVAL_MS);
        if (!poll_due) {
            return;
        }

        last_rx_poll_ = now;

        kavio::ImageFrame frame;
        auto st = client_.ReadLatestRxFrame(frame);
        if (!st.ok()) {
            if (st.code == kavio::StatusCode::Common_NotConnected) {
                fsm_.PostEvent(FSM_Event_ClientInvalid);
                return;
            }
            if (st.code != kavio::StatusCode::Media_NoFrame &&
                st.code != kavio::StatusCode::Media_VideoNotReady) {
                std::cerr << "ReadLatestRxFrame failed: " << st.message << std::endl;
            }
            return;
        }

        if (frame.empty() || frame.frame_id == 0 || frame.frame_id == last_processed_frame_id_) {
            return;
        }

        last_processed_frame_id_ = frame.frame_id;
        ProcessFrameAndOverlay(demo_layers_, frame, tick_++, "[FSM-PULL]");
    }

    void OnRunningExit() {
        StopInteractiveInput();
    }

    // session 失效后旧 TX Surface 已无效；异步销毁只在 Session 可用时提交。
    // 仅在 Session 仍有效时发送异步远端清理；断线后旧 SHM 自动不可再使用。
    void OnCleanupEnter() {
        tx_setup_active_.store(false);
        g_stop.store(true);
        StopInteractiveInput();

        if (client_.IsClientReady()) {
            ClearSessionResources(true);
        } else {
            ClearSessionResources(false);
            std::cout << "Skip TX cleanup because client is invalid; session surfaces are unusable." << std::endl;
        }

        client_.Stop();
        fsm_.PostEvent(FSM_Event_CleanupDone);
    }

    void OnExitEnter() {
        std::cout << "[FSM] exit" << std::endl;
    }

    void StartInteractiveInput() {
        StopInteractiveInput();
        input_stop_.store(false, std::memory_order_release);
        input_thread_ = std::thread([this]() {
            RunInteractiveInput();
        });
    }

    void StopInteractiveInput() {
        input_stop_.store(true, std::memory_order_release);
        if (input_thread_.joinable()) {
            input_thread_.join();
        }
    }

    void ResetInteractiveMouseState() {
        std::lock_guard<std::mutex> lock(input_mutex_);
        mouse_bound_width_ = kInteractiveMouseDefaultWidth;
        mouse_bound_height_ = kInteractiveMouseDefaultHeight;
        mouse_x_ = mouse_bound_width_ / 2;
        mouse_y_ = mouse_bound_height_ / 2;
        mouse_position_ready_ = false;
    }

    void UpdateInteractiveMouseBounds(uint32_t width, uint32_t height) {
        if (width == 0 || height == 0) {
            return;
        }

        std::lock_guard<std::mutex> lock(input_mutex_);
        mouse_bound_width_ = static_cast<int32_t>(width);
        mouse_bound_height_ = static_cast<int32_t>(height);

        const int32_t max_x = std::max<int32_t>(0, mouse_bound_width_ - 1);
        const int32_t max_y = std::max<int32_t>(0, mouse_bound_height_ - 1);
        if (!mouse_position_ready_) {
            mouse_x_ = max_x / 2;
            mouse_y_ = max_y / 2;
            mouse_position_ready_ = true;
            return;
        }

        mouse_x_ = std::max<int32_t>(0, std::min(mouse_x_, max_x));
        mouse_y_ = std::max<int32_t>(0, std::min(mouse_y_, max_y));
    }

    bool MoveInteractiveMouseBy(int32_t dx, int32_t dy) {
        int32_t target_x = 0;
        int32_t target_y = 0;
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            const int32_t max_x = std::max<int32_t>(0, mouse_bound_width_ - 1);
            const int32_t max_y = std::max<int32_t>(0, mouse_bound_height_ - 1);
            if (!mouse_position_ready_) {
                mouse_x_ = max_x / 2;
                mouse_y_ = max_y / 2;
                mouse_position_ready_ = true;
            }

            mouse_x_ = std::max<int32_t>(0, std::min(mouse_x_ + dx, max_x));
            mouse_y_ = std::max<int32_t>(0, std::min(mouse_y_ + dy, max_y));
            target_x = mouse_x_;
            target_y = mouse_y_;
        }

        if (!SendMouseInput(client_,
                            kavio::input::proto::MouseEventPb_MouseOptionPb_MOUSE_OPTION_MOVE,
                            target_x,
                            target_y,
                            0)) {
            return false;
        }

        std::cout << "[INPUT] mouse move -> (" << target_x << ", " << target_y << ")" << std::endl;
        return true;
    }

    // 将解析出的终端动作映射为设备输入事件，业务发送失败在此直接反馈。
    bool DispatchInteractiveAction(const TerminalAction& action) {
        switch (action.kind) {
        case TerminalActionKind::kKeyPress:
            if (!SendKeyPress(client_, action.key_code, action.shift)) {
                return false;
            }
            std::cout << "[INPUT] key -> "
                      << kavio::input::proto::InputKeyCodePb_Name(action.key_code)
                      << (action.shift ? " (shift)" : "")
                      << std::endl;
            return true;
        case TerminalActionKind::kMouseMove:
            return MoveInteractiveMouseBy(action.mouse_dx, action.mouse_dy);
        case TerminalActionKind::kNone:
        default:
            return true;
        }
    }

    // 终端交互独立于 TX 绘制：按键事件按协议即时提交而不等待远端应答。
    void RunInteractiveInput() {
        TerminalInputGuard terminal_guard;
        std::string enable_error;
        if (!terminal_guard.Enable(STDIN_FILENO, &enable_error)) {
            if (enable_error == "stdin is not a TTY") {
                std::cout << "[INPUT] stdin is not a TTY; interactive input disabled" << std::endl;
            } else {
                std::cerr << "[INPUT] failed to enable terminal input: " << enable_error << std::endl;
            }
            return;
        }

        std::cout << "[INPUT] interactive mode: ASCII -> keyboard, arrows -> keyboard arrows, "
                     "Ctrl+arrows -> mouse move, Ctrl+C -> exit"
                  << std::endl;

        while (!g_stop.load(std::memory_order_acquire) &&
               !input_stop_.load(std::memory_order_acquire)) {
            std::string bytes;
            if (!ReadNextTerminalBytes(STDIN_FILENO, &bytes, &input_stop_)) {
                return;
            }

            if (bytes.empty()) {
                continue;
            }

            TerminalAction action;
            if (!ParseTerminalBytes(bytes, &action)) {
                if (!bytes.empty() && bytes.front() == '\x1b') {
                    std::cout << "[INPUT] unsupported terminal sequence: "
                              << BytesToHex(bytes) << std::endl;
                }
                continue;
            }

            if (!DispatchInteractiveAction(action)) {
                return;
            }
        }
    }

    void ClearSessionResources(bool destroy_remote) {
        if (demo_layers_.empty()) {
            return;
        }

        if (destroy_remote && client_.IsClientReady()) {
            for (auto& layer : demo_layers_) {
                if (!layer.tx_surface.IsValid()) {
                    continue;
                }

                auto clear_st = layer.tx_surface.Clear();
                if (!clear_st.ok()) {
                    std::cerr << "Clear failed for shm_id=" << layer.tx_surface.id()
                              << ": " << clear_st.message << std::endl;
                }

                uint64_t destroy_request_id = 0;
                auto destroy_st = layer.tx_surface.Destroy(destroy_request_id);
                if (!destroy_st.ok()) {
                    std::cerr << "Destroy failed for shm_id=" << layer.tx_surface.id()
                              << ": " << destroy_st.message << std::endl;
                }
            }
        }

        demo_layers_.clear();
    }

private:
    std::atomic<bool> tx_setup_active_{false};
    kavio::ipc::proto::A2PReqCreateSessionPb session_options_;
    FiniteStateMachine fsm_;

    std::atomic<bool> rx_available_{false};
    std::chrono::steady_clock::time_point last_rx_poll_{};

    std::vector<DemoTxLayer> demo_layers_;
    uint64_t tick_ = 0;
    uint64_t last_processed_frame_id_ = 0;

    std::thread input_thread_;
    std::atomic<bool> input_stop_{false};
    std::mutex input_mutex_;
    int32_t mouse_bound_width_ = kInteractiveMouseDefaultWidth;
    int32_t mouse_bound_height_ = kInteractiveMouseDefaultHeight;
    int32_t mouse_x_ = kInteractiveMouseDefaultWidth / 2;
    int32_t mouse_y_ = kInteractiveMouseDefaultHeight / 2;
    bool mouse_position_ready_ = false;

    // Last member: destroy SDK and join its dispatch thread before FSM and App state.
    kavio::KavioClient client_;
};

} // namespace

int main() {
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);

    KavioDemoApp app;
    app.InitFsm();

    while (!g_stop.load(std::memory_order_acquire) && !app.IsFinished()) {
        app.OnLoop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!app.IsFinished()) {
        app.Stop();
    }

    while (!app.IsFinished()) {
        app.OnLoop();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    return 0;
}
