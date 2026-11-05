#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include <kavio/App.h>
#include <kavio/AppContext.h>
#include <kavio/Device.h>
#include <kavio/presentation/Presentation.h>
#include <kavio_c/app.h>
#include <kavio_c/device.h>
#include <kavio_c/presentation.h>

struct KavioApp;

struct KavioPresentation {
    KavioApp* owner = nullptr;
    kavio::Presentation* presentation = nullptr;
    std::mutex callback_mutex;
    KavioTouchCallback touch_callback = nullptr;
    void* touch_user_data = nullptr;
    KavioVisibilityCallback visibility_callback = nullptr;
    void* visibility_user_data = nullptr;
};

struct KavioDevice {
    KavioApp* owner = nullptr;
    kavio::Device* device = nullptr;
    std::mutex callback_mutex;
    KavioCommDataCallback comm_data_callback = nullptr;
    void* comm_data_user_data = nullptr;
    KavioTxSurfaceCallback tx_surface_callback = nullptr;
    void* tx_surface_user_data = nullptr;
    KavioH264StreamCallback h264_stream_callback = nullptr;
    void* h264_stream_user_data = nullptr;
    KavioH264InvalidatedCallback h264_invalidated_callback = nullptr;
    void* h264_invalidated_user_data = nullptr;
    KavioDeviceErrorCallback error_callback = nullptr;
    void* error_user_data = nullptr;
    KavioConfigChangedCallback config_changed_callback = nullptr;
    void* config_changed_user_data = nullptr;
};

struct KavioApp {
    explicit KavioApp(kavio::AppOptions options)
        : app(std::move(options))
    {
    }

    kavio::App app;
    std::atomic<bool> running{false};
    std::mutex callback_mutex;
    KavioAppReadyCallback ready_callback = nullptr;
    void* ready_user_data = nullptr;
    KavioAppTickCallback tick_callback = nullptr;
    void* tick_user_data = nullptr;
    KavioAppStoppingCallback stopping_callback = nullptr;
    void* stopping_user_data = nullptr;
    std::unique_ptr<KavioPresentation> presentation;
    std::unique_ptr<KavioDevice> device;
};

struct KavioFrameLease {
    explicit KavioFrameLease(kavio::FrameLease&& value)
        : lease(std::move(value)), owner_thread(std::this_thread::get_id())
    {
    }

    kavio::FrameLease lease;
    std::thread::id owner_thread;
    bool completed = false;
};

struct KavioImageFrame {
    kavio::ImageFrame frame;
};

struct KavioTxSurface {
    kavio::DeviceTxSurface surface;
};

struct KavioH264Packet {
    kavio::DeviceH264Packet packet;
};
