#include <kavio/App.h>

#include <utility>

#include "runtime/AppRuntime.h"

namespace kavio {

App::App() : App(AppOptions{}) {}

App::App(AppOptions options)
    : runtime_(std::make_unique<detail::AppRuntime>(std::move(options)))
{
}

App::~App() = default;

int App::run()
{
    return runtime_ ? runtime_->Run() : 1;
}

void App::request_stop() noexcept
{
    if (runtime_) runtime_->RequestStop();
}

void App::on_ready(ReadyHandler handler)
{
    if (runtime_) runtime_->SetReadyHandler(std::move(handler));
}

void App::on_tick(TickHandler handler)
{
    if (runtime_) runtime_->SetTickHandler(std::move(handler));
}

void App::on_stopping(StopHandler handler)
{
    if (runtime_) runtime_->SetStopHandler(std::move(handler));
}

AppContext& App::context() noexcept
{
    return runtime_->context();
}

const AppContext& App::context() const noexcept
{
    return runtime_->context();
}

Status App::last_status() const
{
    return runtime_ ? runtime_->last_status()
                    : Status::Error(StatusCode::Common_InvalidState,
                                    "KavioApp runtime is unavailable");
}

}  // namespace kavio
