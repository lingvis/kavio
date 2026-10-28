/*
 * demo_metrics_panel - Windows Host Component
 *
 * 1. 接收Python Device Component 的指标查询并返回 Windows CPU、内存和固定磁盘；
 * 2. 提供一个固定大小的 App Panel 矩形；
 * 3. 用户拖动矩形时，把 X/Y 坐标发送给Device Component。
 *
 * 不包含图片解码、文件传输和驱动逻辑，便于验证最短 Python 通路。
 */
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>

#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>
#include <KLogger.h>
#include "DemoPythonMetricsProtocol.h"

namespace {

// 900x650 是宿主 App Panel 外框；560x220 是可拖动指标矩形，与 Python renderer 常量一致。
// 独立 WM_APP 消息用于从工作线程安全通知 UI，避免跨线程直接绘图。
constexpr int kPanelWidth = 900;
constexpr int kPanelHeight = 650;
constexpr int kBoxWidth = 560;
constexpr int kBoxHeight = 220;
constexpr UINT kDestroyPanelMessage = WM_APP + 1;
constexpr UINT kRefreshPanelMessage = WM_APP + 2;

uint64_t FileTimeValue(const FILETIME& value) {
    ULARGE_INTEGER number;
    number.LowPart = value.dwLowDateTime;
    number.HighPart = value.dwHighDateTime;
    return number.QuadPart;
}

// HostComponent API 回调与 Panel UI 线程并行运行；坐标/指标和 HWND 分别用独立 mutex 保护。
struct HostComponent {
    const KavioHostApi* host = nullptr;
    std::atomic<bool> running{false};
    std::atomic<bool> channel_ready{false};
    std::atomic<bool> device_ready{false};

    std::mutex state_mutex;
    int x = 40;
    int y = 40;
    demo_python_metrics::ResponseMessage latest_metrics{};
    bool has_metrics = false;

    bool has_cpu_sample = false;
    uint64_t previous_idle = 0;
    uint64_t previous_kernel = 0;
    uint64_t previous_user = 0;

    std::mutex panel_mutex;
    std::thread panel_thread;
    HWND panel_window = nullptr;
    HANDLE panel_ready = nullptr;
};

struct PanelState {
    HostComponent* component = nullptr;
    bool dragging = false;
    int drag_offset_x = 0;
    int drag_offset_y = 0;
};

// send 使用有界队列；短消息遇到 QueueFull 时做有限重试，避免拖动瞬间丢坐标。
KavioResult SendRawResult(HostComponent* component, const void* data, size_t size,
                          bool require_device_ready) {
    if (!component || !data || size == 0)
        return KavioResult_InvalidArgument;
    if (!component->host || !component->running.load() || !component->channel_ready.load() ||
        !component->host->send ||
        (require_device_ready && !component->device_ready.load()))
        return KavioResult_NotReady;

    KavioResult result = KavioResult_NotReady;
    for (int attempt = 0; attempt < 50; ++attempt) {
        result = component->host->send(component->host->context, data, size);
        if (result != KavioResult_QueueFull) return result;
        Sleep(2);
    }
    return result;
}

bool SendRaw(HostComponent* component, const void* data, size_t size, bool require_device_ready) {
    return SendRawResult(component, data, size, require_device_ready) ==
           KavioResult_Success;
}

template <typename T>
bool SendMessage(HostComponent* component, const T& message, bool require_device_ready = true) {
    return SendRaw(component, &message, sizeof(message), require_device_ready);
}

// CPU 使用率来自累计 FILETIME 的两次采样差，因此第一次 Query 返回 0 属于正常行为。
// GetSystemTimes 提供累计 CPU 时间，用前后差分计算百分比以避免启动时数值误差。
void ReadCpu(HostComponent& component, double& usage) {
    usage = 0.0;
    FILETIME idle = {}, kernel = {}, user = {};
    if (!GetSystemTimes(&idle, &kernel, &user)) return;

    const uint64_t next_idle = FileTimeValue(idle);
    const uint64_t next_kernel = FileTimeValue(kernel);
    const uint64_t next_user = FileTimeValue(user);
    if (component.has_cpu_sample && next_kernel >= component.previous_kernel &&
        next_user >= component.previous_user && next_idle >= component.previous_idle) {
        const uint64_t idle_delta = next_idle - component.previous_idle;
        const uint64_t total_delta = (next_kernel - component.previous_kernel) +
                                     (next_user - component.previous_user);
        if (total_delta > 0 && idle_delta <= total_delta) {
            usage = 100.0 * static_cast<double>(total_delta - idle_delta) /
                    static_cast<double>(total_delta);
        }
    }
    component.previous_idle = next_idle;
    component.previous_kernel = next_kernel;
    component.previous_user = next_user;
    component.has_cpu_sample = true;
}

void ReadMemory(demo_python_metrics::ResponseMessage& response) {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) return;
    response.memory_total_bytes = status.ullTotalPhys;
    response.memory_used_bytes = status.ullTotalPhys - status.ullAvailPhys;
}

