#include "runtime/AppRuntime.h"

#include "device/DeviceInternal.h"

#include <chrono>
#include <csignal>
#include <exception>
#include <thread>
#include <utility>

namespace kavio::detail {
namespace {
volatile std::sig_atomic_t g_signal_stop_requested = 0;

void HandleStopSignal(int)
{
    g_signal_stop_requested = 1;
}

class SignalGuard {
public:
    explicit SignalGuard(bool enabled) : enabled_(enabled)
    {
        if (!enabled_) return;
        g_signal_stop_requested = 0;
        old_int_ = std::signal(SIGINT, HandleStopSignal);
        old_term_ = std::signal(SIGTERM, HandleStopSignal);
    }

    ~SignalGuard()
    {
        if (!enabled_) return;
        (void)std::signal(SIGINT, old_int_);
        (void)std::signal(SIGTERM, old_term_);
    }

private:
    bool enabled_ = false;
    using SignalHandler = void (*)(int);
    SignalHandler old_int_ = SIG_DFL;
    SignalHandler old_term_ = SIG_DFL;
};
}  // namespace

AppRuntime::AppRuntime(AppOptions options)
    : options_(std::move(options)),
      presentation_runtime_(presentation_),
      context_(presentation_, device_)
{
    if (options_.idle_sleep_ms == 0) options_.idle_sleep_ms = 1;
}

AppRuntime::~AppRuntime()
{
    RequestStop();
    if (device_runtime_.started()) {
        presentation_runtime_.Shutdown(device_runtime_.client());
        DeviceAccess::Unbind(device_);
        device_runtime_.Stop();
    }
}

void AppRuntime::SetReadyHandler(App::ReadyHandler handler)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    ready_handler_ = std::move(handler);
}

void AppRuntime::SetTickHandler(App::TickHandler handler)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    tick_handler_ = std::move(handler);
}

void AppRuntime::SetStopHandler(App::StopHandler handler)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    stop_handler_ = std::move(handler);
}

Status AppRuntime::last_status() const
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_status_;
}

void AppRuntime::SetLastStatus(const Status& status)
{
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_status_ = status;
}

bool AppRuntime::ProcessEvents()
{
    if (events_.Overflowed()) {
        SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                    "KavioApp event queue overflowed"));
        return false;
    }

    for (auto& event : events_.Drain()) {
        if (event.type == EventRuntime::Event::Type::kDisconnected) {
            if (!stop_requested_.load()) {
                SetLastStatus(Status::Error(StatusCode::Common_NotConnected,
                                            "Kavio Device Session disconnected"));
            }
            return false;
        }

        const auto& message = event.message;
        const auto type = message.header().msg_type();
        if (type == kavio::ipc::proto::MSG_P2A_RSP_CREATE_SESSION &&
            !presentation_requested_) {
            const Status status = presentation_runtime_.Initialize(device_runtime_.client());
            if (!status.ok()) {
                SetLastStatus(status);
                return false;
            }
            presentation_requested_ = true;
        }

        bool consumed = false;
        bool became_ready = false;
        const Status status = presentation_runtime_.HandleMessage(
            message, device_runtime_.client(), consumed, became_ready);
        if (!status.ok()) {
            SetLastStatus(status);
            return false;
        }

        DeviceAccess::HandleMessage(device_, message);

        if (became_ready) {
            // Every KavioApp has a defined first frame even when the App does not
            // install a widget framework or custom renderer.
            const Status clear_status = presentation_.Clear(0x20, 0x12, 0x0B, 0xFF);
            if (!clear_status.ok()) {
                SetLastStatus(clear_status);
                return false;
            }

            App::ReadyHandler handler;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                if (!ready_callback_called_) {
                    ready_callback_called_ = true;
                    handler = ready_handler_;
                }
            }
            if (handler) handler(context_);
        }

        (void)consumed;
    }
    return true;
}

int AppRuntime::Run()
{
    if (running_) {
        SetLastStatus(Status::Error(StatusCode::Common_AlreadyExists,
                                    "KavioApp is already running"));
        return 1;
    }

    running_ = true;
    events_.Reset();
    stop_requested_.store(false);
    presentation_requested_ = false;
    ready_callback_called_ = false;
    SetLastStatus(Status::Ok());
    SignalGuard signal_guard(options_.install_signal_handlers);

    const Status start_status = device_runtime_.Start(options_, events_);
    if (!start_status.ok()) {
        SetLastStatus(start_status);
        running_ = false;
        return 1;
    }
    DeviceAccess::Bind(device_, device_runtime_.client());

    bool runtime_ok = true;
    while (!stop_requested_.load()) {
        if (options_.install_signal_handlers && g_signal_stop_requested != 0) {
            stop_requested_.store(true);
            break;
        }
        try {
            if (!ProcessEvents()) {
                runtime_ok = false;
                break;
            }
        } catch (const std::exception& exc) {
            SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                        std::string("KavioApp callback exception: ") + exc.what()));
            runtime_ok = false;
            break;
        } catch (...) {
            SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                        "KavioApp callback raised an unknown exception"));
            runtime_ok = false;
            break;
        }
        App::TickHandler tick_handler;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            tick_handler = tick_handler_;
        }
        if (tick_handler && ready_callback_called_) {
            try {
                tick_handler(context_);
            } catch (const std::exception& exc) {
                SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                            std::string("KavioApp tick exception: ") + exc.what()));
                runtime_ok = false;
                break;
            } catch (...) {
                SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                            "KavioApp tick raised an unknown exception"));
                runtime_ok = false;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(options_.idle_sleep_ms));
    }

    App::StopHandler stop_handler;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stop_handler = stop_handler_;
    }
    if (stop_handler && ready_callback_called_) {
        try {
            stop_handler(context_);
        } catch (const std::exception& exc) {
            SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                        std::string("KavioApp stopping callback exception: ") + exc.what()));
            runtime_ok = false;
        } catch (...) {
            SetLastStatus(Status::Error(StatusCode::Common_InternalError,
                                        "KavioApp stopping callback raised an unknown exception"));
            runtime_ok = false;
        }
    }

    presentation_runtime_.Shutdown(device_runtime_.client());
    DeviceAccess::Unbind(device_);
    device_runtime_.Stop();
    running_ = false;

    if (!runtime_ok) return 1;
    return last_status().ok() ? 0 : 1;
}

}  // namespace kavio::detail
