#include <kavio/KavioClient.h>
#include <kavio/LvglAppViewAdapter.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {

std::atomic<bool> g_running{true};

void OnSignal(int)
{
    g_running.store(false);
}

uint32_t LvglTickMs()
{
    static const auto start = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count());
}

class AppFrame final : public kavio::IClientMessageHandler {
public:
    AppFrame() : app_view_(client_) {}

    int Run()
    {
        lv_init();
        lv_tick_set_cb(LvglTickMs);
        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);

        // Keep the template session minimal. Enable RX/TX only when the App needs them.
        kavio::ipc::proto::A2PReqCreateSessionPb options;
        options.set_client_name("kavio_app_frame");
        options.mutable_rx_params()->set_enabled(false);
        options.mutable_rx_params()->set_frame_sample_interval(1);
        options.mutable_tx_params()->set_enabled(false);
        options.mutable_tx_params()->set_z_order(0);

        const kavio::Status status = client_.Start(options, *this, true);
        if (!status.ok()) {
            std::cerr << "KavioClient start failed: " << status.message << '\n';
            lv_deinit();
            return 1;
        }

        bool ui_created = false;
        while (g_running.load() && !disconnected_.load()) {
            const kavio::Status pending = app_view_.ProcessPending();
            if (!pending.ok()) {
                std::cerr << "AppView initialization failed: " << pending.message << '\n';
                startup_failed_.store(true);
                break;
            }

            if (app_view_.Display()) {
                app_view_.MakeDefault();
                if (!ui_created) {
                    CreateUi();
                    ui_created = true;
                }
                (void)lv_timer_handler();
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        app_view_.Shutdown();
        client_.Stop();
        lv_deinit();
        return startup_failed_.load() ? 1 : 0;
    }

    void OnMessage(const kavio::ipc::proto::IPCPackagePb& message) override
    {
        // AppView consumes its own asynchronous create/visibility/touch messages first.
        if (app_view_.HandleMessage(message)) return;

        if (message.header().msg_type() == kavio::ipc::proto::MSG_P2A_RSP_CREATE_SESSION) {
            bool expected = false;
            if (app_view_requested_.compare_exchange_strong(expected, true)) {
                const kavio::Status status = app_view_.Initialize();
                if (!status.ok()) {
                    std::cerr << "AppView create request failed: " << status.message << '\n';
                    startup_failed_.store(true);
                    g_running.store(false);
                }
            }
        }

        // Add App-specific asynchronous SDK responses here.
    }

    void OnDisconnected() override
    {
        disconnected_.store(true);
    }

private:
    void CreateUi()
    {
        lv_obj_t* screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1220), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

        lv_obj_t* title = lv_label_create(screen);
        lv_label_set_text(title, "Kavio App Frame");
        lv_obj_set_style_text_color(title, lv_color_hex(0xF3F7FC), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_align(title, LV_ALIGN_CENTER, 0, -24);

        lv_obj_t* subtitle = lv_label_create(screen);
        lv_label_set_text(subtitle, "SDK connected - replace this UI with your App");
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0x91A4BA), 0);
        lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
        lv_obj_align(subtitle, LV_ALIGN_CENTER, 0, 24);
    }

    kavio::KavioClient client_;
    kavio::LvglAppViewAdapter app_view_;
    std::atomic<bool> app_view_requested_{false};
    std::atomic<bool> startup_failed_{false};
    std::atomic<bool> disconnected_{false};
};

} // namespace

int main()
{
    AppFrame app;
    return app.Run();
}
