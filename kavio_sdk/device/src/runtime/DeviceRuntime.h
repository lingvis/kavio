#pragma once

#include <memory>

#include <kavio/App.h>
#include <kavio/KavioClient.h>

namespace kavio::detail {

class EventRuntime;

class DeviceRuntime {
public:
    DeviceRuntime();
    ~DeviceRuntime();

    DeviceRuntime(const DeviceRuntime&) = delete;
    DeviceRuntime& operator=(const DeviceRuntime&) = delete;

    Status Start(const AppOptions& options, EventRuntime& events);
    void Stop() noexcept;

    bool started() const noexcept { return client_ != nullptr && handler_ != nullptr; }
    KavioClient& client() noexcept { return *client_; }
    const KavioClient& client() const noexcept { return *client_; }

private:
    class Handler;
    // The handler must outlive the client dispatch threads. Stop() destroys the
    // client first, which joins those threads, and only then releases the handler.
    std::unique_ptr<Handler> handler_;
    std::unique_ptr<KavioClient> client_;
};

}  // namespace kavio::detail
