/**
 * @file main.cpp
 * @brief C++ device-side Kavio SDK feature demonstration.
 *
 * Demonstrates public device, presentation, App Channel, TX surface, input,
 * H.264, and application configuration APIs. The view is implemented in LVGL
 * and driven on the SDK application tick. Native device callbacks may run on
 * worker threads; use the pending-work queue before touching LVGL objects.
 */

#include <kavio/App.h>
#include <kavio/Device.h>
#include <kavio/Keyboard.h>
#include <kavio/ui/LvglBackend.h>
#include <kavio/ui/Widgets.h>

#include "DemoProtocol.h"
#include <json/json.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fstream>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

// Time values sent in Ping packets are nanoseconds from a monotonic clock;
// they are not wall-clock timestamps and cannot be compared across devices.
/** Return monotonic nanoseconds for device/host timing payloads. */
uint64_t SteadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

/** Format a boolean as a short user-facing text value. */
const char* YesNo(bool value) { return value ? "Yes" : "No"; }

/** Map a supported pixel format to a diagnostic name. */
const char* PixelFormatName(kavio::PixelFormat format)
{
    switch (format) {
        case kavio::PixelFormat::kUYVY: return "UYVY";
        case kavio::PixelFormat::kYUYV: return "YUYV";
        case kavio::PixelFormat::kBGRA8888: return "BGRA8888";
        default: return "Unknown";
    }
}

/** Parse application settings; return an empty object for invalid JSON. */
Json::Value ParseConfigJson(const std::string& encoded)
{
    Json::Value value(Json::objectValue);
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(encoded.data(), encoded.data() + encoded.size(), &value, &errors))
        return Json::Value(Json::objectValue);
    return value;
}

/** Serialize a JSON object into a compact settings string. */
std::string EncodeConfigJson(const Json::Value& value)
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, value);
}

/** Create an LVGL label at pixel coordinates in its parent.
 * @param parent Parent object that owns the label.
 * @param text UTF-8 caption, copied by LVGL.
 * @param x X offset in pixels.
 * @param y Y offset in pixels.
 * @param width Allocated label width in pixels.
 * @param height Allocated label height in pixels.
 * @param color RGB text color (0xRRGGBB).
 * @param font Optional LVGL font, defaults to 16 px.
 */
lv_obj_t* CreateLabel(lv_obj_t* parent,
                      const char* text,
                      int32_t x,
                      int32_t y,
                      int32_t width,
                      int32_t height,
                      uint32_t color,
                      const lv_font_t* font = &lv_font_montserrat_16)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_size(label, width, height);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    if (font) lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
    return label;
}

/** Create a fixed-position rounded LVGL container.
 * @param parent Parent object owning the card.
 * @param x Horizontal offset in pixels.
 * @param y Vertical offset in pixels.
 * @param width Card width in pixels.
 * @param height Card height in pixels.
 * @param color RGB fill color.
 */