// 固定 8 个磁盘槽位，Host 必须按 Python protocol.py 的静态响应长度填充。
void ReadDisks(demo_python_metrics::ResponseMessage& response) {
    wchar_t drives[512] = {};
    const DWORD length = GetLogicalDriveStringsW(511, drives);
    if (length == 0 || length >= 511) return;

    for (const wchar_t* drive = drives;
         *drive && response.disk_count < demo_python_metrics::kMaxDisks;
         drive += wcslen(drive) + 1) {
        if (GetDriveTypeW(drive) != DRIVE_FIXED) continue;
        ULARGE_INTEGER free_available = {}, total = {}, free_total = {};
        if (!GetDiskFreeSpaceExW(drive, &free_available, &total, &free_total)) continue;

        demo_python_metrics::DiskUsage& disk = response.disks[response.disk_count++];
        const wchar_t letter = drive[0];
        disk.name[0] = static_cast<char>(letter >= L'a' && letter <= L'z'
            ? letter - L'a' + 'A' : letter);
        disk.name[1] = '\0';
        disk.total_bytes = total.QuadPart;
        disk.used_bytes = total.QuadPart - free_total.QuadPart;
    }
}

// Position 只有 DeviceReady 后才发送；Channel ready 本身不足以说明 Python 业务循环已就绪。
// 仅发送矩形在面板内的当前位置，无须重新上传指标像素。
void SendPosition(HostComponent* component) {
    int x = 0;
    int y = 0;
    {
        std::lock_guard<std::mutex> lock(component->state_mutex);
        x = component->x;
        y = component->y;
    }
    demo_python_metrics::PositionMessage message;
    demo_python_metrics::Initialize(message, x, y);
    SendMessage(component, message, true);
}

void RequestPanelRefresh(HostComponent* component) {
    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (window) PostMessageW(window, kRefreshPanelMessage, 0, 0);
}

std::wstring BytesText(uint64_t value) {
    std::wostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    out << static_cast<double>(value) / (1024.0 * 1024.0 * 1024.0) << L" GB";
    return out.str();
}

void DrawPanelBackground(HDC dc, const RECT& rect) {
    HBRUSH background = CreateSolidBrush(RGB(235, 238, 242));
    FillRect(dc, &rect, background);
    DeleteObject(background);

    HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(215, 219, 224));
    HGDIOBJ old_pen = SelectObject(dc, grid_pen);
    for (int x = 0; x < rect.right; x += 40) {
        MoveToEx(dc, x, 0, nullptr);
        LineTo(dc, x, rect.bottom);
    }
    for (int y = 0; y < rect.bottom; y += 40) {
        MoveToEx(dc, 0, y, nullptr);
        LineTo(dc, rect.right, y);
    }
    SelectObject(dc, old_pen);
    DeleteObject(grid_pen);
}

