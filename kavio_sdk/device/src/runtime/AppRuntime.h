#pragma once

#include <atomic>
#include <mutex>

#include <kavio/App.h>
#include <kavio/AppContext.h>
#include <kavio/presentation/Presentation.h>
#include <kavio/Device.h>

#include "runtime/DeviceRuntime.h"
#include "runtime/EventRuntime.h"
#include "presentation/PresentationRuntime.h"

namespace kavio::detail {

class AppRuntime {
public:
    explicit AppRuntime(AppOptions options);
    ~AppRuntime();

    AppRuntime(const AppRuntime&) = delete;
    AppRuntime& operator=(const AppRuntime&) = delete;

    int Run();
    void RequestStop() noexcept { stop_requested_.store(true); }
    void SetReadyHandler(App::ReadyHandler handler);
    void SetTickHandler(App::TickHandler handler);
    void SetStopHandler(App::StopHandler handler);

    AppContext& context() noexcept { return context_; }
    const AppContext& context() const noexcept { return context_; }
    Status last_status() const;

private:
    void SetLastStatus(const Status& status);
    bool ProcessEvents();

    AppOptions options_;
    EventRuntime events_;
    DeviceRuntime device_runtime_;
    Device device_;
    Presentation presentation_;
    PresentationRuntime presentation_runtime_;
    AppContext context_;

    std::atomic<bool> stop_requested_{false};
    bool running_ = false;
    bool presentation_requested_ = false;
    bool ready_callback_called_ = false;

    mutable std::mutex state_mutex_;
    Status last_status_ = Status::Ok();
    App::ReadyHandler ready_handler_;
    App::TickHandler tick_handler_;
    App::StopHandler stop_handler_;
};

}  // namespace kavio::detail
