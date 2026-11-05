/**
 * @file HostComponent.cpp
 * @brief Windows Host component for the Kavio SDK demonstration.
 *
 * Shows Host ABI lifecycle callbacks, a Win32 control panel, App Channel
 * protocol handling, ping timing, and transfer throughput measurement.
 * Device callbacks may arrive off the UI thread; GUI refreshes use messages
 * rather than manipulating HWND controls directly from transport threads.
 */

#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>

#include "../common/DemoProtocol.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Win32 message and control IDs. Keep each ID distinct within the panel.
constexpr UINT kPanelRefreshMessage = WM_APP + 1;
constexpr int kIdHome = 1001;
constexpr int kIdRender = 1002;
constexpr int kIdTransfer = 1003;
constexpr int kIdHdmiRx = 1004;
constexpr int kIdHdmiTx = 1005;
constexpr int kIdRuntime = 1006;
constexpr int kIdKeyboard = 1007;
constexpr int kIdH264 = 1008;
constexpr int kIdSchema = 1009;
constexpr int kIdPing = 1010;
constexpr int kIdStartTransfer = 1011;
constexpr int kIdMouse = 1012;
constexpr int kIdPayloadKb = 1020;
constexpr int kIdPacketCount = 1021;
constexpr int kIdStateText = 1030;
constexpr int kIdLogText = 1031;

/** Return monotonic nanoseconds for same-host RTT measurements. */
uint64_t SteadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** Convert a Host API status code into a diagnostic string. */
std::string ResultName(KavioResult result)
{
    switch (result) {
        case KavioResult_Success: return "Success";
        case KavioResult_InvalidArgument: return "InvalidArgument";
        case KavioResult_NotReady: return "NotReady";
        case KavioResult_TransportError: return "TransportError";
        case KavioResult_InternalError: return "InternalError";
        case KavioResult_NotSupported: return "NotSupported";
        case KavioResult_QueueFull: return "QueueFull";
        default: return "Unknown";
    }
}

/**
 * @brief Per-instance Host state and resources.
 *
 * Atomic flags coordinate lifecycle/threads. All throughput statistics and
 * pending request metadata are guarded by mutex. HWND values are borrowed
 * from the panel thread and must not outlive its message loop.
 */
struct DemoHost {
    explicit DemoHost(const KavioHostApi* host_api) : api(host_api) {}

    const KavioHostApi* api = nullptr;
    // Lifecycle and channel readiness (read from Host transport callbacks).
    std::atomic<bool> started{false};
    std::atomic<bool> device_online{false};
    std::atomic<bool> transfer_running{false};
    std::atomic<uint32_t> sequence{0};

    // State and benchmark metrics protected against concurrent callback access.
    std::mutex mutex;
    std::string state_text = "No Device state received yet.";
    std::string log_text;
    double last_rtt_ms = 0.0;
    uint64_t ack_packets = 0;
    uint64_t ack_bytes = 0;
    uint64_t ack_errors = 0;
    double current_rate_mib_s = 0.0;
    double average_rate_mib_s = 0.0;
    uint64_t test_start_ns = 0;
    uint64_t test_start_ack_bytes = 0;
    uint64_t rate_window_start_ns = 0;
    uint64_t rate_window_start_bytes = 0;
    uint64_t last_ack_ns = 0;
    uint32_t test_payload_kb = 64;
    uint64_t submitted_packets = 0;
    uint64_t submitted_bytes = 0;
    uint64_t queue_full_retries = 0;
    double submit_rate_mib_s = 0.0;
    // Outstanding sequence -> (payload bytes, expected checksum).
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> pending_transfer;

    // Windows panel resources belong to the GUI thread; worker threads use
    // PostMessage and must be joined during shutdown.
    std::thread panel_thread;
    std::thread transfer_thread;
    std::atomic<DWORD> panel_thread_id{0};
    std::atomic<HWND> panel_hwnd{nullptr};
    HWND state_hwnd = nullptr;
    HWND log_hwnd = nullptr;
    HFONT panel_font = nullptr;
    HFONT section_font = nullptr;
    HBRUSH panel_brush = nullptr;
    HBRUSH edit_brush = nullptr;