lv_obj_t* CreateCard(lv_obj_t* parent,
                     int32_t x,
                     int32_t y,
                     int32_t width,
                     int32_t height,
                     uint32_t color = 0x132238)
{
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_set_style_bg_color(card, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x263B56), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

/** Create a touch button whose click callback receives user_data.
 * @param parent Parent LVGL object.
 * @param text Visible button caption.
 * @param x Horizontal offset in pixels.
 * @param y Vertical offset in pixels.
 * @param width Button width in pixels.
 * @param height Button height in pixels.
 * @param callback Click event callback.
 * @param user_data Callback context; owned by the caller.
 * @param color RGB button fill.
 */
lv_obj_t* CreateButton(lv_obj_t* parent,
                       const char* text,
                       int32_t x,
                       int32_t y,
                       int32_t width,
                       int32_t height,
                       lv_event_cb_t callback,
                       void* user_data,
                       uint32_t color = 0x276BBD)
{
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_bg_color(button, lv_color_hex(color), 0);
    lv_obj_set_style_radius(button, 14, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
    lv_obj_center(label);
    return button;
}

/**
 * @brief Owns the demo's device session, UI tree, and asynchronous resources.
 *
 * Device callbacks enqueue or snapshot state. Only the UI tick and UI event
 * callbacks manipulate LVGL objects; TX/H.264 resources have explicit cleanup.
 */
class KavioDemo final {
    // All three demos use identical image conversion bounds (pixels).
    static constexpr uint32_t kRxPreviewWidth = 620;
    static constexpr uint32_t kRxPreviewHeight = 360;
    static constexpr uint32_t kRxPreviewAreaWidth = 640;
    static constexpr uint32_t kRxPreviewAreaHeight = 368;
public:
    /** Start the SDK lifecycle and return a process exit status. */
    int Run()
    {
        kavio::AppOptions options;
        options.client_name = "kavio_demo_cpp";
        options.rx_enabled = true;
        options.rx_frame_sample_interval = 1;  // Request every RX frame for unthrottled preview.
        options.tx_enabled = true;
        options.tx_z_order = 0;
        options.idle_sleep_ms = 5;

        kavio::App app(options);
        app.on_ready([this, &app](kavio::AppContext& context) {
            device_ = &context.device();
            SetupDeviceHandlers();

            const kavio::Status status = view_.Initialize(context.presentation());
            if (!status.ok()) {
                std::cerr << "LVGL backend initialization failed: " << status.message << '\n';
                startup_failed_.store(true);
                app.request_stop();
                return;
            }

            ApplyConfig(true);
            CreateUi();
            const auto now = std::chrono::steady_clock::now();
            next_ui_update_ = now;
            next_config_check_ = now;
            next_render_frame_ = now;
            next_hello_check_ = now;
            next_tx_move_ = now;
        });

        // RX preview attempts one read per UI tick when enabled. Actual FPS is
        // measured from distinct frames successfully submitted to the image widget.
        app.on_tick([this](kavio::AppContext&) {
            if (!device_ || !view_.IsReady()) return;
            view_.MakeDefault();
            ProcessPendingWork();

            const auto now = std::chrono::steady_clock::now();
            if (config_dirty_.exchange(false) || now >= next_config_check_) {
                ApplyConfig(false);
                next_config_check_ = now + std::chrono::seconds(1);
            }
            if (now >= next_hello_check_) {
                UpdateChannelHandshake();
                next_hello_check_ = now + std::chrono::milliseconds(500);
            }
            if (now >= next_ui_update_) {
                UpdateVisiblePage();
                next_ui_update_ = now + std::chrono::milliseconds(250);
            }
            const int frame_interval_ms = std::max(16, 1000 / std::max(1, render_fps_));
            if (current_page_ == kavio_demo::Page::Render && now >= next_render_frame_) {
                UpdateRenderAnimation();
                next_render_frame_ = now + std::chrono::milliseconds(frame_interval_ms);
            }
            if (current_page_ == kavio_demo::Page::HdmiRx && rx_live_) {
                ReadRxSample(true);
                UpdateRxFps(now);
            }
            if (now >= next_tx_move_) {
                UpdateTxOverlaysMotion();
                next_tx_move_ = now + std::chrono::milliseconds(33);
            }
            view_.Process();
        });

        app.on_stopping([this](kavio::AppContext&) {
            StopH264Capture(true);
            ClearTxOverlays();
            view_.Shutdown();
            device_ = nullptr;
        });

        const int result = app.run();
        return result == 0 && !startup_failed_.load() ? 0 : 1;
    }

/** Register SDK callbacks and synchronize worker-thread notifications. */
private:
    // --- Device event registration and configuration ---
    void SetupDeviceHandlers()
    {
        if (!device_) return;
        device_->SetCommDataHandler([this](const std::vector<uint8_t>& bytes) {
            kavio_demo::Packet packet;
            if (!kavio_demo::ParsePacket(bytes.data(), bytes.size(), packet)) {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                ++protocol_errors_;
                return;
            }
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_packets_.push_back(std::move(packet));
        });
        device_->SetTxSurfaceHandler([this](uint64_t request_id,
                                            kavio::Status status,
                                            kavio::DeviceTxSurface surface) {
            OpenAndFillTxSurface(request_id, status, std::move(surface));
        });
        device_->SetH264StreamHandler([this](uint64_t request_id,
                                             kavio::Status status,
                                             kavio::DeviceH264StreamInfo info) {
            OpenH264Capture(request_id, status, info);
        });
        device_->SetH264InvalidatedHandler([this](const kavio::DeviceH264InvalidatedEvent& event) {
            {
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "Stream invalidated, reason=" + std::to_string(event.reason);
            }
            h264_capture_running_.store(false);
            h264_invalidated_.store(true);
        });
        device_->SetErrorHandler([this](const kavio::DeviceErrorEvent& event) {
            if (event.track_id == h264_subscribe_request_id_.load()) {
                h264_subscribe_request_id_ = 0;
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "Subscribe failed: " + event.message;
            }
        });
        device_->SetConfigChangedHandler([this]() {
            config_dirty_.store(true);
        });
    }
    /** Load configuration and apply the UI settings to current state. */
    void ApplyConfig(bool first_load)
    {
        const Json::Value config = ParseConfigJson(device_->GetAppConfigJson());
        const int new_render_fps = std::max(5, std::min(60, config.get("render_fps", 20).asInt()));
        const bool new_show_metrics = config.get("show_metrics", true).asBool();
        const int new_payload_kb = std::max(1, std::min(1023, config.get("transfer_payload_kb", 64).asInt()));
        render_fps_ = new_render_fps;
        show_metrics_ = new_show_metrics;
        transfer_payload_kb_ = new_payload_kb;
        if (render_metrics_card_) {
            if (show_metrics_) lv_obj_clear_flag(render_metrics_card_, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(render_metrics_card_, LV_OBJ_FLAG_HIDDEN);
        }

        if (first_load) {
            const std::string default_page = config.get("default_page", "home").asString();
            if (default_page == "render") current_page_ = kavio_demo::Page::Render;
            else if (default_page == "transfer") current_page_ = kavio_demo::Page::Transfer;
            else if (default_page == "hdmi_rx") current_page_ = kavio_demo::Page::HdmiRx;
            else if (default_page == "hdmi_tx") current_page_ = kavio_demo::Page::HdmiTx;
            else if (default_page == "keyboard") current_page_ = kavio_demo::Page::Keyboard;
            else if (default_page == "mouse") current_page_ = kavio_demo::Page::Mouse;
            else if (default_page == "h264") current_page_ = kavio_demo::Page::H264;
            else if (default_page == "schema") current_page_ = kavio_demo::Page::Schema;
            else if (default_page == "runtime") current_page_ = kavio_demo::Page::Runtime;
            else current_page_ = kavio_demo::Page::Home;
        }
    }
    // --- Page construction and navigation ---
    /** Create the initial device page. */
    void CreateUi()
    {
        ShowPage(current_page_);
    }
    /** Invalidate borrowed LVGL widget pointers before replacing the screen. */
    void ResetPagePointers()
    {
        page_title_ = nullptr;
        home_status_label_ = nullptr;
        home_scroll_ = nullptr;
        render_frame_label_ = nullptr;
        render_fps_label_ = nullptr;
        render_pause_label_ = nullptr;
        render_metrics_card_ = nullptr;
        transfer_state_label_ = nullptr;
        transfer_metric_label_ = nullptr;
        transfer_protocol_label_ = nullptr;
        rx_state_label_ = nullptr;
        rx_fps_label_ = nullptr;
        rx_preview_canvas_ = nullptr;
        rx_preview_image_.SetSource({});  // Canvas is gone; release converted pixels.
        rx_preview_placeholder_ = nullptr;
        tx_state_label_ = nullptr;
        keyboard_state_label_ = nullptr;
        mouse_state_label_ = nullptr;
        mouse_pad_ = nullptr;
        mouse_cursor_dot_ = nullptr;
        h264_state_label_ = nullptr;
        h264_metric_label_ = nullptr;
        schema_state_label_ = nullptr;
        schema_value_label_ = nullptr;
        runtime_summary_label_ = nullptr;
        wave_bars_.fill(nullptr);
    }
    /** Clear the active screen and construct the shared page heading. */
    void PrepareScreen(const char* title)
    {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_clean(screen);
        ResetPagePointers();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0A1220), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);
        page_title_ = CreateLabel(screen, title, 26, 18, 470, 34, 0xF5F8FC, &lv_font_montserrat_24);
        CreateLabel(screen, "KAVIO DEMO", 502, 22, 190, 24, 0x6F8EAF, &lv_font_montserrat_16);
    }
    /** Replace the active page and reset page-specific runtime state. */
    void ShowPage(kavio_demo::Page page)
    {
        current_page_ = page;
        render_paused_ = false;
        rx_live_ = false;  // Never continue reading after the preview is unmounted.
        last_rx_frame_id_ = 0;
        rx_fps_frames_ = 0;
        rx_fps_ = 0.0;
        rx_fps_window_start_ = {};
        switch (page) {
            /** Construct the capability overview cards. */
            case kavio_demo::Page::Home: CreateHomePage(); break;
            /** Build LVGL render animation and controls. */
            case kavio_demo::Page::Render: CreateRenderPage(); break;
            /** Build App Channel transfer controls and metrics. */
            case kavio_demo::Page::Transfer: CreateTransferPage(); break;
            /** Build HDMI RX status and SDK Image RGB565 preview. */
            case kavio_demo::Page::HdmiRx: CreateHdmiRxPage(); break;
            /** Build TX surface controls and status. */
            case kavio_demo::Page::HdmiTx: CreateHdmiTxPage(); break;
            /** Build keyboard input controls. */
            case kavio_demo::Page::Keyboard: CreateKeyboardPage(); break;
            /** Build virtual mouse controls. */
            case kavio_demo::Page::Mouse: CreateMousePage(); break;
            /** Build H.264 capture controls and metrics. */
            case kavio_demo::Page::H264: CreateH264Page(); break;
            /** Build configuration read/update controls. */
            case kavio_demo::Page::Schema: CreateSchemaPage(); break;
            /** Build device runtime status view. */
            case kavio_demo::Page::Runtime: CreateRuntimePage(); break;
        }
        SendStateReport();
    }

    void CreateHomePage()
    {
        PrepareScreen("KavioDemo Capabilities");

        home_scroll_ = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(home_scroll_, 18, 58);
        lv_obj_set_size(home_scroll_, 684, 644);
        lv_obj_set_style_bg_opa(home_scroll_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(home_scroll_, 0, 0);
        lv_obj_set_style_pad_all(home_scroll_, 10, 0);
        lv_obj_set_style_pad_row(home_scroll_, 16, 0);
        lv_obj_set_flex_flow(home_scroll_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(home_scroll_, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(home_scroll_, LV_SCROLLBAR_MODE_AUTO);

        CreateLabel(home_scroll_,
                    "One App demonstrates the public Device SDK, Host SDK, App Panel and App Configuration.",
                    0, 0, 644, 44, 0x8EABC9, &lv_font_montserrat_16);

        CreateFeatureCard(home_scroll_, "RENDER", "AppView + LVGL",
                          "Full-screen BGRA8888 rendering, touch and partial updates.",
                          RenderNav, 0x1E5DA8);
        CreateFeatureCard(home_scroll_, "TRANSFER", "USB App Channel",
                          "Bidirectional datagrams, ping and payload integrity testing.",
                          TransferNav, 0x276BBD);
        CreateFeatureCard(home_scroll_, "HDMI RX", "Read + Preview",
                          "Read one HDMI RX frame and scale it into the AppView preview area.",
                          HdmiRxNav, 0x1B7C6D);
        CreateFeatureCard(home_scroll_, "HDMI TX", "Multiple TX Surfaces",
                          "Create up to three BGRA TX surfaces and move them automatically.",
                          HdmiTxNav, 0x387C91);
        CreateFeatureCard(home_scroll_, "KEYBOARD", "HID Keyboard Output",
                          "Test letters, numbers, function keys, navigation keys and other system keys.",
                          KeyboardNav, 0x8B5F3C);
        CreateFeatureCard(home_scroll_, "MOUSE", "Pointer Output",
                          "Use the touch pad to drive absolute mouse coordinates in real time, plus click and wheel tests.",
                          MouseNav, 0x715A8D);
        CreateFeatureCard(home_scroll_, "H.264 CAPTURE", "Subscribe + Save",
                          "Subscribe to the public H.264 stream and save Annex-B packets into the App cache directory.",
                          H264Nav, 0x7A4E9D);
        CreateFeatureCard(home_scroll_, "SCHEMA UPDATE", "App Configuration",
                          "Read, update and reset persistent configuration through the SDK-managed Schema contract.",
                          SchemaNav, 0x4F718C);
        CreateFeatureCard(home_scroll_, "RUNTIME", "State + Statistics",
                          "Inspect Session state and query Platform runtime statistics.",
                          RuntimeNav, 0x6F55B5);

        lv_obj_t* status = CreateCard(home_scroll_, 0, 0, 644, 142, 0x111F32);
        lv_obj_set_flex_grow(status, 0);
        CreateLabel(status, "CURRENT STATE", 18, 14, 220, 24, 0x7F9BB9, &lv_font_montserrat_16);
        home_status_label_ = CreateLabel(status, "Initializing...", 18, 45, 608, 76, 0xE6F1FC, &lv_font_montserrat_16);
    }
    /** Create one navigable entry on the home screen. */
    void CreateFeatureCard(lv_obj_t* parent,
                           const char* title, const char* subtitle, const char* description,
                           lv_event_cb_t cb, uint32_t color)
    {
        lv_obj_t* card = CreateCard(parent, 0, 0, 644, 142, 0x132238);
        lv_obj_set_flex_grow(card, 0);
        CreateLabel(card, title, 18, 14, 240, 28, 0xF5F8FC, &lv_font_montserrat_20);
        CreateLabel(card, subtitle, 18, 46, 330, 22, 0x67A9FF, &lv_font_montserrat_16);
        CreateLabel(card, description, 18, 76, 460, 48, 0x8EABC9, &lv_font_montserrat_16);
        CreateButton(card, "OPEN", 514, 49, 108, 44, cb, this, color);
    }
    /** Create a page-level return button. */
    void CreateBackButton()
    {
        CreateButton(lv_screen_active(), "BACK", 28, 652, 112, 44, HomeNav, this, 0x304660);
    }

    void CreateRenderPage()
    {
        PrepareScreen("Render / AppView");
        CreateBackButton();

        lv_obj_t* summary = CreateCard(lv_screen_active(), 28, 66, 676, 110);
        CreateLabel(summary, "AppViewSurface", 18, 14, 220, 24, 0x7F9BB9, &lv_font_montserrat_16);
        CreateLabel(summary, "BGRA8888 + LVGL 9.4", 18, 42, 330, 34, 0xF5F8FC, &lv_font_montserrat_24);
        render_fps_label_ = CreateLabel(summary, "Target 20 FPS", 450, 46, 190, 28, 0x78E6BB, &lv_font_montserrat_16);

        lv_obj_t* animation = CreateCard(lv_screen_active(), 28, 194, 676, 278, 0x111F32);
        CreateLabel(animation, "PARTIAL RENDER ACTIVITY", 18, 12, 320, 24, 0xDDEBFA, &lv_font_montserrat_16);
        CreateLabel(animation, "The moving bars continuously generate local LVGL invalidation regions.",
                    18, 39, 470, 40, 0x718DAE, &lv_font_montserrat_16);
        CreateButton(animation, "PAUSE", 510, 14, 140, 44, ToggleRender, this);
        render_pause_label_ = CreateLabel(animation, "Running", 510, 67, 140, 24, 0x78E6BB, &lv_font_montserrat_16);

        for (size_t i = 0; i < wave_bars_.size(); ++i) {
            lv_obj_t* bar = lv_obj_create(animation);
            lv_obj_set_size(bar, 18, 30);
            lv_obj_set_pos(bar, 30 + static_cast<int32_t>(i) * 39, 232 - 30);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x397DE0 + static_cast<uint32_t>((i % 3) * 0x071510)), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_radius(bar, 5, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            wave_bars_[i] = bar;
        }

        lv_obj_t* metrics = CreateCard(lv_screen_active(), 28, 490, 676, 142);
        render_metrics_card_ = metrics;
        if (!show_metrics_) lv_obj_add_flag(metrics, LV_OBJ_FLAG_HIDDEN);
        CreateLabel(metrics, "FRAME COUNT", 18, 12, 180, 24, 0x7F9BB9, &lv_font_montserrat_16);
        render_frame_label_ = CreateLabel(metrics, "0", 18, 39, 220, 34, 0xF5F8FC, &lv_font_montserrat_24);
        CreateLabel(metrics, "PARTIAL UPDATE MODE", 280, 12, 260, 24, 0x7F9BB9, &lv_font_montserrat_16);
        CreateLabel(metrics, "Continuous local invalidation from animated bars",
                    280, 43, 360, 48, 0x78E6BB, &lv_font_montserrat_16);
        CreateLabel(metrics, show_metrics_ ? "Metrics enabled by App Configuration" : "Metrics hidden by App Configuration",
                    18, 94, 620, 24, 0x718DAE, &lv_font_montserrat_16);
    }

    void CreateTransferPage()
    {
        PrepareScreen("Transfer / USB Channel");
        CreateBackButton();
        lv_obj_t* state = CreateCard(lv_screen_active(), 28, 66, 676, 142);
        CreateLabel(state, "CHANNEL STATE", 18, 14, 220, 24, 0x7F9BB9, &lv_font_montserrat_16);
        transfer_state_label_ = CreateLabel(state, "Waiting for Host Component...", 18, 44, 630, 70, 0xE6F1FC, &lv_font_montserrat_18);

        lv_obj_t* metrics = CreateCard(lv_screen_active(), 28, 226, 676, 204, 0x111F32);
        CreateLabel(metrics, "DEVICE RECEIVE METRICS", 18, 14, 300, 24, 0x7F9BB9, &lv_font_montserrat_16);
        transfer_metric_label_ = CreateLabel(metrics, "Packets: 0\nBytes: 0\nLast checksum: 0", 18, 48, 420, 120,
                                             0xF5F8FC, &lv_font_montserrat_18);
        CreateLabel(metrics, "Run Ping and throughput tests from the Windows Host Panel.",
                    438, 52, 204, 86, 0x78E6BB, &lv_font_montserrat_16);

        lv_obj_t* protocol = CreateCard(lv_screen_active(), 28, 448, 676, 184);
        CreateLabel(protocol, "APP-LOCAL DEMO PROTOCOL", 18, 14, 330, 24, 0x7F9BB9, &lv_font_montserrat_16);
        transfer_protocol_label_ = CreateLabel(protocol,
            "Shared only by this Demo's Device and Host components.\n"
            "Transport: one complete App Channel datagram per message.\n"
            "Payload default: 64 KB (configurable in standard App Settings).",
            18, 48, 630, 108, 0xDDEBFA, &lv_font_montserrat_16);
    }

    void CreateHdmiRxPage()
    {
        PrepareScreen("HDMI RX / Frame Preview");
        CreateBackButton();

        lv_obj_t* state = CreateCard(lv_screen_active(), 28, 66, 676, 116);
        CreateLabel(state, "HDMI RX", 18, 12, 160, 24, 0x7F9BB9, &lv_font_montserrat_16);
        rx_state_label_ = CreateLabel(state, "Reading RX state...", 18, 40, 430, 62, 0xF5F8FC, &lv_font_montserrat_16);
        CreateButton(state, "READ ONE FRAME", 470, 36, 178, 46, ReadRxFrame, this);

        lv_obj_t* preview = CreateCard(lv_screen_active(), 28, 198, 676, 434, 0x111F32);
        CreateLabel(preview, "RX IMAGE PREVIEW", 18, 12, 260, 24, 0x7F9BB9, &lv_font_montserrat_16);
        rx_fps_label_ = CreateLabel(preview, "FPS: 0.0", 295, 12, 160, 28, 0x78E6BB, &lv_font_montserrat_16);
        CreateButton(preview, "START LIVE", 470, 6, 178, 38, ToggleRxLive, this);
        rx_preview_placeholder_ = CreateLabel(preview,
            "Press READ ONE FRAME or enable LIVE. The latest HDMI RX frame appears here.",
            58, 180, 560, 60, 0x718DAE, &lv_font_montserrat_16);
        lv_obj_set_style_text_align(rx_preview_placeholder_, LV_TEXT_ALIGN_CENTER, 0);
        rx_preview_canvas_ = lv_image_create(preview);
        lv_obj_add_flag(rx_preview_canvas_, LV_OBJ_FLAG_HIDDEN);
    }

    void CreateHdmiTxPage()
    {
        PrepareScreen("HDMI TX / Moving Surfaces");
        CreateBackButton();

        lv_obj_t* control = CreateCard(lv_screen_active(), 28, 66, 676, 146);
        CreateLabel(control, "TX SURFACE TEST", 18, 14, 220, 24, 0x7F9BB9, &lv_font_montserrat_16);
        tx_state_label_ = CreateLabel(control, "No demo surfaces", 18, 44, 360, 76, 0xF5F8FC, &lv_font_montserrat_16);
        CreateButton(control, "CREATE", 374, 34, 92, 46, CreateOverlay, this);
        CreateButton(control, "PARTIAL", 476, 34, 92, 46, CreatePartialOverlay, this, 0x6F55B5);
        CreateButton(control, "CLEAR", 578, 34, 72, 46, ClearOverlays, this, 0x834A55);
        CreateLabel(control, "CREATE: 200x120 moving surface. PARTIAL: 320x200 surface with a moving valid_rect. Max 3 total.",
                    374, 88, 270, 52, 0x718DAE, &lv_font_montserrat_16);

        lv_obj_t* info = CreateCard(lv_screen_active(), 28, 230, 676, 402, 0x111F32);
        CreateLabel(info, "AUTOMATIC MOVEMENT", 18, 14, 260, 24, 0x7F9BB9, &lv_font_montserrat_16);
        CreateLabel(info,
            "Created TX surfaces are independent of this AppView.\n\n"
            "Normal Demo surfaces update dest_pos at about 30 Hz.\n\n"
            "PARTIAL creates a larger fixed TX surface and moves ImageFrame.valid_rect inside it. "
            "This demonstrates partial publish/composition. WriteFrame still copies the full SHM payload; "
            "valid_rect is not a partial-memory-copy API.\n\n"
            "Maximum active/pending surfaces: 3.",
            18, 52, 630, 230, 0xDDEBFA, &lv_font_montserrat_16);
    }

    struct KeyBinding {
        KavioDemo* owner = nullptr;
        kavio::KeyCode key = kavio::KeyCode::kReserved;
        std::string label;
    };

    void AddKeyboardKeyButton(lv_obj_t* parent,
                              const char* label,
                              kavio::KeyCode key,
                              int32_t x,
                              int32_t y,
                              int32_t width = 94,
                              int32_t height = 48,
                              uint32_t color = 0x276BBD)
    {
        keyboard_bindings_.push_back(KeyBinding{this, key, label});
        CreateButton(parent, label, x, y, width, height,
                     KeyboardKeyPressed, &keyboard_bindings_.back(), color);
    }

    void CreateKeyboardPage()
    {
        PrepareScreen("Keyboard Output");
        CreateBackButton();
        keyboard_bindings_.clear();

        lv_obj_t* scroll = lv_obj_create(lv_screen_active());
        lv_obj_set_pos(scroll, 18, 62);
        lv_obj_set_size(scroll, 684, 570);
        lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(scroll, 0, 0);
        lv_obj_set_style_pad_all(scroll, 10, 0);
        lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);

        lv_obj_t* letters = CreateCard(scroll, 0, 0, 644, 300, 0x132238);
        CreateLabel(letters, "LETTERS A-Z", 18, 14, 260, 28, 0x7F9BB9, &lv_font_montserrat_16);
        for (int i = 0; i < 26; ++i) {
            char label[2] = {static_cast<char>('A' + i), 0};
            const auto key = static_cast<kavio::KeyCode>(
                kavio::KeyCodeValue(kavio::KeyCode::kA) + i);
            const int col = i % 6;
            const int row = i / 6;
            AddKeyboardKeyButton(letters, label, key, 18 + col * 102, 52 + row * 50,
                                 94, 44, 0x276BBD);
        }

        lv_obj_t* numbers = CreateCard(scroll, 0, 316, 644, 158, 0x111F32);
        CreateLabel(numbers, "NUMBERS 0-9", 18, 14, 260, 28, 0x7F9BB9, &lv_font_montserrat_16);
        const kavio::KeyCode number_keys[10] = {
            kavio::KeyCode::kDigit0, kavio::KeyCode::kDigit1,
            kavio::KeyCode::kDigit2, kavio::KeyCode::kDigit3,
            kavio::KeyCode::kDigit4, kavio::KeyCode::kDigit5,
            kavio::KeyCode::kDigit6, kavio::KeyCode::kDigit7,
            kavio::KeyCode::kDigit8, kavio::KeyCode::kDigit9,
        };
        for (int i = 0; i < 10; ++i) {
            char label[2] = {static_cast<char>('0' + i), 0};
            AddKeyboardKeyButton(numbers, label, number_keys[i], 18 + (i % 6) * 102,
                                 52 + (i / 6) * 50, 94, 44, 0x1B7C6D);
        }

        lv_obj_t* system = CreateCard(scroll, 0, 490, 644, 374, 0x132238);
        CreateLabel(system, "SYSTEM / NAVIGATION / FUNCTION KEYS", 18, 14, 440, 28,
                    0x7F9BB9, &lv_font_montserrat_16);
        struct KeyDef { const char* label; kavio::KeyCode key; };
        const KeyDef system_keys[] = {
            {"ENTER", kavio::KeyCode::kEnter}, {"ESC", kavio::KeyCode::kEscape},
            {"BACK", kavio::KeyCode::kBackspace}, {"TAB", kavio::KeyCode::kTab},
            {"SPACE", kavio::KeyCode::kSpace}, {"CAPS", kavio::KeyCode::kCapsLock},
            {"INS", kavio::KeyCode::kInsert}, {"DEL", kavio::KeyCode::kDelete},
            {"HOME", kavio::KeyCode::kHome}, {"END", kavio::KeyCode::kEnd},
            {"PGUP", kavio::KeyCode::kPageUp}, {"PGDN", kavio::KeyCode::kPageDown},
            {"LEFT", kavio::KeyCode::kLeft}, {"RIGHT", kavio::KeyCode::kRight},
            {"UP", kavio::KeyCode::kUp}, {"DOWN", kavio::KeyCode::kDown},
            {"PRINT", kavio::KeyCode::kPrintScreen}, {"PAUSE", kavio::KeyCode::kPause},
            {"CTRL", kavio::KeyCode::kLeftCtrl}, {"SHIFT", kavio::KeyCode::kLeftShift},
            {"ALT", kavio::KeyCode::kLeftAlt}, {"WIN", kavio::KeyCode::kLeftGui},
            {"F1", kavio::KeyCode::kF1}, {"F2", kavio::KeyCode::kF2},
            {"F3", kavio::KeyCode::kF3}, {"F4", kavio::KeyCode::kF4},
            {"F5", kavio::KeyCode::kF5}, {"F6", kavio::KeyCode::kF6},
            {"F7", kavio::KeyCode::kF7}, {"F8", kavio::KeyCode::kF8},
            {"F9", kavio::KeyCode::kF9}, {"F10", kavio::KeyCode::kF10},
            {"F11", kavio::KeyCode::kF11}, {"F12", kavio::KeyCode::kF12},
        };
        for (size_t i = 0; i < sizeof(system_keys) / sizeof(system_keys[0]); ++i) {
            AddKeyboardKeyButton(system, system_keys[i].label, system_keys[i].key,
                                 18 + static_cast<int>(i % 6) * 102,
                                 52 + static_cast<int>(i / 6) * 50,
                                 94, 44, 0x6F55B5);
        }

        lv_obj_t* status = CreateCard(scroll, 0, 880, 644, 84, 0x111F32);
        CreateLabel(status, "LAST RESULT", 18, 12, 140, 24, 0x7F9BB9, &lv_font_montserrat_16);
        keyboard_state_label_ = CreateLabel(status, last_input_result_.c_str(), 18, 42, 606, 30,
                                            0xF5F8FC, &lv_font_montserrat_16);
    }

    void CreateMousePage()
    {
        PrepareScreen("Mouse Output");
        CreateBackButton();

        lv_obj_t* card = CreateCard(lv_screen_active(), 28, 66, 676, 446, 0x111F32);
        CreateLabel(card, "TOUCH PAD", 18, 14, 220, 28, 0x7F9BB9, &lv_font_montserrat_16);
        CreateLabel(card, "Drag your finger inside the area. The touch position is mapped to 0..65535 mouse coordinates in real time.",
                    18, 42, 630, 48, 0xDDEBFA, &lv_font_montserrat_16);

        mouse_pad_ = lv_obj_create(card);
        lv_obj_set_pos(mouse_pad_, 18, 102);
        lv_obj_set_size(mouse_pad_, 640, 302);
        lv_obj_set_style_bg_color(mouse_pad_, lv_color_hex(0x0D1A2A), 0);
        lv_obj_set_style_bg_opa(mouse_pad_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(mouse_pad_, lv_color_hex(0x4E7EAA), 0);
        lv_obj_set_style_border_width(mouse_pad_, 2, 0);
        lv_obj_set_style_radius(mouse_pad_, 14, 0);
        lv_obj_set_style_pad_all(mouse_pad_, 0, 0);
        lv_obj_clear_flag(mouse_pad_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(mouse_pad_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(mouse_pad_, MousePadEvent, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(mouse_pad_, MousePadEvent, LV_EVENT_PRESSING, this);
        CreateLabel(mouse_pad_, "TOUCH / DRAG HERE", 214, 132, 230, 30, 0x4E7EAA, &lv_font_montserrat_16);
        mouse_cursor_dot_ = lv_obj_create(mouse_pad_);
        lv_obj_set_size(mouse_cursor_dot_, 18, 18);
        lv_obj_set_style_radius(mouse_cursor_dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(mouse_cursor_dot_, lv_color_hex(0x78E6BB), 0);
        lv_obj_set_style_bg_opa(mouse_cursor_dot_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(mouse_cursor_dot_, 0, 0);
        lv_obj_clear_flag(mouse_cursor_dot_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(mouse_cursor_dot_, 311, 142);

        lv_obj_t* controls = CreateCard(lv_screen_active(), 28, 530, 676, 102, 0x132238);
        CreateButton(controls, "LEFT CLICK", 18, 18, 142, 48, MouseLeftClick, this, 0x276BBD);
        CreateButton(controls, "RIGHT CLICK", 174, 18, 152, 48, MouseRightClick, this, 0x276BBD);
        CreateButton(controls, "WHEEL +", 340, 18, 130, 48, MouseWheelUp, this, 0x1B7C6D);
        CreateButton(controls, "WHEEL -", 484, 18, 130, 48, MouseWheelDown, this, 0x1B7C6D);
        mouse_state_label_ = CreateLabel(controls, last_input_result_.c_str(), 18, 72, 630, 24,
                                         0x78E6BB, &lv_font_montserrat_16);
    }

    void CreateH264Page()
    {
        PrepareScreen("H.264 / Subscribe & Save");
        CreateBackButton();

        lv_obj_t* control = CreateCard(lv_screen_active(), 28, 66, 676, 160);
        CreateLabel(control, "PUBLIC H.264 STREAM", 18, 14, 280, 24, 0x7F9BB9, &lv_font_montserrat_16);
        CreateButton(control, "START", 472, 24, 80, 44, StartH264, this, 0x276BBD);
        CreateButton(control, "STOP", 566, 24, 80, 44, StopH264, this, 0x834A55);
        h264_state_label_ = CreateLabel(control, "Idle", 18, 48, 420, 92, 0xF5F8FC, &lv_font_montserrat_16);

        lv_obj_t* metrics = CreateCard(lv_screen_active(), 28, 244, 676, 214, 0x111F32);
        CreateLabel(metrics, "CAPTURE STATUS", 18, 14, 240, 24, 0x7F9BB9, &lv_font_montserrat_16);
        h264_metric_label_ = CreateLabel(metrics, "No capture started.", 18, 48, 630, 142, 0xDDEBFA, &lv_font_montserrat_16);

        lv_obj_t* note = CreateCard(lv_screen_active(), 28, 476, 676, 156, 0x132238);
        CreateLabel(note, "RAW ANNEX-B ONLY", 18, 14, 220, 24, 0x7F9BB9, &lv_font_montserrat_16);
        CreateLabel(note,
                    "The Demo does not decode or play video. It subscribes through the Kavio Device API, reads H.264 packets "
                    "from the SDK SHM and appends them to $KAVIO_APP_DATA_DIR/cache/*.h264.",
                    18, 48, 630, 86, 0xDDEBFA, &lv_font_montserrat_16);
        UpdateH264Labels();
    }

    void CreateSchemaPage()
    {
        PrepareScreen("Schema / Configuration Update");
        CreateBackButton();

        lv_obj_t* current = CreateCard(lv_screen_active(), 28, 66, 676, 220);
        CreateLabel(current, "SDK-MANAGED APP CONFIGURATION", 18, 14, 360, 24, 0x7F9BB9, &lv_font_montserrat_16);
        schema_value_label_ = CreateLabel(current, "Loading...", 18, 48, 630, 150, 0xF5F8FC, &lv_font_montserrat_16);

        lv_obj_t* controls = CreateCard(lv_screen_active(), 28, 304, 676, 250, 0x111F32);
        CreateLabel(controls, "UPDATE VALUES THROUGH SCHEMA", 18, 14, 360, 24, 0x7F9BB9, &lv_font_montserrat_16);
        CreateButton(controls, "FPS +5", 18, 54, 132, 44, SchemaFpsPlus, this, 0x276BBD);
        CreateButton(controls, "METRICS", 166, 54, 132, 44, SchemaToggleMetrics, this, 0x1B7C6D);
        CreateButton(controls, "PAYLOAD x2", 314, 54, 150, 44, SchemaPayloadDouble, this, 0x6F55B5);
        CreateButton(controls, "RESET", 480, 54, 150, 44, SchemaReset, this, 0x834A55);
        CreateLabel(controls,
                    "UpdateAppConfigJson() validates the complete JSON against schema_description.json and atomically "
                    "persists config.json. RESET restores schema defaults. The Schema definition itself remains static.",
                    18, 118, 630, 90, 0xDDEBFA, &lv_font_montserrat_16);
        schema_state_label_ = CreateLabel(controls, "Ready", 18, 212, 620, 24, 0x78E6BB, &lv_font_montserrat_16);
        UpdateSchemaLabels();
    }

    void CreateRuntimePage()
    {
        PrepareScreen("Runtime / Diagnostics");
        CreateBackButton();
        lv_obj_t* summary = CreateCard(lv_screen_active(), 28, 66, 676, 222);
        CreateLabel(summary, "SESSION SNAPSHOT", 18, 14, 240, 24, 0x7F9BB9, &lv_font_montserrat_16);
        runtime_summary_label_ = CreateLabel(summary, "Loading...", 18, 48, 630, 148, 0xF5F8FC, &lv_font_montserrat_16);

    }
    // --- Page state and visualization updates ---
    /** Refresh labels for the currently active page. */
    void UpdateVisiblePage()
    {
        if (current_page_ == kavio_demo::Page::Home && home_status_label_) {
            char text[256]{};
            std::snprintf(text, sizeof(text),
                          "Device Session: %s\nHost: %s   App Channel: %s   Channel ID: %u\nRX: %s   TX demo surfaces: %zu",
                          YesNo(device_->IsReady()), YesNo(device_->IsUSBHostOnline()),
                          YesNo(device_->IsCommChannelReady()), device_->GetCommChannelId(),
                          YesNo(device_->GetRxInfo().connected), ActiveTxSurfaceCount());
            lv_label_set_text(home_status_label_, text);
        } else if (current_page_ == kavio_demo::Page::Transfer) {
            UpdateTransferLabels();
        } else if (current_page_ == kavio_demo::Page::HdmiRx) {
            UpdateRxLabels();
        } else if (current_page_ == kavio_demo::Page::HdmiTx) {
            UpdateTxLabels();
        } else if (current_page_ == kavio_demo::Page::H264) {
            UpdateH264Labels();
        } else if (current_page_ == kavio_demo::Page::Schema) {
            UpdateSchemaLabels();
        } else if (current_page_ == kavio_demo::Page::Runtime) {
            UpdateRuntimeLabels();
        }
    }
    /** Advance the demonstration waveform animation. */
    void UpdateRenderAnimation()
    {
        if (render_paused_) return;
        ++render_frames_;
        const double phase = static_cast<double>(render_frames_) * 0.22;
        for (size_t i = 0; i < wave_bars_.size(); ++i) {
            if (!wave_bars_[i]) continue;
            const double wave = (std::sin(phase + static_cast<double>(i) * 0.48) + 1.0) * 0.5;
            const int32_t height = 24 + static_cast<int32_t>(wave * 110.0);
            lv_obj_set_size(wave_bars_[i], 18, height);
            lv_obj_set_pos(wave_bars_[i], 30 + static_cast<int32_t>(i) * 39, 232 - height);
        }
        if (render_frame_label_) {
            char text[64]{};
            std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(render_frames_));
            lv_label_set_text(render_frame_label_, text);
        }
        if (render_fps_label_) {
            char text[64]{};
            std::snprintf(text, sizeof(text), "Target %d FPS", render_fps_);
            lv_label_set_text(render_fps_label_, text);
        }
    }
    /** Present transport throughput and protocol statistics. */
    void UpdateTransferLabels()
    {
        if (transfer_state_label_) {
            char state[256]{};
            std::snprintf(state, sizeof(state),
                          "USB Host: %s\nPeer: %s\nChannel ID: %u",
                          YesNo(device_->IsUSBHostOnline()), YesNo(device_->IsCommChannelReady()),
                          device_->GetCommChannelId());
            lv_label_set_text(transfer_state_label_, state);
        }
        if (transfer_metric_label_) {
            uint64_t packets = 0;
            uint64_t bytes = 0;
            uint32_t checksum = 0;
            uint64_t errors = 0;
            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                packets = transfer_packets_;
                bytes = transfer_bytes_;
                checksum = last_transfer_checksum_;
                errors = protocol_errors_;
            }
            char metrics[256]{};
            std::snprintf(metrics, sizeof(metrics),
                          "Packets: %llu\nBytes: %llu\nLast checksum: 0x%08X\nProtocol errors: %llu",
                          static_cast<unsigned long long>(packets),
                          static_cast<unsigned long long>(bytes), checksum,
                          static_cast<unsigned long long>(errors));
            lv_label_set_text(transfer_metric_label_, metrics);
        }
        if (transfer_protocol_label_) {
            char text[384]{};
            std::snprintf(text, sizeof(text),
                          "Shared only by common/DemoProtocol.h.\n"
                          "Transport: one complete App Channel datagram per message.\n"
                          "Configured payload: %d KB; maximum SDK datagram: 1 MiB.",
                          transfer_payload_kb_);
            lv_label_set_text(transfer_protocol_label_, text);
        }
    }
    /** Convert RX frames using the public SDK Image implementation.
     * @param frame Owned source frame with a valid pixel format and stride.
     * @return True when the native LVGL image has received new RGB565 pixels.
     *
     * C++ deliberately uses the same conversion path, sizing and input
     * validation as the Python and Lua Image.set_buffer implementations.
     * The LVGL descriptor is owned by this demo; its data pointer remains
     * valid only until Image::SetBuffer or SetSource is called again.
     */
    bool BuildRxPreview(const kavio::ImageFrame& frame)
    {
        if (!rx_preview_canvas_ || frame.data.empty()) return false;
        if (!rx_preview_image_.SetBuffer(frame.data.data(), frame.data.size(),
                                         frame.width, frame.height, frame.stride,
                                         frame.pixel_format, kRxPreviewWidth,
                                         kRxPreviewHeight)) return false;
        const kavio::ui::ImageBufferView view = rx_preview_image_.BufferView();
        if (!view.data || !view.width || !view.height) return false;

        // Bind the current SDK-owned buffer after every conversion. Previous
        // views become invalid when SetBuffer replaces the vector storage.
        rx_preview_descriptor_ = {};
        rx_preview_descriptor_.header.magic = LV_IMAGE_HEADER_MAGIC;
        rx_preview_descriptor_.header.cf = LV_COLOR_FORMAT_RGB565;
        rx_preview_descriptor_.header.w = view.width;
        rx_preview_descriptor_.header.h = view.height;
        rx_preview_descriptor_.data_size = view.width * view.height * sizeof(uint16_t);
        rx_preview_descriptor_.data = reinterpret_cast<const uint8_t*>(view.data);
        lv_image_set_src(rx_preview_canvas_, &rx_preview_descriptor_);
        lv_obj_set_pos(rx_preview_canvas_,
                       18 + static_cast<int32_t>((kRxPreviewAreaWidth - view.width) / 2u),
                       46 + static_cast<int32_t>((kRxPreviewAreaHeight - view.height) / 2u));
        lv_obj_clear_flag(rx_preview_canvas_, LV_OBJ_FLAG_HIDDEN);
        if (rx_preview_placeholder_) lv_obj_add_flag(rx_preview_placeholder_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(rx_preview_canvas_);
        return true;
    }

    /** Present HDMI RX connection, frame, and preview status. */
    void UpdateRxLabels()
    {
        const kavio::DeviceRxInfo rx = device_->GetRxInfo();
        if (!rx_state_label_) return;
        char text[320]{};
        std::snprintf(text, sizeof(text),
                      "Connected: %s   Frame: %s\n%ux%u  stride=%u  %s\nLast read: %s",
                      YesNo(rx.connected), YesNo(rx.frame_available), rx.width, rx.height, rx.stride,
                      PixelFormatName(rx.pixel_format), last_rx_result_.c_str());
        lv_label_set_text(rx_state_label_, text);
    }

    size_t ActiveTxSurfaceCount() const
    {
        size_t count = 0;
        for (const auto& overlay : tx_overlays_) if (overlay.surface.IsValid()) ++count;
        return count;
    }

    void UpdateTxLabels()
    {
        if (!tx_state_label_) return;
        char text[420]{};
        size_t used = 0;
        int written = std::snprintf(text, sizeof(text), "Active: %zu / 3   Pending: %zu\n",
                                    ActiveTxSurfaceCount(), pending_tx_requests_.size());
        used = written > 0 ? static_cast<size_t>(written) : 0;
        for (size_t i = 0; i < tx_overlays_.size() && used + 64 < sizeof(text); ++i) {
            const auto& overlay = tx_overlays_[i];
            if (!overlay.surface.IsValid()) continue;
            const int n = overlay.partial
                ? std::snprintf(text + used, sizeof(text) - used,
                                "#%zu id=%u PARTIAL rect=(%d,%d,96,72)  ", i + 1, overlay.surface.id(),
                                overlay.partial_x, overlay.partial_y)
                : std::snprintf(text + used, sizeof(text) - used,
                                "#%zu id=%u pos=(%d,%d)  ", i + 1, overlay.surface.id(),
                                overlay.x, overlay.y);
            if (n > 0) used += static_cast<size_t>(n);
        }
        if (used + 48 < sizeof(text))
            std::snprintf(text + used, sizeof(text) - used, "\n%s", last_tx_result_.c_str());
        lv_label_set_text(tx_state_label_, text);
    }

    void UpdateRuntimeLabels()
    {
        if (runtime_summary_label_) {
            const kavio::DeviceRxInfo rx = device_->GetRxInfo();
            char text[512]{};
            std::snprintf(text, sizeof(text),
                          "Client ready: %s\nUSB Host online: %s\nApp Channel ready: %s\nChannel ID: %u\n"
                          "RX: %s  %ux%u %s\nConfig: render_fps=%d, payload=%d KB, metrics=%s",
                          YesNo(device_->IsReady()), YesNo(device_->IsUSBHostOnline()),
                          YesNo(device_->IsCommChannelReady()), device_->GetCommChannelId(),
                          rx.connected ? "connected" : "disconnected", rx.width, rx.height,
                          PixelFormatName(rx.pixel_format), render_fps_, transfer_payload_kb_, YesNo(show_metrics_));
            lv_label_set_text(runtime_summary_label_, text);
        }

    }
    // --- App Channel and transport operations ---
    /** Drain pending SDK events on the LVGL-owning thread. */
    void ProcessPendingWork()
    {
        std::deque<kavio_demo::Packet> packets;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            packets.swap(pending_packets_);
        }
        for (const auto& packet : packets) HandleDemoPacket(packet);
        if (h264_invalidated_.exchange(false)) HandleH264Invalidated();
    }
    /** Dispatch a validated App Channel packet. */
    void HandleDemoPacket(const kavio_demo::Packet& packet)
    {
        switch (packet.type) {
            /** Send serialized App Channel bytes through the SDK. */
            case kavio_demo::MessageType::Hello:
                SendDemoPacket(kavio_demo::MakeTextPacket(kavio_demo::MessageType::HelloAck,
                                                          packet.sequence, "KavioDemo C++ Device"));
                break;
            case kavio_demo::MessageType::Ping:
                SendDemoPacket(kavio_demo::MakePacket(kavio_demo::MessageType::Pong,
                                                      packet.sequence, packet.payload));
                break;
            case kavio_demo::MessageType::Navigate: {
                kavio_demo::Page page;
                if (kavio_demo::DecodePage(packet.payload, page)) ShowPage(page);
                /** Account for an invalid protocol datagram. */
                else RecordProtocolError();
                break;
            }
            case kavio_demo::MessageType::TransferData: {
                // ParsePacket() has already validated the payload checksum. Reuse it here
                // instead of hashing a potentially 1 MiB benchmark payload a second time.
                const uint32_t checksum = packet.payload_checksum;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    ++transfer_packets_;
                    transfer_bytes_ += packet.payload.size();
                    last_transfer_checksum_ = checksum;
                }
                SendDemoPacket(kavio_demo::MakePacket(kavio_demo::MessageType::TransferAck,
                                                      packet.sequence,
                                                      kavio_demo::EncodeTransferAck(
                                                          static_cast<uint32_t>(packet.payload.size()), checksum)));
                break;
            }
            /** Send the current state to Windows Host. */
            case kavio_demo::MessageType::QueryState:
                SendStateReport(packet.sequence);
                break;
            default:
                break;
        }
    }
    /** Send the demo Hello once when the channel becomes ready. */
    void UpdateChannelHandshake()
    {
        const bool ready = device_->IsCommChannelReady();
        if (ready && !channel_was_ready_) {
            ++outgoing_sequence_;
            SendDemoPacket(kavio_demo::MakeTextPacket(kavio_demo::MessageType::Hello,
                                                      outgoing_sequence_, "KavioDemo C++ Device"));
            SendStateReport();
        }
        channel_was_ready_ = ready;
    }

    void SendStateReport(uint32_t sequence = 0)
    {
        if (!device_->IsCommChannelReady()) return;
        if (sequence == 0) sequence = ++outgoing_sequence_;
        const kavio::DeviceRxInfo rx = device_->GetRxInfo();
        char text[512]{};
        std::snprintf(text, sizeof(text),
                      "page=%u;client_ready=%u;host_online=%u;channel_ready=%u;channel_id=%u;"
                      "rx_connected=%u;rx_width=%u;rx_height=%u;tx_surfaces=%zu;h264_capture=%u",
                      static_cast<unsigned>(current_page_), device_->IsReady() ? 1u : 0u,
                      device_->IsUSBHostOnline() ? 1u : 0u, device_->IsCommChannelReady() ? 1u : 0u,
                      device_->GetCommChannelId(), rx.connected ? 1u : 0u, rx.width, rx.height,
                      ActiveTxSurfaceCount(), h264_capture_running_.load() ? 1u : 0u);
        SendDemoPacket(kavio_demo::MakeTextPacket(kavio_demo::MessageType::StateReport,
                                                  sequence, text));
    }

    bool SendDemoPacket(const std::vector<uint8_t>& bytes)
    {
        if (bytes.empty() || !device_->IsCommChannelReady()) return false;
        const kavio::Status status = device_->SendCommChannelData(bytes);
        if (!status.ok()) {
            std::cerr << "Demo channel send failed: " << status.message << '\n';
            return false;
        }
        return true;
    }

    void RecordProtocolError()
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        ++protocol_errors_;
    }
    // --- TX surface lifecycle and animation ---
    /** Request asynchronous creation of a TX overlay surface. */
    void RequestTxOverlay(bool partial)
    {
        if (ActiveTxSurfaceCount() + pending_tx_requests_.size() >= tx_overlays_.size()) {
            last_tx_result_ = "Maximum of 3 TX surfaces reached";
            UpdateTxLabels();
            return;
        }
        size_t slot = tx_overlays_.size();
        for (size_t i = 0; i < tx_overlays_.size(); ++i) {
            if (tx_overlays_[i].surface.IsValid()) continue;
            bool pending = false;
            for (const auto& item : pending_tx_requests_) if (item.second.slot == i) { pending = true; break; }
            if (!pending) { slot = i; break; }
        }
        if (slot >= tx_overlays_.size()) return;

        uint64_t request_id = 0;
        const uint32_t width = partial ? 320u : 200u;
        const uint32_t height = partial ? 200u : 120u;
        const kavio::Status status = device_->CreateTxSurface(width, height, request_id);
        if (status.ok()) {
            pending_tx_requests_[request_id] = PendingTxRequest{slot, partial};
            last_tx_result_ = partial ? "Partial surface create submitted" : "Create request submitted";
        } else {
            last_tx_result_ = status.message;
        }
        UpdateTxLabels();
    }
    /** Initialize an acquired TX surface with demo pixels. */
    void OpenAndFillTxSurface(uint64_t request_id,
                              const kavio::Status& open_status,
                              kavio::DeviceTxSurface surface)
    {
        const auto it = pending_tx_requests_.find(request_id);
        if (it == pending_tx_requests_.end()) {
            if (surface.IsValid() && device_) {
                uint64_t ignored = 0;
                (void)device_->DestroyTxSurface(surface.id(), ignored);
            }
            last_tx_result_ = "Unexpected TX create response was cleaned up";
            return;
        }
        const size_t slot = it->second.slot;
        const bool partial = it->second.partial;
        pending_tx_requests_.erase(it);

        if (!open_status.ok() || !surface.IsValid()) {
            last_tx_result_ = "OpenTxSurface failed: " + open_status.message;
            UpdateTxLabels();
            return;
        }

        auto& overlay = tx_overlays_[slot];
        overlay.surface = std::move(surface);
        overlay.partial = partial;
        overlay.x = partial ? 80 + static_cast<int32_t>(slot) * 70 : 40 + static_cast<int32_t>(slot) * 90;
        overlay.y = partial ? 80 + static_cast<int32_t>(slot) * 50 : 40 + static_cast<int32_t>(slot) * 65;
        overlay.dx = 4 + static_cast<int32_t>(slot);
        overlay.dy = 3 + static_cast<int32_t>(slot);
        overlay.partial_x = 0;
        overlay.partial_y = 0;
        overlay.partial_dx = 5 + static_cast<int32_t>(slot);
        overlay.partial_dy = 4 + static_cast<int32_t>(slot);
        FillTxOverlay(slot);
        last_tx_result_ = partial ? "Partial TX surface created" : "TX surface created";
        UpdateTxLabels();
    }
    /** Render content into one TX overlay buffer. */
    void FillTxOverlay(size_t slot)
    {
        if (slot >= tx_overlays_.size()) return;
        auto& overlay = tx_overlays_[slot];
        if (!overlay.surface.IsValid()) return;

        auto& frame = overlay.frame;
        if (frame.data.empty()) {
            frame.width = overlay.surface.width();
            frame.height = overlay.surface.height();
            frame.stride = overlay.surface.stride();
            frame.pixel_format = kavio::PixelFormat::kBGRA8888;
            frame.valid_rect = kavio::Rect{0, 0, frame.width, frame.height};
            frame.data.resize(static_cast<size_t>(frame.height) * frame.stride, 0);

            const uint8_t base_r[3] = {235u, 70u, 85u};
            const uint8_t base_g[3] = {95u, 210u, 125u};
            const uint8_t base_b[3] = {75u, 105u, 235u};
            for (uint32_t y = 0; y < frame.height; ++y) {
                for (uint32_t x = 0; x < frame.width; ++x) {
                    const size_t offset = static_cast<size_t>(y) * frame.stride + static_cast<size_t>(x) * 4u;
                    frame.data[offset + 0] = static_cast<uint8_t>(std::min(255u, static_cast<unsigned>(base_b[slot]) + x * 40u / std::max(1u, frame.width)));
                    frame.data[offset + 1] = static_cast<uint8_t>(std::min(255u, static_cast<unsigned>(base_g[slot]) + y * 35u / std::max(1u, frame.height)));
                    frame.data[offset + 2] = base_r[slot];
                    frame.data[offset + 3] = 150u;
                }
            }
        }

        if (overlay.partial) {
            const uint32_t rect_w = std::min<uint32_t>(96u, frame.width);
            const uint32_t rect_h = std::min<uint32_t>(72u, frame.height);
            frame.valid_rect = kavio::Rect{overlay.partial_x,
                                             overlay.partial_y, rect_w, rect_h};
        } else {
            frame.valid_rect = kavio::Rect{0, 0, frame.width, frame.height};
        }
        frame.frame_id = ++tx_frame_id_;
        frame.timestamp_ns = SteadyNowNs();
        frame.dest_pos_x = overlay.x;
        frame.dest_pos_y = overlay.y;
        const kavio::Status status = overlay.surface.WriteFrame(frame);
        if (!status.ok()) last_tx_result_ = status.message;
    }
    /** Advance active TX overlay positions using bounded steps. */
    void UpdateTxOverlaysMotion()
    {
        const auto runtime = device_->GetRuntimeData();
        uint32_t output_width = 720;
        uint32_t output_height = 720;
        if (runtime.tx.width > 0 && runtime.tx.height > 0) {
            output_width = runtime.tx.width;
            output_height = runtime.tx.height;
        }
        for (size_t i = 0; i < tx_overlays_.size(); ++i) {
            auto& overlay = tx_overlays_[i];
            if (!overlay.surface.IsValid()) continue;
            const int32_t max_x = std::max<int32_t>(0, static_cast<int32_t>(output_width) - static_cast<int32_t>(overlay.surface.width()));
            const int32_t max_y = std::max<int32_t>(0, static_cast<int32_t>(output_height) - static_cast<int32_t>(overlay.surface.height()));
            if (overlay.partial) {
                const int32_t rect_w = std::min<int32_t>(96, static_cast<int32_t>(overlay.surface.width()));
                const int32_t rect_h = std::min<int32_t>(72, static_cast<int32_t>(overlay.surface.height()));
                const int32_t max_partial_x = std::max<int32_t>(0, static_cast<int32_t>(overlay.surface.width()) - rect_w);
                const int32_t max_partial_y = std::max<int32_t>(0, static_cast<int32_t>(overlay.surface.height()) - rect_h);
                overlay.partial_x += overlay.partial_dx;
                overlay.partial_y += overlay.partial_dy;
                if (overlay.partial_x < 0) { overlay.partial_x = 0; overlay.partial_dx = std::abs(overlay.partial_dx); }
                if (overlay.partial_x > max_partial_x) { overlay.partial_x = max_partial_x; overlay.partial_dx = -std::abs(overlay.partial_dx); }
                if (overlay.partial_y < 0) { overlay.partial_y = 0; overlay.partial_dy = std::abs(overlay.partial_dy); }
                if (overlay.partial_y > max_partial_y) { overlay.partial_y = max_partial_y; overlay.partial_dy = -std::abs(overlay.partial_dy); }
            } else {
                overlay.x += overlay.dx;
                overlay.y += overlay.dy;
                if (overlay.x < 0) { overlay.x = 0; overlay.dx = std::abs(overlay.dx); }
                if (overlay.x > max_x) { overlay.x = max_x; overlay.dx = -std::abs(overlay.dx); }
                if (overlay.y < 0) { overlay.y = 0; overlay.dy = std::abs(overlay.dy); }
                if (overlay.y > max_y) { overlay.y = max_y; overlay.dy = -std::abs(overlay.dy); }
            }
            FillTxOverlay(i);
        }
        if (current_page_ == kavio_demo::Page::HdmiTx) UpdateTxLabels();
    }
    /** Destroy the demo TX surfaces and clear their state. */
    void ClearTxOverlays()
    {
        for (auto& overlay : tx_overlays_) {
            if (!overlay.surface.IsValid()) continue;
            uint64_t request_id = 0;
            const kavio::Status status = overlay.surface.Destroy(request_id);
            if (status.ok()) {
                overlay.surface = kavio::DeviceTxSurface();
                overlay.frame = kavio::ImageFrame();
            } else {
                last_tx_result_ = status.message;
            }
        }
        pending_tx_requests_.clear();
        last_tx_result_ = "TX surfaces cleared";
        UpdateTxLabels();
    }
    // --- Virtual keyboard and mouse input ---
    /** Send a virtual keyboard event to the host. */
    bool SendKeyboardEvent(kavio::KeyCode key,
                           kavio::KeyOption option)
    {
        const kavio::Status status = device_->SendKeyboard(
            option, key, SteadyNowNs());
        if (!status.ok()) {
            last_input_result_ = "Keyboard send failed: " + status.message;
            if (keyboard_state_label_) lv_label_set_text(keyboard_state_label_, last_input_result_.c_str());
            if (mouse_state_label_) lv_label_set_text(mouse_state_label_, last_input_result_.c_str());
            return false;
        }
        return true;
    }
    /** Send a virtual key press and release pair. */
    bool TapKey(kavio::KeyCode key)
    {
        return SendKeyboardEvent(key, kavio::KeyOption::kDown) &&
               SendKeyboardEvent(key, kavio::KeyOption::kUp);
    }
    /** Submit a virtual pointer or mouse-button event. */
    bool SendMouseEvent(kavio::MouseOption option,
                        int32_t x = 0,
                        int32_t y = 0,
                        int32_t wheel = 0)
    {
        const kavio::Status status = device_->SendMouse(
            option, x, y, wheel, SteadyNowNs());
        if (!status.ok()) {
            last_input_result_ = "Mouse send failed: " + status.message;
            if (keyboard_state_label_) lv_label_set_text(keyboard_state_label_, last_input_result_.c_str());
            if (mouse_state_label_) lv_label_set_text(mouse_state_label_, last_input_result_.c_str());
            return false;
        }
        return true;
    }
    /** Update the input-page feedback after successful submission. */
    void SetInputSuccess(const std::string& text)
    {
        last_input_result_ = text;
        if (keyboard_state_label_) lv_label_set_text(keyboard_state_label_, last_input_result_.c_str());
        if (mouse_state_label_) lv_label_set_text(mouse_state_label_, last_input_result_.c_str());
    }

    std::string EnsureAppCacheDir()
    {
        const char* data_dir = std::getenv("KAVIO_APP_DATA_DIR");
        if (!data_dir || !*data_dir) return {};
        const std::string cache_dir = std::string(data_dir) + "/cache";
        if (::mkdir(cache_dir.c_str(), 0755) != 0 && errno != EEXIST) return {};
        return cache_dir;
    }
    // --- H.264 capture lifecycle ---
    /** Start capture; the stream becomes ready asynchronously. */
    void StartH264Capture()
    {
        uint32_t existing_stream = 0;
        { std::lock_guard<std::mutex> lock(h264_state_mutex_); existing_stream = h264_stream_id_; }
        if (h264_capture_running_.load() || h264_subscribe_request_id_.load() != 0 || existing_stream != 0) {
            {
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "Capture is already active or subscribing";
            }
            UpdateH264Labels();
            return;
        }
        const std::string cache_dir = EnsureAppCacheDir();
        if (cache_dir.empty()) {
            {
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "KAVIO_APP_DATA_DIR/cache is unavailable";
            }
            UpdateH264Labels();
            return;
        }
        uint64_t request_id = 0;
        const kavio::Status status = device_->SubscribeH264Stream(request_id);
        if (!status.ok()) {
            std::lock_guard<std::mutex> lock(h264_state_mutex_);
            h264_last_result_ = "Subscribe failed: " + status.message;
        } else {
            h264_subscribe_request_id_ = request_id;
            std::lock_guard<std::mutex> lock(h264_state_mutex_);
            h264_last_result_ = "Subscribe request submitted";
        }
        UpdateH264Labels();
    }
    /** Handle the completed H.264 subscription request. */
    void OpenH264Capture(uint64_t request_id,
                         const kavio::Status& open_status,
                         const kavio::DeviceH264StreamInfo& info)
    {
        if (request_id != h264_subscribe_request_id_.load()) {
            if (open_status.ok() && info.stream_id != 0 && device_) {
                uint64_t ignored = 0;
                (void)device_->UnsubscribeH264Stream(info.stream_id, ignored);
            }
            return;
        }
        h264_subscribe_request_id_ = 0;
        if (!open_status.ok()) {
            {
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "OpenH264Stream failed: " + open_status.message;
            }
            UpdateH264Labels();
            return;
        }

        const std::string cache_dir = EnsureAppCacheDir();
        if (cache_dir.empty()) {
            {
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "Cache directory unavailable after subscribe";
            }
            uint64_t ignored = 0;
            (void)device_->UnsubscribeH264Stream(info.stream_id, ignored);
            UpdateH264Labels();
            return;
        }

        JoinH264Worker();
        {
            std::lock_guard<std::mutex> lock(h264_state_mutex_);
            h264_stream_id_ = info.stream_id;
            h264_width_ = info.width;
            h264_height_ = info.height;
            h264_fps_ = info.fps;
            h264_bitrate_kbps_ = info.bitrate_kbps;
            h264_packets_ = 0;
            h264_bytes_ = 0;
            h264_dropped_reads_ = 0;
            h264_capture_path_ = cache_dir + "/h264_capture_" + std::to_string(SteadyNowNs()) + ".h264";
            h264_last_result_ = "Capture started";
        }
        h264_capture_running_.store(true);
        const uint32_t stream_id = info.stream_id;
        const std::string path = h264_capture_path_;
        h264_worker_ = std::thread([this, stream_id, path]() { H264CaptureLoop(stream_id, path); });
        UpdateH264Labels();
    }
    /** Read encoded packets on a dedicated capture worker. */
    void H264CaptureLoop(uint32_t stream_id, const std::string& path)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            std::lock_guard<std::mutex> lock(h264_state_mutex_);
            h264_last_result_ = "Unable to open capture file";
            h264_capture_running_.store(false);
            return;
        }

        uint64_t next_seq = 0;
        auto next_flush = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (h264_capture_running_.load()) {
            const uint64_t latest = device_->GetLatestH264PacketSeq(stream_id);
            if (latest == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            if (next_seq == 0) next_seq = latest;
            uint32_t processed = 0;
            while (next_seq <= latest && processed < 64 && h264_capture_running_.load()) {
                kavio::DeviceH264Packet packet;
                const kavio::Status status = device_->ReadH264Packet(stream_id, next_seq, packet);
                if (status.ok()) {
                    if (!packet.data.empty()) {
                        output.write(reinterpret_cast<const char*>(packet.data.data()),
                                     static_cast<std::streamsize>(packet.data.size()));
                        if (!output.good()) {
                            std::lock_guard<std::mutex> lock(h264_state_mutex_);
                            h264_last_result_ = "Capture file write failed";
                            h264_capture_running_.store(false);
                            break;
                        }
                        std::lock_guard<std::mutex> lock(h264_state_mutex_);
                        ++h264_packets_;
                        h264_bytes_ += packet.data.size();
                    }
                    ++next_seq;
                } else if (status.code == kavio::StatusCode::Media_NoFrame) {
                    std::lock_guard<std::mutex> lock(h264_state_mutex_);
                    ++h264_dropped_reads_;
                    next_seq = latest + 1;
                    break;
                } else {
                    std::lock_guard<std::mutex> lock(h264_state_mutex_);
                    h264_last_result_ = "Read failed: " + status.message;
                    h264_capture_running_.store(false);
                    break;
                }
                ++processed;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_flush) {
                output.flush();
                next_flush = now + std::chrono::milliseconds(500);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        output.flush();
        output.close();
    }
    /** Wait for a running capture worker to exit. */
    void JoinH264Worker()
    {
        h264_capture_running_.store(false);
        if (h264_worker_.joinable()) h264_worker_.join();
    }
    /** Respond to an invalidated H.264 stream. */
    void HandleH264Invalidated()
    {
        JoinH264Worker();
        std::lock_guard<std::mutex> lock(h264_state_mutex_);
        h264_stream_id_ = 0;
    }
    /** Stop the stream and release worker resources. */
    void StopH264Capture(bool unsubscribe)
    {
        h264_subscribe_request_id_ = 0;
        JoinH264Worker();
        uint32_t stream_id = 0;
        {
            std::lock_guard<std::mutex> lock(h264_state_mutex_);
            stream_id = h264_stream_id_;
            h264_stream_id_ = 0;
            if (h264_last_result_.find("failed") == std::string::npos &&
                h264_last_result_.find("invalidated") == std::string::npos)
                h264_last_result_ = "Capture stopped";
        }
        if (unsubscribe && stream_id != 0 && device_->IsReady()) {
            uint64_t request_id = 0;
            const kavio::Status status = device_->UnsubscribeH264Stream(stream_id, request_id);
            if (!status.ok()) {
                std::lock_guard<std::mutex> lock(h264_state_mutex_);
                h264_last_result_ = "Unsubscribe failed: " + status.message;
            }
        }
        UpdateH264Labels();
    }
    /** Refresh capture and transport metrics. */
    void UpdateH264Labels()
    {
        if (!h264_state_label_ && !h264_metric_label_) return;
        std::string state;
        std::string path;
        uint64_t packets = 0, bytes = 0, dropped = 0;
        uint32_t stream_id = 0, width = 0, height = 0, fps = 0, bitrate = 0;
        {
            std::lock_guard<std::mutex> lock(h264_state_mutex_);
            state = h264_last_result_;
            path = h264_capture_path_;
            packets = h264_packets_;
            bytes = h264_bytes_;
            dropped = h264_dropped_reads_;
            stream_id = h264_stream_id_;
            width = h264_width_;
            height = h264_height_;
            fps = h264_fps_;
            bitrate = h264_bitrate_kbps_;
        }
        if (h264_state_label_) {
            char text[256]{};
            std::snprintf(text, sizeof(text), "%s\nStream ID: %u   Running: %s",
                          state.c_str(), stream_id, YesNo(h264_capture_running_.load()));
            lv_label_set_text(h264_state_label_, text);
        }
        if (h264_metric_label_) {
            char text[640]{};
            std::snprintf(text, sizeof(text),
                          "Format: H.264 Annex-B   %ux%u @ %u fps   bitrate=%u kbps\n"
                          "Packets: %llu   Bytes: %llu   Overwritten/NoFrame reads: %llu\n"
                          "File: %s",
                          width, height, fps, bitrate,
                          static_cast<unsigned long long>(packets),
                          static_cast<unsigned long long>(bytes),
                          static_cast<unsigned long long>(dropped),
                          path.empty() ? "not created" : path.c_str());
            lv_label_set_text(h264_metric_label_, text);
        }
    }
    // --- Application configuration ---
    /** Apply and report a JSON configuration update. */
    void CommitSchemaUpdate(const Json::Value& config, const char* success_text)
    {
        const kavio::Status status = device_->UpdateAppConfigJson(EncodeConfigJson(config));
        schema_last_result_ = status.ok() ? success_text : ("Update failed: " + status.message);
        if (status.ok()) ApplyConfig(false);
        UpdateSchemaLabels();
    }
    /** Refresh displayed application configuration values. */
    void UpdateSchemaLabels()
    {
        if (!schema_value_label_ && !schema_state_label_) return;
        const Json::Value config = ParseConfigJson(device_->GetAppConfigJson());
        if (schema_value_label_) {
            char text[512]{};
            const std::string default_page = config.get("default_page", "home").asString();
            std::snprintf(text, sizeof(text),
                          "default_page       %s\nrender_fps         %d\nshow_metrics       %s\ntransfer_payload   %d KB",
                          default_page.c_str(),
                          config.get("render_fps", 20).asInt(),
                          YesNo(config.get("show_metrics", true).asBool()),
                          config.get("transfer_payload_kb", 64).asInt());
            lv_label_set_text(schema_value_label_, text);
        }
        if (schema_state_label_) lv_label_set_text(schema_state_label_, schema_last_result_.c_str());
    }
    /** Recover the demo instance from LVGL event user_data. */
    static KavioDemo* Self(lv_event_t* event)
    {
        return static_cast<KavioDemo*>(lv_event_get_user_data(event));
    }

    static void HomeNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Home); }
    static void RenderNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Render); }
    static void TransferNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Transfer); }
    static void HdmiRxNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::HdmiRx); }
    static void HdmiTxNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::HdmiTx); }
    static void KeyboardNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Keyboard); }
    static void MouseNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Mouse); }
    static void H264Nav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::H264); }
    static void SchemaNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Schema); }
    static void RuntimeNav(lv_event_t* e) { if (auto* s = Self(e)) s->ShowPage(kavio_demo::Page::Runtime); }

    static void ToggleRender(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            s->render_paused_ = !s->render_paused_;
            if (s->render_pause_label_)
                lv_label_set_text(s->render_pause_label_, s->render_paused_ ? "Paused" : "Running");
        }
    }

    /**
     * Read and present one RX sample from the device.
     * @param automatic True for continuous preview; suppress expected
     *                  no-signal errors and skip duplicate frame IDs.
     */
    void ReadRxSample(bool automatic)
    {
        if (!device_ || !rx_preview_canvas_) return;
        kavio::ImageFrame frame;
        const kavio::Status status = device_->ReadLatestRxFrame(frame);
        if (!status.ok()) {
            if (!automatic) {
                last_rx_result_ = status.message;
                UpdateRxLabels();
            }
            return;
        }
        if (automatic && frame.frame_id != 0 && frame.frame_id == last_rx_frame_id_)
            return;
        const bool preview_ok = BuildRxPreview(frame);
        if (preview_ok) {
            last_rx_frame_id_ = frame.frame_id;
            if (automatic) ++rx_fps_frames_;
        }
        char text[192]{};
        std::snprintf(text, sizeof(text), "OK frame=%llu bytes=%zu preview=%s",
                      static_cast<unsigned long long>(frame.frame_id), frame.data.size(),
                      preview_ok ? "shown" : "unsupported");
        last_rx_result_ = text;
        if (!automatic) UpdateRxLabels();
    }

    /** Report the number of distinct, successfully displayed frames per second. */
    void UpdateRxFps(std::chrono::steady_clock::time_point now)
    {
        if (rx_fps_window_start_ == std::chrono::steady_clock::time_point{})
            rx_fps_window_start_ = now;
        const double elapsed = std::chrono::duration<double>(now - rx_fps_window_start_).count();
        if (elapsed < 1.0) return;
        rx_fps_ = static_cast<double>(rx_fps_frames_) / elapsed;
        rx_fps_frames_ = 0;
        rx_fps_window_start_ = now;
        if (rx_fps_label_) {
            char fps_text[48]{};
            std::snprintf(fps_text, sizeof(fps_text), "FPS: %.1f", rx_fps_);
            lv_label_set_text(rx_fps_label_, fps_text);
        }
    }

    /** Read one frame on explicit user request. */
    static void ReadRxFrame(lv_event_t* e)
    {
        if (auto* s = Self(e)) s->ReadRxSample(false);
    }

    /** Toggle continuous RX preview; each UI loop tries the latest frame. */
    static void ToggleRxLive(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            s->rx_live_ = !s->rx_live_;
            s->rx_fps_frames_ = 0;
            s->rx_fps_ = 0.0;
            s->rx_fps_window_start_ = std::chrono::steady_clock::now();
            if (s->rx_fps_label_) lv_label_set_text(s->rx_fps_label_, "FPS: 0.0");
            lv_obj_t* label = lv_obj_get_child(lv_event_get_target_obj(e), 0);
            if (label) lv_label_set_text(label, s->rx_live_ ? "STOP LIVE" : "START LIVE");
        }
    }

    static void CreateOverlay(lv_event_t* e) { if (auto* s = Self(e)) s->RequestTxOverlay(false); }
    static void CreatePartialOverlay(lv_event_t* e) { if (auto* s = Self(e)) s->RequestTxOverlay(true); }
    static void ClearOverlays(lv_event_t* e) { if (auto* s = Self(e)) s->ClearTxOverlays(); }







    static void KeyboardKeyPressed(lv_event_t* e)
    {
        auto* binding = static_cast<KeyBinding*>(lv_event_get_user_data(e));
        if (!binding || !binding->owner) return;
        if (binding->owner->TapKey(binding->key))
            binding->owner->SetInputSuccess(binding->label + ": submitted");
    }
    /** Convert touch-pad input into a device mouse event. */
    static void MousePadEvent(lv_event_t* e)
    {
        auto* s = Self(e);
        if (!s || !s->mouse_pad_) return;
        lv_indev_t* indev = lv_indev_active();
        if (!indev) return;
        lv_point_t point{};
        lv_indev_get_point(indev, &point);
        lv_area_t area{};
        lv_obj_get_coords(s->mouse_pad_, &area);
        const int32_t width = std::max<int32_t>(1, area.x2 - area.x1 + 1);
        const int32_t height = std::max<int32_t>(1, area.y2 - area.y1 + 1);
        const int32_t local_x = std::max<int32_t>(0, std::min<int32_t>(width - 1, point.x - area.x1));
        const int32_t local_y = std::max<int32_t>(0, std::min<int32_t>(height - 1, point.y - area.y1));
        const int32_t mouse_x = width <= 1 ? 0 : static_cast<int32_t>((static_cast<int64_t>(local_x) * 65535) / (width - 1));
        const int32_t mouse_y = height <= 1 ? 0 : static_cast<int32_t>((static_cast<int64_t>(local_y) * 65535) / (height - 1));
        if (s->SendMouseEvent(kavio::MouseOption::kMove, mouse_x, mouse_y)) {
            if (s->mouse_cursor_dot_) {
                lv_obj_set_pos(s->mouse_cursor_dot_, std::max<int32_t>(0, local_x - 9),
                               std::max<int32_t>(0, local_y - 9));
            }
            char text[128]{};
            std::snprintf(text, sizeof(text), "Mouse position: %d, %d", mouse_x, mouse_y);
            s->SetInputSuccess(text);
        }
    }

    static void MouseLeftClick(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            const bool down = s->SendMouseEvent(kavio::MouseOption::kLeftDown);
            const bool up = s->SendMouseEvent(kavio::MouseOption::kLeftUp);
            if (down && up) s->SetInputSuccess("Left click: submitted");
        }
    }

    static void MouseRightClick(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            const bool down = s->SendMouseEvent(kavio::MouseOption::kRightDown);
            const bool up = s->SendMouseEvent(kavio::MouseOption::kRightUp);
            if (down && up) s->SetInputSuccess("Right click: submitted");
        }
    }

    static void MouseWheelUp(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            if (s->SendMouseEvent(kavio::MouseOption::kWheel, 0, 0, 120))
                s->SetInputSuccess("Wheel +120: submitted");
        }
    }

    static void MouseWheelDown(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            if (s->SendMouseEvent(kavio::MouseOption::kWheel, 0, 0, -120))
                s->SetInputSuccess("Wheel -120: submitted");
        }
    }

    static void StartH264(lv_event_t* e) { if (auto* s = Self(e)) s->StartH264Capture(); }
    static void StopH264(lv_event_t* e) { if (auto* s = Self(e)) s->StopH264Capture(true); }

    static void SchemaFpsPlus(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            Json::Value config = ParseConfigJson(s->device_->GetAppConfigJson());
            int fps = config.get("render_fps", 20).asInt();
            config["render_fps"] = fps >= 60 ? 5 : std::min(60, fps + 5);
            s->CommitSchemaUpdate(config, "render_fps updated");
        }
    }

    static void SchemaToggleMetrics(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            Json::Value config = ParseConfigJson(s->device_->GetAppConfigJson());
            config["show_metrics"] = !config.get("show_metrics", true).asBool();
            s->CommitSchemaUpdate(config, "show_metrics toggled");
        }
    }

    static void SchemaPayloadDouble(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            Json::Value config = ParseConfigJson(s->device_->GetAppConfigJson());
            int kb = config.get("transfer_payload_kb", 64).asInt();
            config["transfer_payload_kb"] = kb >= 512 ? 1 : std::min(1023, kb * 2);
            s->CommitSchemaUpdate(config, "transfer_payload_kb updated");
        }
    }

    static void SchemaReset(lv_event_t* e)
    {
        if (auto* s = Self(e)) {
            const kavio::Status status = s->device_->ResetAppConfigToDefaults();
            s->schema_last_result_ = status.ok() ? "Defaults restored" : ("Reset failed: " + status.message);
            if (status.ok()) s->ApplyConfig(false);
            s->UpdateSchemaLabels();
        }
    }


    // SDK-owned device reference: valid between on_ready and on_stopping.
    kavio::Device* device_ = nullptr;
    kavio::ui::LvglBackend view_;
    std::atomic<bool> startup_failed_{false};
    std::atomic<bool> config_dirty_{false};

    // Cross-thread App Channel queue; LVGL is updated only on the UI tick.
    std::mutex pending_mutex_;
    std::deque<kavio_demo::Packet> pending_packets_;
    uint64_t protocol_errors_ = 0;
    uint64_t transfer_packets_ = 0;
    uint64_t transfer_bytes_ = 0;
    uint32_t last_transfer_checksum_ = 0;

    kavio_demo::Page current_page_ = kavio_demo::Page::Home;
    // Configuration and tick deadlines (monotonic time, milliseconds).
    int render_fps_ = 20;
    int transfer_payload_kb_ = 64;
    bool show_metrics_ = true;
    bool channel_was_ready_ = false;
    uint32_t outgoing_sequence_ = 0;
    std::chrono::steady_clock::time_point next_ui_update_{};
    std::chrono::steady_clock::time_point next_config_check_{};
    std::chrono::steady_clock::time_point next_render_frame_{};
    std::chrono::steady_clock::time_point next_hello_check_{};
    std::chrono::steady_clock::time_point next_tx_move_{};

    // TX overlay resources and motion velocities (screen pixels per update).
    struct TxOverlayState {
        kavio::DeviceTxSurface surface;
        int32_t x = 0;
        int32_t y = 0;
        int32_t dx = 0;
        int32_t dy = 0;
        bool partial = false;
        int32_t partial_x = 0;
        int32_t partial_y = 0;
        int32_t partial_dx = 0;
        int32_t partial_dy = 0;
        kavio::ImageFrame frame;
    };
    struct PendingTxRequest { size_t slot = 0; bool partial = false; };
    std::array<TxOverlayState, 3> tx_overlays_{};
    std::unordered_map<uint64_t, PendingTxRequest> pending_tx_requests_;
    uint64_t tx_frame_id_ = 0;
    std::string last_rx_result_ = "Not requested";
    std::string last_tx_result_ = "Not requested";
    std::string last_input_result_ = "No input event sent yet";
    // SDK Image owns converted RGB565 pixels; LVGL only borrows the view.
    kavio::ui::Image rx_preview_image_;
    lv_image_dsc_t rx_preview_descriptor_{};
    bool rx_live_ = false;
    uint64_t last_rx_frame_id_ = 0;
    uint64_t rx_fps_frames_ = 0;  // Distinct frames presented in the current window.
    double rx_fps_ = 0.0;
    std::chrono::steady_clock::time_point rx_fps_window_start_{};

    // H.264 capture worker state; synchronized independently from UI state.
    std::mutex h264_state_mutex_;
    std::thread h264_worker_;
    std::atomic<bool> h264_capture_running_{false};
    std::atomic<bool> h264_invalidated_{false};
    std::atomic<uint64_t> h264_subscribe_request_id_{0};
    uint32_t h264_stream_id_ = 0;
    uint32_t h264_width_ = 0;
    uint32_t h264_height_ = 0;
    uint32_t h264_fps_ = 0;
    uint32_t h264_bitrate_kbps_ = 0;
    uint64_t h264_packets_ = 0;
    uint64_t h264_bytes_ = 0;
    uint64_t h264_dropped_reads_ = 0;
    std::string h264_capture_path_;
    std::string h264_last_result_ = "Idle";
    std::string schema_last_result_ = "Ready";

    // LVGL objects are borrowed handles owned by the active screen; the
    // pointers must be cleared before page replacement destroys the widgets.
    lv_obj_t* page_title_ = nullptr;
    lv_obj_t* home_status_label_ = nullptr;
    lv_obj_t* home_scroll_ = nullptr;
    lv_obj_t* render_frame_label_ = nullptr;
    lv_obj_t* render_fps_label_ = nullptr;
    lv_obj_t* render_pause_label_ = nullptr;
    lv_obj_t* render_metrics_card_ = nullptr;
    lv_obj_t* transfer_state_label_ = nullptr;
    lv_obj_t* transfer_metric_label_ = nullptr;
    lv_obj_t* transfer_protocol_label_ = nullptr;
    lv_obj_t* rx_state_label_ = nullptr;
    lv_obj_t* rx_fps_label_ = nullptr;
    lv_obj_t* rx_preview_canvas_ = nullptr;
    lv_obj_t* rx_preview_placeholder_ = nullptr;
    lv_obj_t* tx_state_label_ = nullptr;
    lv_obj_t* keyboard_state_label_ = nullptr;
    lv_obj_t* mouse_state_label_ = nullptr;
    lv_obj_t* mouse_pad_ = nullptr;
    lv_obj_t* mouse_cursor_dot_ = nullptr;
    std::deque<KeyBinding> keyboard_bindings_;
    lv_obj_t* h264_state_label_ = nullptr;
    lv_obj_t* h264_metric_label_ = nullptr;
    lv_obj_t* schema_state_label_ = nullptr;
    lv_obj_t* schema_value_label_ = nullptr;
    lv_obj_t* runtime_summary_label_ = nullptr;
    std::array<lv_obj_t*, 16> wave_bars_{};
    // Render-page animation counters, valid independently of the RX preview.
    bool render_paused_ = false;
    uint64_t render_frames_ = 0;
};

} // namespace

int main()
{
    KavioDemo app;
    return app.Run();
}
