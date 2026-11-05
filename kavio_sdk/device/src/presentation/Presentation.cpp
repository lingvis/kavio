#include <kavio/presentation/Presentation.h>

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>
#include <thread>
#include <memory>

#include <kavio/AppViewSurface.h>
#include <kavio/KavioClient.h>

#include "presentation/FrameBufferInternal.h"
#include "presentation/PresentationInternal.h"

namespace kavio {

struct Presentation::Impl {
    std::shared_ptr<AppViewSurface> surface = std::make_shared<AppViewSurface>();
    uint64_t create_request_id = 0;
    uint64_t generation = 0;
    uint64_t last_touch_sequence = 0;
    uint32_t pending_visible_surface_id = 0;
    bool pending_visible = false;
    bool creating = false;
    bool ready = false;
    bool visible = false;
    TouchHandler touch_handler;
    VisibilityHandler visibility_handler;
    uint64_t next_observer_id = 1;
    std::vector<std::pair<uint64_t, TouchHandler>> touch_observers;
    std::vector<std::pair<uint64_t, VisibilityHandler>> visibility_observers;
};

Presentation::Presentation() : impl_(std::make_unique<Impl>()) {}
Presentation::~Presentation() = default;
Presentation::Presentation(Presentation&&) noexcept = default;
Presentation& Presentation::operator=(Presentation&&) noexcept = default;

bool Presentation::IsReady() const noexcept
{
    return impl_ && impl_->ready && impl_->surface && impl_->surface->IsValid();
}

bool Presentation::IsVisible() const noexcept
{
    return IsReady() && impl_->visible;
}

uint64_t Presentation::Generation() const noexcept
{
    return impl_ ? impl_->generation : 0;
}

FrameBufferInfo Presentation::FrameInfo() const noexcept
{
    FrameBufferInfo info;
    if (!IsReady()) return info;
    const AppViewSurfaceDescriptor descriptor = impl_->surface->Descriptor();
    info.width = descriptor.width;
    info.height = descriptor.height;
    info.stride = descriptor.stride;
    info.pixel_format = descriptor.format == AppViewSurfaceFormat::kBgra8888
        ? PixelFormat::kBGRA8888
        : PixelFormat::kUnknown;
    info.buffer_size = static_cast<uint64_t>(descriptor.stride) * descriptor.height;
    info.surface_id = descriptor.id;
    info.generation = impl_->generation;
    return info;
}

Status Presentation::AcquireFrame(FrameLease& out_frame)
{
    out_frame = FrameLease{};
    if (!IsReady()) {
        return Status::Error(StatusCode::Common_InvalidState,
                             "AppView Presentation is not ready");
    }
    const Status status = impl_->surface->BeginWrite();
    if (!status.ok()) return status;

    auto lease = std::make_unique<FrameLease::Impl>();
    lease->surface = impl_->surface;
    lease->info = FrameInfo();
    lease->active = true;
    lease->owner_thread = std::this_thread::get_id();
    out_frame = FrameLease(std::move(lease));
    return Status::Ok();
}

Status Presentation::Clear(uint8_t blue, uint8_t green, uint8_t red, uint8_t alpha)
{
    FrameLease frame;
    Status status = AcquireFrame(frame);
    if (!status.ok()) return status;

    uint8_t* pixels = frame.Data();
    const FrameBufferInfo info = frame.Info();
    if (!pixels || info.pixel_format != PixelFormat::kBGRA8888 ||
        info.stride < info.width * 4U) {
        (void)frame.Discard();
        return Status::Error(StatusCode::Common_InvalidState,
                             "AppView frame buffer is not writable BGRA8888");
    }

    for (uint32_t y = 0; y < info.height; ++y) {
        uint8_t* row = pixels + static_cast<size_t>(y) * info.stride;
        for (uint32_t x = 0; x < info.width; ++x) {
            uint8_t* pixel = row + static_cast<size_t>(x) * 4U;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = alpha;
        }
        const size_t payload = static_cast<size_t>(info.width) * 4U;
        if (info.stride > payload) {
            std::memset(row + payload, 0, info.stride - payload);
        }
    }
    return frame.Present();
}

void Presentation::SetTouchHandler(TouchHandler handler)
{
    if (impl_) impl_->touch_handler = std::move(handler);
}

void Presentation::SetVisibilityHandler(VisibilityHandler handler)
{
    if (impl_) impl_->visibility_handler = std::move(handler);
}

Presentation::ObserverId Presentation::RuntimeAddTouchObserver(TouchHandler handler)
{
    if (!impl_ || !handler) return 0;
    const ObserverId id = impl_->next_observer_id++;
    if (impl_->next_observer_id == 0) ++impl_->next_observer_id;
    impl_->touch_observers.emplace_back(id, std::move(handler));
    return id;
}

Presentation::ObserverId Presentation::RuntimeAddVisibilityObserver(VisibilityHandler handler)
{
    if (!impl_ || !handler) return 0;
    const ObserverId id = impl_->next_observer_id++;
    if (impl_->next_observer_id == 0) ++impl_->next_observer_id;
    impl_->visibility_observers.emplace_back(id, std::move(handler));
    return id;
}

void Presentation::RuntimeRemoveTouchObserver(ObserverId id) noexcept
{
    if (!impl_ || id == 0) return;
    auto& observers = impl_->touch_observers;
    observers.erase(std::remove_if(observers.begin(), observers.end(),
        [id](const auto& item) { return item.first == id; }), observers.end());
}

void Presentation::RuntimeRemoveVisibilityObserver(ObserverId id) noexcept
{
    if (!impl_ || id == 0) return;
    auto& observers = impl_->visibility_observers;
    observers.erase(std::remove_if(observers.begin(), observers.end(),
        [id](const auto& item) { return item.first == id; }), observers.end());
}

Status detail::PresentationAccess::Initialize(Presentation& presentation, KavioClient& client)
{
    Presentation::Impl* impl = presentation.impl_.get();
    if (!impl) {
        return Status::Error(StatusCode::Common_InternalError,
                             "Presentation implementation is unavailable");
    }
    if (impl->creating || impl->ready || (impl->surface && impl->surface->IsValid())) {
        return Status::Error(StatusCode::Common_AlreadyExists,
                             "AppView Presentation is already initializing or ready");
    }
    uint64_t request_id = 0;
    const Status status = client.CreateAppViewSurface(request_id);
    if (!status.ok()) return status;
    impl->create_request_id = request_id;
    impl->creating = true;
    return Status::Ok();
}

Status detail::PresentationAccess::HandleMessage(Presentation& presentation,
                                                   const ipc::proto::IPCPackagePb& message,
                                                   KavioClient& client,
                                                   bool& consumed,
                                                   bool& became_ready)
{
    consumed = false;
    became_ready = false;
    Presentation::Impl* impl = presentation.impl_.get();
    if (!impl) {
        return Status::Error(StatusCode::Common_InternalError,
                             "Presentation implementation is unavailable");
    }

    const auto type = message.header().msg_type();

    if (type == ipc::proto::MSG_P2A_NTF_APP_VIEW_VISIBILITY &&
        message.payload().has_p2a_ntf_app_view_visibility()) {
        const auto& event = message.payload().p2a_ntf_app_view_visibility();
        consumed = true;
        if (!impl->ready) {
            impl->pending_visible_surface_id = event.surface_id();
            impl->pending_visible = event.visible();
            return Status::Ok();
        }
        if (event.surface_id() != impl->surface->id()) return Status::Ok();
        const bool changed = impl->visible != event.visible();
        impl->visible = event.visible();
        if (changed) {
            if (impl->visibility_handler) impl->visibility_handler(impl->visible);
            for (const auto& observer : impl->visibility_observers) {
                if (observer.second) observer.second(impl->visible);
            }
        }
        return Status::Ok();
    }

    if (type == ipc::proto::MSG_P2A_NTF_APP_VIEW_TOUCH_FRAME &&
        message.payload().has_p2a_ntf_app_view_touch_frame()) {
        const auto& event = message.payload().p2a_ntf_app_view_touch_frame();
        consumed = true;
        if (!impl->ready || event.surface_id() != impl->surface->id() ||
            event.sequence() <= impl->last_touch_sequence) {
            return Status::Ok();
        }
        impl->last_touch_sequence = event.sequence();
        if (!impl->touch_handler && impl->touch_observers.empty()) return Status::Ok();

        TouchFrame frame;
        frame.surface_id = event.surface_id();
        frame.sequence = event.sequence();
        frame.cancelled = event.cancelled();
        frame.points.reserve(static_cast<size_t>(event.points_size()));
        for (const auto& point : event.points()) {
            TouchPoint converted;
            converted.slot = point.slot();
            converted.tracking_id = point.tracking_id();
            converted.x = point.x();
            converted.y = point.y();
            converted.pressure = point.pressure();
            converted.touch_major = point.touch_major();
            converted.active = point.active();
            converted.changed = point.changed();
            frame.points.push_back(converted);
        }
        if (impl->touch_handler) impl->touch_handler(frame);
        for (const auto& observer : impl->touch_observers) {
            if (observer.second) observer.second(frame);
        }
        return Status::Ok();
    }

    if (!impl->creating) return Status::Ok();

    const uint64_t track_id = message.header().track_id();
    const bool track_matches = impl->create_request_id == 0 || track_id == impl->create_request_id;

    if (type == ipc::proto::MSG_P2A_RSP_CREATE_APP_VIEW_SURFACE &&
        message.payload().has_p2a_rsp_create_app_view_surface() && track_matches) {
        consumed = true;
        impl->creating = false;
        const auto& info = message.payload().p2a_rsp_create_app_view_surface().surface();
        if (info.surface_id() == 0 || info.name().empty()) {
            return Status::Error(StatusCode::Common_ProtocolError,
                                 "Platform returned invalid AppView surface info");
        }
        Status status = client.OpenAppViewSurface(info, *impl->surface);
        if (!status.ok()) {
            uint64_t destroy_request_id = 0;
            (void)client.DestroyAppViewSurface(info.surface_id(), destroy_request_id);
            return status;
        }
        ++impl->generation;
        if (impl->generation == 0) ++impl->generation;
        impl->last_touch_sequence = 0;
        impl->ready = true;
        const bool had_pending_visibility =
            impl->pending_visible_surface_id == impl->surface->id();
        impl->visible = had_pending_visibility ? impl->pending_visible : false;
        impl->pending_visible_surface_id = 0;
        impl->pending_visible = false;
        if (had_pending_visibility) {
            if (impl->visibility_handler) impl->visibility_handler(impl->visible);
            for (const auto& observer : impl->visibility_observers) {
                if (observer.second) observer.second(impl->visible);
            }
        }
        became_ready = true;
        return Status::Ok();
    }

    if (type == ipc::proto::MSG_P2A_RSP_STATUS &&
        message.payload().has_p2a_rsp_status() && track_matches) {
        consumed = true;
        impl->creating = false;
        const auto& body = message.payload().p2a_rsp_status();
        return Status::Error(
            body.code() == ipc::proto::STATUS_OK
                ? StatusCode::Common_ProtocolError
                : StatusCode::SHM_CreateFailed,
            body.code() == ipc::proto::STATUS_OK
                ? "AppView create returned a status-only success reply"
                : body.message());
    }

    if (type == ipc::proto::MSG_P2A_RSP_ERROR &&
        message.payload().has_p2a_rsp_error()) {
        const auto& body = message.payload().p2a_rsp_error();
        if (track_matches || body.track_id() == impl->create_request_id) {
            consumed = true;
            impl->creating = false;
            return Status::Error(StatusCode::SHM_CreateFailed, body.message());
        }
    }

    return Status::Ok();
}

void detail::PresentationAccess::Shutdown(Presentation& presentation, KavioClient& client) noexcept
{
    Presentation::Impl* impl = presentation.impl_.get();
    if (!impl) return;
    if (impl->surface && impl->surface->IsValid()) {
        const uint32_t id = impl->surface->id();
        if (client.IsClientReady()) {
            uint64_t request_id = 0;
            (void)client.DestroyAppViewSurface(id, request_id);
        }
    }
    if (impl->surface) impl->surface->Close();
    impl->create_request_id = 0;
    impl->creating = false;
    impl->ready = false;
    impl->visible = false;
    impl->last_touch_sequence = 0;
    impl->pending_visible_surface_id = 0;
    impl->pending_visible = false;
}

}  // namespace kavio
