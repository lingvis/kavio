#include "runtime/DeviceRuntime.h"

#include "runtime/EventRuntime.h"

namespace kavio::detail {

class DeviceRuntime::Handler final : public IClientMessageHandler {
public:
    explicit Handler(EventRuntime& events) : events_(&events) {}

    void OnMessage(const kavio::ipc::proto::IPCPackagePb& message) override
    {
        if (events_) (void)events_->PushMessage(message);
    }

    void OnDisconnected() override
    {
        if (events_) (void)events_->PushDisconnected();
    }

private:
    EventRuntime* events_ = nullptr;
};

DeviceRuntime::DeviceRuntime() = default;
DeviceRuntime::~DeviceRuntime() { Stop(); }

Status DeviceRuntime::Start(const AppOptions& options, EventRuntime& events)
{
    if (handler_) {
        return Status::Error(StatusCode::Common_AlreadyExists,
                             "DeviceRuntime is already started");
    }
    if (!client_) client_ = std::make_unique<KavioClient>();

    auto handler = std::make_unique<Handler>(events);
    kavio::ipc::proto::A2PReqCreateSessionPb session;
    if (!options.client_name.empty()) session.set_client_name(options.client_name);

    auto* rx = session.mutable_rx_params();
    rx->set_enabled(options.rx_enabled);
    rx->set_frame_sample_interval(options.rx_frame_sample_interval == 0 ? 1 : options.rx_frame_sample_interval);
    auto* tx = session.mutable_tx_params();
    tx->set_enabled(options.tx_enabled);
    tx->set_z_order(options.tx_z_order);

    const Status status = client_->Start(session, *handler, true);
    if (!status.ok()) {
        client_.reset();
        return status;
    }
    handler_ = std::move(handler);
    return Status::Ok();
}

void DeviceRuntime::Stop() noexcept
{
    if (!client_) return;
    client_->Stop();
    // KavioClient destruction joins worker/dispatch threads. Only after that is
    // it safe to release the non-owning message handler used by those threads.
    client_.reset();
    handler_.reset();
}

}  // namespace kavio::detail