    /** Report whether the Host API and device channel are ready. */
    bool CanSend() const
    {
        if (!started.load() || !api || !api->send) return false;
        if (KAVIO_HOST_API_HAS_FIELD(api, is_device_online) && api->is_device_online)
            return api->is_device_online(api->context) != 0;
        return device_online.load();
    }

    /** Return the current App Channel identifier, when supported. */
    uint32_t ChannelId() const
    {
        if (!api || !KAVIO_HOST_API_HAS_FIELD(api, get_channel_id) || !api->get_channel_id) return 0;
        return api->get_channel_id(api->context);
    }

    /** Append a bounded log entry and request panel repaint. */
    void AppendLog(const std::string& line)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            log_text += line;
            log_text += "\r\n";
            if (log_text.size() > 24000) log_text.erase(0, log_text.size() - 18000);
        }
        PostRefresh();
    }

    /** Update the latest device state string and request panel repaint. */
    void SetState(const std::string& text)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            state_text = text;
        }
        PostRefresh();
    }

    /** Post an asynchronous refresh notification to the Win32 panel. */
    void PostRefresh()
    {
        HWND hwnd = panel_hwnd.load();
        if (hwnd) PostMessageW(hwnd, kPanelRefreshMessage, 0, 0);
    }

    /** Send one complete binary datagram through the Host API. */
    KavioResult SendBytes(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty() || !CanSend()) return KavioResult_NotReady;
        return api->send(api->context, bytes.data(), bytes.size());
    }

    /** Tell the device to display a protocol-defined page. */
    void SendNavigate(kavio_demo::Page page)
    {
        const uint32_t seq = ++sequence;
        const KavioResult result = SendBytes(kavio_demo::MakePacket(
            kavio_demo::MessageType::Navigate, seq, kavio_demo::EncodePage(page)));
        AppendLog("Navigate request: result=" + ResultName(result));
    }

    /** Send a timestamp used to measure round-trip latency. */
    void SendPing()
    {
        const uint32_t seq = ++sequence;
        const auto payload = kavio_demo::EncodeU64(SteadyNowNs());
        const KavioResult result = SendBytes(kavio_demo::MakePacket(
            kavio_demo::MessageType::Ping, seq, payload));
        AppendLog("Ping: result=" + ResultName(result));
    }

    /** Request current device state from the connected Demo. */
    void QueryState()
    {
        const uint32_t seq = ++sequence;
        (void)SendBytes(kavio_demo::MakePacket(kavio_demo::MessageType::QueryState, seq, nullptr, 0));
    }

    /** Start a bounded background App Channel benchmark.
      * @param payload_kb Size of each packet payload, in KiB (clamped to [1,1023]).
      * @param packet_count Number of packets to transmit (clamped to [1,10000]).
     */
    void StartTransferTest(uint32_t payload_kb, uint32_t packet_count)
    {
        if (transfer_running.exchange(true)) {
            AppendLog("Transfer test is already running.");
            return;
        }
        if (transfer_thread.joinable()) transfer_thread.join();
        payload_kb = (payload_kb < 1) ? 1 : ((payload_kb > 1023) ? 1023 : payload_kb);
        packet_count = (packet_count < 1) ? 1 : ((packet_count > 10000) ? 10000 : packet_count);
        {
            std::lock_guard<std::mutex> lock(mutex);
            test_payload_kb = payload_kb;
            test_start_ns = SteadyNowNs();
            test_start_ack_bytes = ack_bytes;
            rate_window_start_ns = test_start_ns;
            rate_window_start_bytes = ack_bytes;
            last_ack_ns = 0;
            current_rate_mib_s = 0.0;
            average_rate_mib_s = 0.0;
            submitted_packets = 0;
            submitted_bytes = 0;
            queue_full_retries = 0;
            submit_rate_mib_s = 0.0;
        }
        transfer_thread = std::thread([this, payload_kb, packet_count]() {
            const size_t payload_size = static_cast<size_t>(payload_kb) * 1024u;

            // Benchmark the channel rather than payload generation. Build one deterministic
            // payload and its checksum once, then reuse the same packet buffer while only
            // changing the sequence field in the protocol header.
            std::vector<uint8_t> payload(payload_size);
            for (size_t n = 0; n < payload.size(); ++n)
                payload[n] = static_cast<uint8_t>((n * 31u + 17u) & 0xFFu);
            const uint32_t payload_checksum = kavio_demo::Checksum(payload.data(), payload.size());
            std::vector<uint8_t> packet = kavio_demo::MakePacketWithChecksum(
                kavio_demo::MessageType::TransferData, 0, payload.data(), payload.size(), payload_checksum);

            uint32_t submitted = 0;
            uint32_t failed = 0;
            uint64_t queue_full_count = 0;
            std::vector<uint32_t> test_sequences;
            test_sequences.reserve(packet_count);
            const uint64_t submit_begin_ns = SteadyNowNs();
            uint64_t submit_end_ns = submit_begin_ns;
            {
                // Start timing after the reusable benchmark payload has been prepared.
                std::lock_guard<std::mutex> lock(mutex);
                test_start_ns = submit_begin_ns;
                test_start_ack_bytes = ack_bytes;
                rate_window_start_ns = submit_begin_ns;
                rate_window_start_bytes = ack_bytes;
            }

            for (uint32_t i = 0; i < packet_count && transfer_running.load() && started.load(); ++i) {
                if (!CanSend()) { ++failed; break; }
                const uint32_t seq = ++sequence;
                packet[8] = static_cast<uint8_t>(seq & 0xFFu);
                packet[9] = static_cast<uint8_t>((seq >> 8) & 0xFFu);
                packet[10] = static_cast<uint8_t>((seq >> 16) & 0xFFu);
                packet[11] = static_cast<uint8_t>((seq >> 24) & 0xFFu);
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    pending_transfer[seq] = {static_cast<uint32_t>(payload.size()), payload_checksum};
                }
                KavioResult result = KavioResult_NotReady;
                // QueueFull is transient. Limit retries and sleep 1 ms to avoid
                // spinning when transport buffers cannot accept more data.
                for (uint32_t retry = 0; retry < 1000 && transfer_running.load() && started.load(); ++retry) {
                    result = SendBytes(packet);
                    if (result != KavioResult_QueueFull) break;
                    ++queue_full_count;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                if (result == KavioResult_Success) {
                    ++submitted;
                    submit_end_ns = SteadyNowNs();
                    test_sequences.push_back(seq);
                } else {
                    ++failed;
                    std::lock_guard<std::mutex> lock(mutex);
                    pending_transfer.erase(seq);
                }
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                submitted_packets = submitted;
                submitted_bytes = static_cast<uint64_t>(submitted) * payload_size;
                queue_full_retries = queue_full_count;
                if (submit_end_ns > submit_begin_ns && submitted_bytes != 0) {
                    const double seconds = static_cast<double>(submit_end_ns - submit_begin_ns) / 1000000000.0;
                    submit_rate_mib_s = static_cast<double>(submitted_bytes) /
                                        (1024.0 * 1024.0) / seconds;
                }
            }
            PostRefresh();

            const uint64_t wait_deadline_ns = SteadyNowNs() + 5000000000ull;
            while (transfer_running.load() && started.load() && SteadyNowNs() < wait_deadline_ns) {
                bool any_pending = false;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    for (const uint32_t seq : test_sequences) {
                        if (pending_transfer.find(seq) != pending_transfer.end()) {
                            any_pending = true;
                            break;
                        }
                    }
                }
                if (!any_pending) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            char text[256]{};
            std::snprintf(text, sizeof(text),
                          "Transfer test submitted=%u failed=%u payload=%u KB queue_full_retries=%llu",
                          submitted, failed, payload_kb,
                          static_cast<unsigned long long>(queue_full_count));
            AppendLog(text);
            transfer_running.store(false);
            PostRefresh();
        });
    }

    /** Dispatch an already-validated demo-protocol response. */
    void HandlePacket(const kavio_demo::Packet& packet)
    {
        switch (packet.type) {
            case kavio_demo::MessageType::Hello:
                AppendLog("Device hello: " + kavio_demo::PayloadAsString(packet.payload));
                (void)SendBytes(kavio_demo::MakeTextPacket(kavio_demo::MessageType::HelloAck,
                                                           packet.sequence, "KavioDemo Host"));
                break;
            case kavio_demo::MessageType::HelloAck:
                AppendLog("Device hello ack: " + kavio_demo::PayloadAsString(packet.payload));
                break;
            case kavio_demo::MessageType::Ping:
                (void)SendBytes(kavio_demo::MakePacket(kavio_demo::MessageType::Pong,
                                                       packet.sequence, packet.payload));
                break;
            case kavio_demo::MessageType::Pong: {
                uint64_t sent_ns = 0;
                if (kavio_demo::DecodeU64(packet.payload, sent_ns)) {
                    const double rtt_ms = static_cast<double>(SteadyNowNs() - sent_ns) / 1000000.0;
                    {
                        std::lock_guard<std::mutex> lock(mutex);
                        last_rtt_ms = rtt_ms;
                    }
                    char text[96]{};
                    std::snprintf(text, sizeof(text), "Ping RTT: %.3f ms", rtt_ms);
                    AppendLog(text);
                }
                break;
            }
            case kavio_demo::MessageType::TransferAck: {
                uint32_t bytes = 0;
                uint32_t checksum = 0;
                if (!kavio_demo::DecodeTransferAck(packet.payload, bytes, checksum)) {
                    std::lock_guard<std::mutex> lock(mutex);
                    ++ack_errors;
                    break;
                }
                std::lock_guard<std::mutex> lock(mutex);
                const auto it = pending_transfer.find(packet.sequence);
                if (it == pending_transfer.end() || it->second.first != bytes || it->second.second != checksum) {
                    ++ack_errors;
                } else {
                    ++ack_packets;
                    ack_bytes += bytes;
                    pending_transfer.erase(it);
                    const uint64_t now_ns = SteadyNowNs();
                    last_ack_ns = now_ns;
                    if (test_start_ns != 0 && now_ns > test_start_ns) {
                        const double avg_seconds = static_cast<double>(now_ns - test_start_ns) / 1000000000.0;
                        average_rate_mib_s = static_cast<double>(ack_bytes - test_start_ack_bytes) /
                                             (1024.0 * 1024.0) / avg_seconds;
                        if (current_rate_mib_s == 0.0) current_rate_mib_s = average_rate_mib_s;
                    }
                    if (rate_window_start_ns == 0) {
                        rate_window_start_ns = now_ns;
                        rate_window_start_bytes = ack_bytes;
                    } else if (now_ns - rate_window_start_ns >= 200000000ull) {
                        const double seconds = static_cast<double>(now_ns - rate_window_start_ns) / 1000000000.0;
                        current_rate_mib_s = static_cast<double>(ack_bytes - rate_window_start_bytes) /
                                             (1024.0 * 1024.0) / seconds;
                        rate_window_start_ns = now_ns;
                        rate_window_start_bytes = ack_bytes;
                    }
                }
                PostRefresh();
                break;
            }
            case kavio_demo::MessageType::StateReport:
                SetState(kavio_demo::PayloadAsString(packet.payload));
                break;
            default:
                break;
        }
    }

    /** Stop benchmark and GUI threads, joining both before cleanup. */
    void StopThreads()
    {
        transfer_running.store(false);
        if (transfer_thread.joinable()) transfer_thread.join();
        const HWND hwnd = panel_hwnd.load();
        if (hwnd) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        } else {
            const DWORD thread_id = panel_thread_id.load();
            if (thread_id) PostThreadMessageW(thread_id, WM_QUIT, 0, 0);
        }
        if (panel_thread.joinable()) panel_thread.join();
        panel_thread_id.store(0);
        panel_hwnd.store(nullptr);
        state_hwnd = nullptr;
        log_hwnd = nullptr;
    }
};

