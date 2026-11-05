#pragma once

/** Application entry point and lifecycle callbacks. Create one App per running application. */

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <kavio/AppApi.h>
#include <kavio/AppContext.h>
#include <kavio/Status.h>

namespace kavio {
namespace detail {
class AppRuntime;
}

struct AppOptions {
    /** Session client name. Empty selects the SDK default process name. */
    std::string client_name;

    /** App loop sleep interval. This is not a rendering frame interval. */
    uint32_t idle_sleep_ms = 5;

    /** Optional HDMI RX capability enabled when the Device Session is created. */
    bool rx_enabled = false;
    uint32_t rx_frame_sample_interval = 1;

    /** Optional HDMI TX capability enabled when the Device Session is created. */
    bool tx_enabled = false;
    int32_t tx_z_order = 0;

    /** Install SIGINT/SIGTERM handlers that request a clean App shutdown. */
    bool install_signal_handlers = true;
};

/**
 * @brief High-level Kavio Device App runtime.
 *
 * run() owns the Device Session, AppView Presentation, event dispatch loop and
 * shutdown sequence. A minimal App therefore needs no explicit KavioClient or
 * AppView setup.
 */
class KAVIO_APP_API App {
public:
    using ReadyHandler = std::function<void(AppContext&)>;
    using TickHandler = std::function<void(AppContext&)>;
    using StopHandler = std::function<void(AppContext&)>;

    App();
    explicit App(AppOptions options);
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;
    App(App&&) = delete;
    App& operator=(App&&) = delete;

    /** Blocking run loop. Returns 0 on a clean stop and non-zero on runtime failure. */
    int run();

    /** Thread-safe stop request. */
    void request_stop() noexcept;

    /** Called once on the App run thread after AppView is mapped and the first frame is published. */
    void on_ready(ReadyHandler handler);

    /** Called on the App run thread once per runtime loop after queued events are dispatched. */
    void on_tick(TickHandler handler);

    /** Called on the App run thread immediately before Presentation and Device shutdown. */
    void on_stopping(StopHandler handler);

    AppContext& context() noexcept;
    const AppContext& context() const noexcept;
    Status last_status() const;

private:
    std::unique_ptr<detail::AppRuntime> runtime_;
};

}  // namespace kavio