// Panel 只是 Host 侧预览，不是设备 TX 的像素来源；设备端由 Python renderer 独立生成 BGRA。
void DrawMetricBox(HDC dc, HostComponent* component) {
    int x = 0;
    int y = 0;
    bool has_metrics = false;
    demo_python_metrics::ResponseMessage metrics{};
    {
        std::lock_guard<std::mutex> lock(component->state_mutex);
        x = component->x;
        y = component->y;
        has_metrics = component->has_metrics;
        metrics = component->latest_metrics;
    }

    RECT box = {x, y, x + kBoxWidth, y + kBoxHeight};
    HBRUSH brush = CreateSolidBrush(RGB(34, 36, 40));
    FillRect(dc, &box, brush);
    DeleteObject(brush);
    FrameRect(dc, &box, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HFONT font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HGDIOBJ old_font = SelectObject(dc, font);

    int text_y = y + 15;
    const int text_x = x + 16;
    std::wostringstream title;
    title << L"PYTHON METRICS DEMO   POS: " << x << L" / " << y;
    TextOutW(dc, text_x, text_y, title.str().c_str(), static_cast<int>(title.str().size()));
    text_y += 32;

    if (!has_metrics) {
        const wchar_t* waiting = L"WAITING FOR PYTHON APP QUERY";
        TextOutW(dc, text_x, text_y, waiting, static_cast<int>(wcslen(waiting)));
    } else {
        std::wostringstream cpu;
        cpu.setf(std::ios::fixed);
        cpu.precision(1);
        cpu << L"CPU: " << metrics.cpu_usage_percent << L"%";
        TextOutW(dc, text_x, text_y, cpu.str().c_str(), static_cast<int>(cpu.str().size()));
        text_y += 28;

        const std::wstring memory = L"MEM: " + BytesText(metrics.memory_used_bytes) +
                                    L" / " + BytesText(metrics.memory_total_bytes);
        TextOutW(dc, text_x, text_y, memory.c_str(), static_cast<int>(memory.size()));
        text_y += 28;

        for (uint32_t index = 0; index < metrics.disk_count && index < 3; ++index) {
            wchar_t disk_name[16] = {};
            MultiByteToWideChar(CP_ACP, 0, metrics.disks[index].name, -1,
                                disk_name, 16);
            const std::wstring line = std::wstring(disk_name) + L": " +
                BytesText(metrics.disks[index].used_bytes) + L" / " +
                BytesText(metrics.disks[index].total_bytes);
            TextOutW(dc, text_x, text_y, line.c_str(), static_cast<int>(line.size()));
            text_y += 28;
        }
    }

    SelectObject(dc, old_font);
    DeleteObject(font);
}

void UpdateTitle(HWND window, HostComponent* component) {
    int x = 0;
    int y = 0;
    {
        std::lock_guard<std::mutex> lock(component->state_mutex);
        x = component->x;
        y = component->y;
    }
    wchar_t title[256] = {};
    swprintf_s(title, L"Kavio Python Metrics Panel - drag rectangle - position %d,%d - %s",
        x, y, component->device_ready.load() ? L"device ready" : L"waiting for device");
    SetWindowTextW(window, title);
}

LRESULT CALLBACK PanelWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    PanelState* state = reinterpret_cast<PanelState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<PanelState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state || !state->component)
        return DefWindowProcW(window, message, wparam, lparam);

    HostComponent* component = state->component;
    switch (message) {
    case WM_LBUTTONDOWN: {
        int x = 0;
        int y = 0;
        {
            std::lock_guard<std::mutex> lock(component->state_mutex);
            x = component->x;
            y = component->y;
        }
        const int mouse_x = GET_X_LPARAM(lparam);
        const int mouse_y = GET_Y_LPARAM(lparam);
        if (mouse_x >= x && mouse_y >= y &&
            mouse_x < x + kBoxWidth && mouse_y < y + kBoxHeight) {
            state->dragging = true;
            state->drag_offset_x = mouse_x - x;
            state->drag_offset_y = mouse_y - y;
            SetCapture(window);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (state->dragging && (wparam & MK_LBUTTON)) {
            RECT client = {};
            GetClientRect(window, &client);
            const int max_x = std::max(0, static_cast<int>(client.right) - kBoxWidth);
            const int max_y = std::max(0, static_cast<int>(client.bottom) - kBoxHeight);
            const int x = std::max(0, std::min(max_x,
                GET_X_LPARAM(lparam) - state->drag_offset_x));
            const int y = std::max(0, std::min(max_y,
                GET_Y_LPARAM(lparam) - state->drag_offset_y));
            {
                std::lock_guard<std::mutex> lock(component->state_mutex);
                component->x = x;
                component->y = y;
            }
            SendPosition(component);
            UpdateTitle(window, component);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state->dragging) {
            state->dragging = false;
            ReleaseCapture();
            SendPosition(component);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT client = {};
        GetClientRect(window, &client);
        DrawPanelBackground(dc, client);
        DrawMetricBox(dc, component);
        EndPaint(window, &paint);
        return 0;
    }
    case kRefreshPanelMessage:
        UpdateTitle(window, component);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case kDestroyPanelMessage:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

// App Panel 独立创建窗口和消息循环，避免阻塞 KavioAppHost 的 Host Component 回调线程。
// UI 线程处理拖动、绘制和状态刷新；Channel 业务消息通过安全路径更新视图状态。
void PanelThreadMain(HostComponent* component) {
    PanelState state;
    state.component = component;

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = PanelWindowProc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = L"KavioDemoPythonMetricsPanel";
    RegisterClassExW(&window_class);

    HWND window = CreateWindowExW(
        0, window_class.lpszClassName,
        L"Kavio Python Metrics Panel - drag rectangle",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, kPanelHeight,
        nullptr, nullptr, window_class.hInstance, &state);
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        component->panel_window = window;
    }
    SetEvent(component->panel_ready);

    MSG message = {};
    while (window && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        component->panel_window = nullptr;
    }
    UnregisterClassW(window_class.lpszClassName, window_class.hInstance);
}

// 每个 Python Query 都现场采集一份 Windows 指标，同时刷新 Panel 预览并回包。
KavioResult HandleQuery(HostComponent* component, const void* data, size_t size) {
    if (size != sizeof(demo_python_metrics::QueryMessage))
        return KavioResult_InvalidArgument;
    demo_python_metrics::QueryMessage query = {};
    std::memcpy(&query, data, sizeof(query));
    if (!demo_python_metrics::IsValid(
            query.header, demo_python_metrics::MessageType::Query, size))
        return KavioResult_InvalidArgument;

    demo_python_metrics::ResponseMessage response;
    demo_python_metrics::Initialize(response, query.sequence);
    ReadCpu(*component, response.cpu_usage_percent);
    ReadMemory(response);
    ReadDisks(response);
    {
        std::lock_guard<std::mutex> lock(component->state_mutex);
        component->latest_metrics = response;
        component->has_metrics = true;
    }
    RequestPanelRefresh(component);
    return SendRawResult(component, &response, sizeof(response), false);
}

}  // namespace

extern "C" {


KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostCreate(
    const KavioHostCreateParams* params, KavioHostHandle* output) {
    if (!params || !output || !params->host ||
        params->struct_size < KAVIO_HOST_CREATE_PARAMS_BASE_SIZE ||
        params->host->struct_size < KAVIO_HOST_API_BASE_SIZE ||
        !params->host->send)
        return KavioResult_InvalidArgument;

    HostComponent* component = new (std::nothrow) HostComponent();
    if (!component) return KavioResult_InternalError;
    component->host = params->host;
    component->panel_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!component->panel_ready) {
        delete component;
        return KavioResult_InternalError;
    }
    *output = component;
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStart(
    KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(true);
    KLOGI("demo_metrics_panel started");
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostPrepareStop(
    KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStop(
    KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return KavioResult_InvalidArgument;
    component->running.store(false);

    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (window) PostMessageW(window, kDestroyPanelMessage, 0, 0);
    if (component->panel_thread.joinable()) component->panel_thread.join();
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostDestroy(
    KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return;
    KavioHostStop(component);
    if (component->panel_ready) CloseHandle(component->panel_ready);
    delete component;
}

// 一个业务 Channel 数据报只包含一种消息；先验 header 校验通过后再按 type 分派。
KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOnData(
    KavioHostHandle handle, const void* data, size_t size) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component || !data || size < sizeof(demo_python_metrics::MessageHeader))
        return KavioResult_InvalidArgument;
    if (!component->running.load()) return KavioResult_NotReady;

    demo_python_metrics::MessageHeader header = {};
    std::memcpy(&header, data, sizeof(header));
    if (header.magic != demo_python_metrics::kMagic ||
        header.version != demo_python_metrics::kVersion ||
        header.size != size)
        return KavioResult_InvalidArgument;

    const demo_python_metrics::MessageType type =
        static_cast<demo_python_metrics::MessageType>(header.type);
    if (type == demo_python_metrics::MessageType::Query)
        return HandleQuery(component, data, size);

    if (type == demo_python_metrics::MessageType::DeviceReady &&
        size == sizeof(demo_python_metrics::DeviceReadyMessage)) {
        component->device_ready.store(true);
        SendPosition(component);
        RequestPanelRefresh(component);
        return KavioResult_Success;
    }
    return KavioResult_InvalidArgument;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostOnDeviceState(
    KavioHostHandle handle, uint32_t ready) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component) return;
    component->channel_ready.store(ready != 0);
    if (!ready) component->device_ready.store(false);
    RequestPanelRefresh(component);
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOpenPanel(
    KavioHostHandle handle) {
    HostComponent* component = static_cast<HostComponent*>(handle);
    if (!component || !component->running.load()) return KavioResult_NotReady;

    HWND window = nullptr;
    {
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
        if (!window && !component->panel_thread.joinable()) {
            ResetEvent(component->panel_ready);
            component->panel_thread = std::thread(PanelThreadMain, component);
        }
    }
    if (!window) {
        WaitForSingleObject(component->panel_ready, 3000);
        std::lock_guard<std::mutex> lock(component->panel_mutex);
        window = component->panel_window;
    }
    if (!window) return KavioResult_InternalError;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    return KavioResult_Success;
}

}  // extern "C"
