#include <kavio/presentation/FrameBuffer.h>

#include <kavio/AppViewRect.h>
#include <kavio/AppViewSurface.h>

#include "presentation/FrameBufferInternal.h"

#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace kavio {


FrameLease::FrameLease() = default;
FrameLease::FrameLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

FrameLease::~FrameLease() = default;

FrameLease::FrameLease(FrameLease&& other) noexcept = default;
FrameLease& FrameLease::operator=(FrameLease&& other) noexcept = default;

bool FrameLease::IsValid() const noexcept
{
    return impl_ && impl_->active && impl_->surface && impl_->surface->IsValid() &&
           impl_->owner_thread == std::this_thread::get_id();
}

uint8_t* FrameLease::Data() noexcept
{
    return IsValid() ? impl_->surface->GetBuffer() : nullptr;
}

const FrameBufferInfo& FrameLease::Info() const noexcept
{
    static const FrameBufferInfo kEmpty{};
    return impl_ ? impl_->info : kEmpty;
}

Status FrameLease::Present()
{
    return Present(nullptr, 0);
}

Status FrameLease::Present(const Rect* dirty_rects, size_t dirty_rect_count)
{
    if (!IsValid()) {
        return Status::Error(StatusCode::Common_InvalidState,
                             "FrameLease is not active on the acquiring thread");
    }
    if (dirty_rect_count != 0 && dirty_rects == nullptr) {
        return Status::Error(StatusCode::Common_InvalidArgument,
                             "dirty_rects is null while dirty_rect_count is non-zero");
    }

    std::vector<AppViewRect> converted;
    if (dirty_rects && dirty_rect_count) {
        converted.reserve(dirty_rect_count);
        for (size_t i = 0; i < dirty_rect_count; ++i) {
            if (dirty_rects[i].width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
                dirty_rects[i].height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
                return Status::Error(StatusCode::Common_InvalidArgument,
                                     "dirty rectangle dimensions exceed AppView range");
            }
            converted.push_back(AppViewRect{
                dirty_rects[i].pos_x,
                dirty_rects[i].pos_y,
                static_cast<int32_t>(dirty_rects[i].width),
                static_cast<int32_t>(dirty_rects[i].height),
            });
        }
    }

    const Status status = converted.empty()
        ? impl_->surface->Present()
        : impl_->surface->Present(converted.data(), converted.size());

    // Present normally releases the write lock before the asynchronous
    // frame-ready notification. If the unlock itself failed, GetBuffer() stays
    // available and the lease remains active so the caller can still recover.
    impl_->active = impl_->surface->GetBuffer() != nullptr;
    return status;
}

Status FrameLease::Discard()
{
    if (!impl_ || !impl_->active || !impl_->surface) {
        return Status::Error(StatusCode::Common_InvalidState,
                             "FrameLease is not active");
    }
    if (impl_->owner_thread != std::this_thread::get_id()) {
        return Status::Error(StatusCode::Common_InvalidState,
                             "FrameLease must be discarded on the acquiring thread");
    }
    const Status status = impl_->surface->CancelWrite();
    impl_->active = impl_->surface->GetBuffer() != nullptr;
    return status;
}

}  // namespace kavio