/** Replace read-only edit text while preserving scroll behavior. */
void UpdateReadOnlyEdit(HWND edit, const std::string& text, bool follow_when_at_end)
{
    if (!edit) return;
    const int old_len = GetWindowTextLengthA(edit);
    std::string old_text(static_cast<size_t>(std::max(0, old_len)) + 1u, '\0');
    if (old_len > 0) GetWindowTextA(edit, &old_text[0], old_len + 1);
    old_text.resize(static_cast<size_t>(std::max(0, old_len)));
    if (old_text == text) return;

    DWORD sel_start = 0;
    DWORD sel_end = 0;
    SendMessageA(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
    const int first_visible = static_cast<int>(SendMessageA(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
    bool at_bottom = old_len == 0;
    SCROLLINFO scroll_info{};
    scroll_info.cbSize = sizeof(scroll_info);
    scroll_info.fMask = SIF_ALL;
    if (GetScrollInfo(edit, SB_VERT, &scroll_info)) {
        const int bottom_pos = std::max(scroll_info.nMin,
            scroll_info.nMax - static_cast<int>(scroll_info.nPage) + 1);
        at_bottom = scroll_info.nPos >= bottom_pos;
    }
    const bool follow_end = follow_when_at_end && at_bottom &&
                            sel_start == static_cast<DWORD>(old_len) &&
                            sel_end == static_cast<DWORD>(old_len);

    SetWindowTextA(edit, text.c_str());
    const int new_len = GetWindowTextLengthA(edit);
    if (follow_end) {
        SendMessageA(edit, EM_SETSEL, static_cast<WPARAM>(new_len), static_cast<LPARAM>(new_len));
        SendMessageA(edit, EM_SCROLLCARET, 0, 0);
    } else {
        const DWORD restored_start = std::min<DWORD>(sel_start, static_cast<DWORD>(new_len));
        const DWORD restored_end = std::min<DWORD>(sel_end, static_cast<DWORD>(new_len));
        SendMessageA(edit, EM_SETSEL, restored_start, restored_end);
        const int now_first = static_cast<int>(SendMessageA(edit, EM_GETFIRSTVISIBLELINE, 0, 0));
        SendMessageA(edit, EM_LINESCROLL, 0, first_visible - now_first);
    }
}

/** Copy snapshot statistics into the Win32 control panel. */
void RefreshPanel(DemoHost* self)
{
    if (!self || !self->panel_hwnd.load()) return;
    std::string state;
    std::string log;
    double rtt = 0.0;
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t errors = 0;
    double current_rate = 0.0;
    double average_rate = 0.0;
    uint32_t payload_kb = 0;
    size_t pending = 0;
    uint64_t submitted_packets = 0;
    uint64_t submitted_bytes = 0;
    uint64_t queue_full_retries = 0;
    double submit_rate = 0.0;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        state = self->state_text;
        log = self->log_text;
        rtt = self->last_rtt_ms;
        packets = self->ack_packets;
        bytes = self->ack_bytes;
        errors = self->ack_errors;
        current_rate = self->current_rate_mib_s;
        average_rate = self->average_rate_mib_s;
        payload_kb = self->test_payload_kb;
        pending = self->pending_transfer.size();
        submitted_packets = self->submitted_packets;
        submitted_bytes = self->submitted_bytes;
        queue_full_retries = self->queue_full_retries;
        submit_rate = self->submit_rate_mib_s;
    }
    char summary[1792]{};
    std::snprintf(summary, sizeof(summary),
                  "Device online: %s\r\nChannel ID: %u\r\nLast RTT: %.3f ms\r\n"
                  "Host submit rate: %.2f MiB/s (%llu packets / %llu bytes)\r\n"
                  "Device ACK rate: %.2f MiB/s last window | %.2f MiB/s average\r\n"
                  "Payload: %u KB | Pending ACK: %zu | QueueFull retries: %llu\r\n"
                  "Transfer ACK packets: %llu\r\nTransfer ACK bytes: %llu\r\nTransfer errors: %llu\r\n\r\n%s",
                  self->CanSend() ? "Yes" : "No", self->ChannelId(), rtt,
                  submit_rate,
                  static_cast<unsigned long long>(submitted_packets),
                  static_cast<unsigned long long>(submitted_bytes),
                  current_rate, average_rate, payload_kb, pending,
                  static_cast<unsigned long long>(queue_full_retries),
                  static_cast<unsigned long long>(packets),
                  static_cast<unsigned long long>(bytes),
                  static_cast<unsigned long long>(errors), state.c_str());
    UpdateReadOnlyEdit(self->state_hwnd, summary, false);
    UpdateReadOnlyEdit(self->log_hwnd, log, true);
}

/** Read a bounded unsigned integer from a Win32 edit control. */
uint32_t ReadUIntEdit(HWND parent, int control_id, uint32_t fallback)
{
    char text[32]{};
    HWND edit = GetDlgItem(parent, control_id);
    if (!edit || GetWindowTextA(edit, text, sizeof(text)) <= 0) return fallback;
    unsigned long value = std::strtoul(text, nullptr, 10);
    return value == 0 ? fallback : static_cast<uint32_t>(value);
}

HWND CreatePanelControl(DemoHost* self,
                        DWORD ex_style,
                        const char* class_name,
                        const char* text,
                        DWORD style,
                        int x, int y, int width, int height,
                        HWND parent, int id,
                        bool section = false)
{
    HWND control = CreateWindowExA(ex_style, class_name, text, style,
                                   x, y, width, height, parent,
                                   id ? reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)) : nullptr,
                                   nullptr, nullptr);
    if (control) {
        HFONT font = section ? self->section_font : self->panel_font;
        if (font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return control;
}

/** Handle Win32 panel messages, commands, and shutdown. */
LRESULT CALLBACK PanelWndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    DemoHost* self = reinterpret_cast<DemoHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<DemoHost*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (message) {
        case WM_CREATE: {
            if (!self) return -1;
            self->panel_brush = CreateSolidBrush(RGB(255, 255, 255));
            self->edit_brush = CreateSolidBrush(RGB(255, 255, 255));
            self->panel_font = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                           CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            self->section_font = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            CreatePanelControl(self, 0, "STATIC", "Device navigation", WS_CHILD | WS_VISIBLE,
                               20, 16, 240, 26, hwnd, 0, true);
            const int nav_w = 132;
            const int nav_h = 36;
            const int nav_gap = 10;
            const int nav_x[5] = {20, 20 + nav_w + nav_gap, 20 + (nav_w + nav_gap) * 2,
                                  20 + (nav_w + nav_gap) * 3, 20 + (nav_w + nav_gap) * 4};
            CreatePanelControl(self, 0, "BUTTON", "Home", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[0], 48, nav_w, nav_h, hwnd, kIdHome);
            CreatePanelControl(self, 0, "BUTTON", "Render", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[1], 48, nav_w, nav_h, hwnd, kIdRender);
            CreatePanelControl(self, 0, "BUTTON", "Transfer", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[2], 48, nav_w, nav_h, hwnd, kIdTransfer);
            CreatePanelControl(self, 0, "BUTTON", "HDMI RX", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[3], 48, nav_w, nav_h, hwnd, kIdHdmiRx);
            CreatePanelControl(self, 0, "BUTTON", "HDMI TX", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[4], 48, nav_w, nav_h, hwnd, kIdHdmiTx);
            CreatePanelControl(self, 0, "BUTTON", "Keyboard", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[0], 92, nav_w, nav_h, hwnd, kIdKeyboard);
            CreatePanelControl(self, 0, "BUTTON", "Mouse", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[1], 92, nav_w, nav_h, hwnd, kIdMouse);
            CreatePanelControl(self, 0, "BUTTON", "H.264", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[2], 92, nav_w, nav_h, hwnd, kIdH264);
            CreatePanelControl(self, 0, "BUTTON", "Schema", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[3], 92, nav_w, nav_h, hwnd, kIdSchema);
            CreatePanelControl(self, 0, "BUTTON", "Runtime", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               nav_x[4], 92, nav_w, nav_h, hwnd, kIdRuntime);

            CreatePanelControl(self, 0, "STATIC", "Transfer test", WS_CHILD | WS_VISIBLE,
                               20, 144, 220, 26, hwnd, 0, true);
            CreatePanelControl(self, 0, "STATIC", "Payload KB (1-1023)", WS_CHILD | WS_VISIBLE,
                               20, 180, 160, 26, hwnd, 0);
            HWND payload_box = CreatePanelControl(self, 0, "COMBOBOX", "",
                               WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                               CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                               184, 176, 118, 220, hwnd, kIdPayloadKb);
            if (payload_box) {
                const char* values[] = {"1", "4", "16", "64", "256", "512", "1023"};
                for (const char* value : values)
                    SendMessageA(payload_box, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
                SetWindowTextA(payload_box, "64");
            }
            CreatePanelControl(self, 0, "STATIC", "Packets", WS_CHILD | WS_VISIBLE,
                               320, 180, 70, 26, hwnd, 0);
            CreatePanelControl(self, WS_EX_CLIENTEDGE, "EDIT", "100",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL,
                               392, 176, 92, 32, hwnd, kIdPacketCount);
            CreatePanelControl(self, 0, "BUTTON", "Ping", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               504, 174, 92, 36, hwnd, kIdPing);
            CreatePanelControl(self, 0, "BUTTON", "Start Test", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                               608, 174, 112, 36, hwnd, kIdStartTransfer);

            CreatePanelControl(self, 0, "STATIC", "Device state", WS_CHILD | WS_VISIBLE,
                               20, 226, 220, 26, hwnd, 0, true);
            self->state_hwnd = CreatePanelControl(self, WS_EX_CLIENTEDGE, "EDIT", "",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                               ES_AUTOVSCROLL | WS_VSCROLL,
                               20, 258, 700, 220, hwnd, kIdStateText);

            CreatePanelControl(self, 0, "STATIC", "Event log", WS_CHILD | WS_VISIBLE,
                               20, 494, 220, 26, hwnd, 0, true);
            self->log_hwnd = CreatePanelControl(self, WS_EX_CLIENTEDGE, "EDIT", "",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_READONLY |
                               ES_AUTOVSCROLL | WS_VSCROLL,
                               20, 526, 700, 248, hwnd, kIdLogText);
            SetTimer(hwnd, 1, 1000, nullptr);
            self->QueryState();
            RefreshPanel(self);
            return 0;
        }
        case WM_COMMAND:
            if (!self) break;
            switch (LOWORD(wparam)) {
                case kIdHome: self->SendNavigate(kavio_demo::Page::Home); break;
                case kIdRender: self->SendNavigate(kavio_demo::Page::Render); break;
                case kIdTransfer: self->SendNavigate(kavio_demo::Page::Transfer); break;
                case kIdHdmiRx: self->SendNavigate(kavio_demo::Page::HdmiRx); break;
                case kIdHdmiTx: self->SendNavigate(kavio_demo::Page::HdmiTx); break;
                case kIdKeyboard: self->SendNavigate(kavio_demo::Page::Keyboard); break;
                case kIdMouse: self->SendNavigate(kavio_demo::Page::Mouse); break;
                case kIdH264: self->SendNavigate(kavio_demo::Page::H264); break;
                case kIdSchema: self->SendNavigate(kavio_demo::Page::Schema); break;
                case kIdRuntime: self->SendNavigate(kavio_demo::Page::Runtime); break;
                case kIdPing: self->SendPing(); break;
                case kIdStartTransfer:
                    self->StartTransferTest(ReadUIntEdit(hwnd, kIdPayloadKb, 64),
                                            ReadUIntEdit(hwnd, kIdPacketCount, 100));
                    break;
                default: break;
            }
            return 0;
        case WM_TIMER:
            if (self) {
                self->QueryState();
                RefreshPanel(self);
            }
            return 0;
        case kPanelRefreshMessage:
            RefreshPanel(self);
            return 0;
        case WM_CTLCOLORSTATIC: {
            if (!self) break;
            HDC dc = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            SetTextColor(dc, RGB(32, 38, 46));
            if (control == self->state_hwnd || control == self->log_hwnd) {
                SetBkMode(dc, OPAQUE);
                SetBkColor(dc, RGB(255, 255, 255));
                return reinterpret_cast<INT_PTR>(self->edit_brush);
            }
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(self->panel_brush);
        }
        case WM_CTLCOLOREDIT: {
            if (!self) break;
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, RGB(32, 38, 46));
            SetBkColor(dc, RGB(255, 255, 255));
            return reinterpret_cast<INT_PTR>(self->edit_brush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (self) {
                self->panel_hwnd.store(nullptr);
                self->state_hwnd = nullptr;
                self->log_hwnd = nullptr;
                if (self->panel_font) { DeleteObject(self->panel_font); self->panel_font = nullptr; }
                if (self->section_font) { DeleteObject(self->section_font); self->section_font = nullptr; }
                if (self->panel_brush) { DeleteObject(self->panel_brush); self->panel_brush = nullptr; }
                if (self->edit_brush) { DeleteObject(self->edit_brush); self->edit_brush = nullptr; }
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

/** Bring an existing panel window to the foreground. */
void ActivatePanelWindow(HWND hwnd)
{
    if (!hwnd) return;
    ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);

    // The panel is hosted by a separate AppHost process. SetForegroundWindow() alone can
    // be rejected by Windows foreground-lock rules, so briefly raise it to TOPMOST and
    // immediately return it to a normal top-level window. It is not kept always-on-top.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
    SetFocus(hwnd);
}

/** Create and run the Win32 panel message loop. */
void PanelThreadMain(DemoHost* self)
{
    self->panel_thread_id.store(GetCurrentThreadId());
    // Force creation of this thread's message queue before another thread may request shutdown.
    MSG queue_probe{};
    PeekMessageW(&queue_probe, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    const wchar_t* class_name = L"KavioDemoPanel";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PanelWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, class_name, L"KavioDemo Panel",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 760, 840,
                                nullptr, nullptr, instance, self);
    if (!hwnd) {
        self->AppendLog("Panel creation failed.");
        self->panel_thread_id.store(0);
        return;
    }
    self->panel_hwnd.store(hwnd);
    ActivatePanelWindow(hwnd);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    self->panel_thread_id.store(0);
}

} // namespace

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostCreate(
    const KavioHostCreateParams* params,
    KavioHostHandle* out_host)
{
    if (!params || !out_host || !params->host || !params->host->send) return KavioResult_InvalidArgument;
    try {
        *out_host = new DemoHost(params->host);
        return KavioResult_Success;
    } catch (...) {
        return KavioResult_InternalError;
    }
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStart(KavioHostHandle host)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self) return KavioResult_InvalidArgument;
    self->started.store(true);
    self->AppendLog("Host Component started.");
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostPrepareStop(KavioHostHandle host)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self) return KavioResult_InvalidArgument;
    self->transfer_running.store(false);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStop(KavioHostHandle host)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self) return KavioResult_InvalidArgument;
    self->started.store(false);
    self->StopThreads();
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostDestroy(KavioHostHandle host)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self) return;
    self->started.store(false);
    self->StopThreads();
    delete self;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOnData(
    KavioHostHandle host,
    const void* data,
    size_t size)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self || !data || size == 0) return KavioResult_InvalidArgument;
    kavio_demo::Packet packet;
    if (!kavio_demo::ParsePacket(data, size, packet)) {
        self->AppendLog("Rejected invalid Demo protocol packet.");
        return KavioResult_InvalidArgument;
    }
    self->HandlePacket(packet);
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostOnDeviceState(
    KavioHostHandle host,
    uint32_t online)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self) return;
    self->device_online.store(online != 0);
    self->AppendLog(std::string("Device state: ") + (online ? "online" : "offline"));
    if (online) self->QueryState();
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOpenPanel(KavioHostHandle host)
{
    auto* self = static_cast<DemoHost*>(host);
    if (!self) return KavioResult_InvalidArgument;
    if (HWND hwnd = self->panel_hwnd.load()) {
        ActivatePanelWindow(hwnd);
        return KavioResult_Success;
    }
    if (self->panel_thread.joinable()) {
        // A joinable thread with a non-zero thread id is still creating/running the panel.
        // Do not block a repeated OpenPanel call waiting for the user to close it.
        if (self->panel_thread_id.load() != 0) return KavioResult_Success;
        self->panel_thread.join();
    }
    try {
        self->panel_thread = std::thread(PanelThreadMain, self);
        return KavioResult_Success;
    } catch (...) {
        return KavioResult_InternalError;
    }
}
