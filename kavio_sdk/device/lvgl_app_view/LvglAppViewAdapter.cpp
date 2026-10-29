#include <kavio/LvglAppViewAdapter.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include <kavio/detail/AppViewSurfaceShared.h>
#include <kavio/detail/AppViewDirtyRegion.h>

namespace kavio {
namespace {
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kPartialDrawLines = 64;
constexpr size_t kMaxPointerSamples = 64;

bool AddDirtyRect(std::array<AppViewRect, detail::kAppViewMaxDirtyRects>& rects,
                  uint32_t& count, const AppViewRect& input, uint64_t canvas_pixels)
{
    return detail::AppendAppViewRegion(rects.data(), count, detail::kAppViewMaxDirtyRects,
                                       input, canvas_pixels);
}
}

LvglAppViewAdapter::LvglAppViewAdapter(KavioClient& client) : client_(&client)
{
}

LvglAppViewAdapter::~LvglAppViewAdapter()
{
    Shutdown();
}

Status LvglAppViewAdapter::Initialize()
{
    if (!client_ || !client_->IsClientReady()) {
        return Status::Error(StatusCode::Common_NotConnected,
                             "KavioClient Session is not ready");
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (creating_ || display_ || surface_.IsValid()) {
            return Status::Error(StatusCode::Common_AlreadyExists,
                                 "LVGL AppView adapter is already initialized or initializing");
        }
        creating_ = true;
        pending_ready_ = false;
        pending_has_info_ = false;
        create_request_id_ = 0;
        pending_status_ = Status::Ok();
        last_status_ = Status::Ok();
    }

    uint64_t request_id = 0;
    const Status status = client_->CreateAppViewSurface(request_id);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!status.ok()) {
            creating_ = false;
            last_status_ = status;
            return status;
        }
        create_request_id_ = request_id;
    }
    return Status::Ok();
}

bool LvglAppViewAdapter::HandleMessage(const kavio::ipc::proto::IPCPackagePb& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto type = message.header().msg_type();
    if (type == kavio::ipc::proto::MSG_P2A_NTF_APP_VIEW_VISIBILITY &&
        message.payload().has_p2a_ntf_app_view_visibility()) {
        const auto& event = message.payload().p2a_ntf_app_view_visibility();
        if (active_surface_id_ == 0) {
            // Surface creation notification can arrive before UI thread maps it.
            pending_visible_surface_id_ = event.surface_id();
            pending_visible_ = event.visible();
            return true;
        }
        if (event.surface_id() != active_surface_id_) return true;
        foreground_ = event.visible();
        if (!foreground_) {
            pointer_queue_.clear();
            pointer_queue_.push_back(PointerSample{last_received_x_, last_received_y_, false});
        }
        return true;
    }
    if (type == kavio::ipc::proto::MSG_P2A_NTF_APP_VIEW_POINTER &&
        message.payload().has_p2a_ntf_app_view_pointer()) {
        const auto& event = message.payload().p2a_ntf_app_view_pointer();
        if (active_surface_id_ == 0 || event.surface_id() != active_surface_id_ ||
            !foreground_ || event.sequence() <= last_pointer_sequence_) return true;
        last_pointer_sequence_ = event.sequence();
        last_received_x_ = event.x();
        last_received_y_ = event.y();
        if (event.cancelled()) {
            pointer_queue_.clear();
            pointer_queue_.push_back(PointerSample{event.x(), event.y(), false});
        } else {
            // Preserve transitions; coalesce only continuous moves so a fast
            // press+release between LVGL ticks still reaches the indev driver.
            PointerSample sample{event.x(), event.y(), event.pressed()};
            if (!pointer_queue_.empty() && pointer_queue_.back().pressed && sample.pressed) {
                pointer_queue_.back() = sample;
            } else {
                if (pointer_queue_.size() >= kMaxPointerSamples) {
                    pointer_queue_.clear();
                    pointer_queue_.push_back(PointerSample{event.x(), event.y(), false});
                }
                pointer_queue_.push_back(sample);
            }
        }
        return true;
    }
    if (!creating_) return false;

    const uint64_t track_id = message.header().track_id();
    if (type == kavio::ipc::proto::MSG_P2A_RSP_CREATE_APP_VIEW_SURFACE &&
        message.payload().has_p2a_rsp_create_app_view_surface() &&
        (create_request_id_ == 0 || track_id == create_request_id_)) {
        pending_info_.CopyFrom(message.payload().p2a_rsp_create_app_view_surface().surface());
        pending_has_info_ = pending_info_.surface_id() != 0 && !pending_info_.name().empty();
        pending_status_ = pending_has_info_
            ? Status::Ok()
            : Status::Error(StatusCode::Common_ProtocolError,
                            "Platform returned invalid AppView surface info");
        pending_ready_ = true;
        return true;
    }

    if (type == kavio::ipc::proto::MSG_P2A_RSP_STATUS &&
        message.payload().has_p2a_rsp_status() &&
        (create_request_id_ == 0 || track_id == create_request_id_)) {
        const auto code = message.payload().p2a_rsp_status().code();
        pending_status_ = code == kavio::ipc::proto::STATUS_OK
            ? Status::Error(StatusCode::Common_ProtocolError,
                            "AppView create returned a status-only success reply")
            : Status::Error(StatusCode::SHM_CreateFailed,
                            message.payload().p2a_rsp_status().message());
        pending_has_info_ = false;
        pending_ready_ = true;
        return true;
    }

    if (type == kavio::ipc::proto::MSG_P2A_RSP_ERROR &&
        message.payload().has_p2a_rsp_error() &&
        (create_request_id_ == 0 || track_id == create_request_id_ ||
         message.payload().p2a_rsp_error().track_id() == create_request_id_)) {
        pending_status_ = Status::Error(StatusCode::SHM_CreateFailed,
                                        message.payload().p2a_rsp_error().message());
        pending_has_info_ = false;
        pending_ready_ = true;
        return true;
    }
    return false;
}

Status LvglAppViewAdapter::ProcessPending()
{
    kavio::ipc::proto::AppViewSurfaceInfoPb info;
    Status result = Status::Ok();
    bool has_info = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_ready_) return Status::Ok();
        pending_ready_ = false;
        creating_ = false;
        result = pending_status_;
        has_info = pending_has_info_;
        if (has_info) info.CopyFrom(pending_info_);
        pending_has_info_ = false;
    }
    if (!result.ok()) {
        SetLastStatus(result);
        return result;
    }
    if (!has_info || !client_) {
        result = Status::Error(StatusCode::Common_ProtocolError,
                               "AppView create reply did not contain surface info");
        SetLastStatus(result);
        return result;
    }

    result = client_->OpenAppViewSurface(info, surface_);
    if (!result.ok()) {
        uint64_t destroy_request_id = 0;
        (void)client_->DestroyAppViewSurface(info.surface_id(), destroy_request_id);
        SetLastStatus(result);
        return result;
    }

    width_ = surface_.width();
    height_ = surface_.height();
    stride_ = surface_.stride();
    if (width_ == 0 || height_ == 0 || stride_ < width_ * kBytesPerPixel ||
        static_cast<uint64_t>(stride_) * std::min(height_, kPartialDrawLines) >
            std::numeric_limits<size_t>::max()) {
        result = Status::Error(StatusCode::Common_InvalidArgument,
                               "AppView surface dimensions are invalid");
        uint64_t destroy_request_id = 0;
        (void)client_->DestroyAppViewSurface(surface_.id(), destroy_request_id);
        surface_.Close();
        SetLastStatus(result);
        return result;
    }

    const uint32_t draw_lines = std::min(height_, kPartialDrawLines);
    draw_buffer_.assign(static_cast<size_t>(stride_) * draw_lines, 0);
    display_ = lv_display_create(static_cast<int32_t>(width_), static_cast<int32_t>(height_));
    if (!display_) {
        result = Status::Error(StatusCode::Common_InternalError,
                               "LVGL failed to create AppView display");
        uint64_t destroy_request_id = 0;
        (void)client_->DestroyAppViewSurface(surface_.id(), destroy_request_id);
        surface_.Close();
        draw_buffer_.clear();
        SetLastStatus(result);
        return result;
    }
    lv_display_set_user_data(display_, this);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_flush_cb(display_, FlushCallback);
    lv_display_set_buffers(display_, draw_buffer_.data(), nullptr,
                           static_cast<uint32_t>(draw_buffer_.size()),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(display_);
    pointer_device_ = lv_indev_create();
    if (!pointer_device_) {
        result = Status::Error(StatusCode::Common_InternalError,
                               "LVGL failed to create AppView pointer device");
        lv_display_delete(display_);
        display_ = nullptr;
        uint64_t destroy_request_id = 0;
        (void)client_->DestroyAppViewSurface(surface_.id(), destroy_request_id);
        surface_.Close();
        draw_buffer_.clear();
        SetLastStatus(result);
        return result;
    }
    lv_indev_set_type(pointer_device_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(pointer_device_, this);
    lv_indev_set_read_cb(pointer_device_, ReadPointerCallback);
    lv_indev_set_display(pointer_device_, display_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_surface_id_ = surface_.id();
        last_pointer_sequence_ = 0;
        last_received_x_ = 0;
        last_received_y_ = 0;
        pointer_queue_.clear();
        foreground_ = pending_visible_surface_id_ == active_surface_id_ && pending_visible_;
        pending_visible_surface_id_ = 0;
        pending_visible_ = false;
    }
    SetLastStatus(Status::Ok());
    return Status::Ok();
}

bool LvglAppViewAdapter::IsForeground() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return foreground_;
}

void LvglAppViewAdapter::ReadPointerCallback(lv_indev_t* indev, lv_indev_data_t* data)
{
    auto* self = static_cast<LvglAppViewAdapter*>(lv_indev_get_user_data(indev));
    if (self) self->ReadPointer(data);
}

void LvglAppViewAdapter::ReadPointer(lv_indev_data_t* data)
{
    if (!data) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pointer_queue_.empty()) {
            last_pointer_ = pointer_queue_.front();
            pointer_queue_.pop_front();
        }
        data->continue_reading = !pointer_queue_.empty();
        if (!foreground_) {
            last_pointer_.pressed = false;
        }
    }
    data->point.x = last_pointer_.x;
    data->point.y = last_pointer_.y;
    data->state = last_pointer_.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void LvglAppViewAdapter::MakeDefault()
{
    if (display_) lv_display_set_default(display_);
}

Status LvglAppViewAdapter::LastStatus() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return last_status_;
}

void LvglAppViewAdapter::Shutdown()
{
    if (frame_write_locked_ && surface_.IsValid()) {
        (void)surface_.Present();
        frame_write_locked_ = false;
    }
    dirty_rect_count_ = 0;
    dirty_overflow_ = false;
    dirty_perf_ = {};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_surface_id_ = 0;
        pointer_queue_.clear();
        foreground_ = false;
        pending_visible_surface_id_ = 0;
        pending_visible_ = false;
        last_pointer_sequence_ = 0;
        last_received_x_ = 0;
        last_received_y_ = 0;
    }
    last_pointer_ = {};
    if (pointer_device_) {
        lv_indev_delete(pointer_device_);
        pointer_device_ = nullptr;
    }
    if (display_) {
        lv_display_delete(display_);
        display_ = nullptr;
    }
    if (surface_.IsValid()) {
        const uint32_t id = surface_.id();
        if (client_ && client_->IsClientReady()) {
            uint64_t request_id = 0;
            (void)client_->DestroyAppViewSurface(id, request_id);
        }
    }
    surface_.Close();
    frame_write_locked_ = false;
    frame_copy_status_ = Status::Ok();
    draw_buffer_.clear();
    width_ = height_ = stride_ = 0;
    std::lock_guard<std::mutex> lock(mutex_);
    creating_ = false;
    pending_ready_ = false;
    pending_has_info_ = false;
    create_request_id_ = 0;
    pending_info_.Clear();
}

void LvglAppViewAdapter::FlushCallback(lv_display_t* display,
                                       const lv_area_t* area,
                                       uint8_t* px_map)
{
    auto* self = static_cast<LvglAppViewAdapter*>(lv_display_get_user_data(display));
    if (self) self->Flush(display, area, px_map);
    lv_display_flush_ready(display);
}

void LvglAppViewAdapter::Flush(lv_display_t* display,
                               const lv_area_t* area,
                               const uint8_t* px_map)
{
    if (!surface_.IsValid()) return;
    const bool last_flush = lv_display_flush_is_last(display);
    if (!frame_write_locked_) {
        const Status begin_status = surface_.BeginWrite();
        if (!begin_status.ok()) {
            SetLastStatus(begin_status);
            return;
        }
        frame_write_locked_ = true;
        frame_copy_status_ = Status::Ok();
        dirty_rect_count_ = 0;
        dirty_overflow_ = false;
    }

    Status copy_status = Status::Ok();
    if (area && px_map) {
        const int32_t x1 = std::max<int32_t>(0, area->x1);
        const int32_t y1 = std::max<int32_t>(0, area->y1);
        const int32_t x2 = std::min<int32_t>(static_cast<int32_t>(width_) - 1, area->x2);
        const int32_t y2 = std::min<int32_t>(static_cast<int32_t>(height_) - 1, area->y2);
        const int32_t source_width_signed = area->x2 - area->x1 + 1;
        if (x1 <= x2 && y1 <= y2 && source_width_signed > 0) {
            const uint32_t source_width = static_cast<uint32_t>(source_width_signed);
            const uint32_t copy_width = static_cast<uint32_t>(x2 - x1 + 1);
            const uint32_t copy_height = static_cast<uint32_t>(y2 - y1 + 1);
            ++dirty_perf_.flushes;
            const uint64_t flush_pixels = static_cast<uint64_t>(copy_width) * copy_height;
            dirty_perf_.raw_pixels += flush_pixels;
            if (flush_pixels * 10 >= static_cast<uint64_t>(width_) * height_ * 9) {
                ++dirty_perf_.near_full_flushes;
            }
            const uint32_t skip_x = static_cast<uint32_t>(x1 - area->x1);
            const uint32_t skip_y = static_cast<uint32_t>(y1 - area->y1);
            uint8_t* canvas = surface_.GetBuffer();
            if (!canvas) {
                copy_status = Status::Error(StatusCode::Common_InvalidState,
                                            "AppView canvas is unavailable during LVGL flush");
                dirty_overflow_ = true;
            } else {
                for (uint32_t row = 0; row < copy_height; ++row) {
                    const uint8_t* src = px_map +
                        (static_cast<size_t>(skip_y + row) * source_width + skip_x) * kBytesPerPixel;
                    uint8_t* dst = canvas +
                        static_cast<size_t>(y1 + static_cast<int32_t>(row)) * stride_ +
                        static_cast<size_t>(x1) * kBytesPerPixel;
                    std::memcpy(dst, src, static_cast<size_t>(copy_width) * kBytesPerPixel);
                }
                if (!dirty_overflow_) {
                    const AppViewRect dirty{x1, y1, static_cast<int32_t>(copy_width),
                                            static_cast<int32_t>(copy_height)};
                    if (!AddDirtyRect(dirty_rects_, dirty_rect_count_, dirty,
                                      static_cast<uint64_t>(width_) * height_)) {
                        dirty_overflow_ = true;
                        dirty_rect_count_ = 0;
                    }
                }
            }
        }
    }

    if (!copy_status.ok()) {
        frame_copy_status_ = copy_status;
        dirty_overflow_ = true;
        dirty_rect_count_ = 0;
    }

    if (last_flush && frame_write_locked_) {
        ++dirty_perf_.frames;
        const uint64_t output_pixels = dirty_overflow_
            ? static_cast<uint64_t>(width_) * height_
            : detail::AppViewDirtyPixels(dirty_rects_.data(), dirty_rect_count_);
        dirty_perf_.output_pixels += output_pixels;
        dirty_perf_.output_rects += dirty_overflow_ ? 0 : dirty_rect_count_;
        dirty_perf_.full_submits += dirty_overflow_ ? 1 : 0;
        if (dirty_perf_.frames == 200) {
            const double full_pixels = static_cast<double>(width_) * height_ * dirty_perf_.frames;
            std::fprintf(stderr,
                "[AppView][Dirty][LVGL] frames=%llu flushes=%llu raw_pct=%.1f "
                "submit_pct=%.1f avg_submit_rects=%.1f near_full_flushes=%llu "
                "full_submits=%llu\n",
                static_cast<unsigned long long>(dirty_perf_.frames),
                static_cast<unsigned long long>(dirty_perf_.flushes),
                100.0 * dirty_perf_.raw_pixels / full_pixels,
                100.0 * dirty_perf_.output_pixels / full_pixels,
                static_cast<double>(dirty_perf_.output_rects) / dirty_perf_.frames,
                static_cast<unsigned long long>(dirty_perf_.near_full_flushes),
                static_cast<unsigned long long>(dirty_perf_.full_submits));
            dirty_perf_ = {};
        }
        const Status present_status = dirty_overflow_
            ? surface_.Present()
            : surface_.Present(dirty_rects_.data(), dirty_rect_count_);
        frame_write_locked_ = false;
        dirty_rect_count_ = 0;
        dirty_overflow_ = false;
        SetLastStatus(frame_copy_status_.ok() ? present_status : frame_copy_status_);
        frame_copy_status_ = Status::Ok();
    }
}

void LvglAppViewAdapter::SetLastStatus(const Status& status)
{
    std::lock_guard<std::mutex> lock(mutex_);
    last_status_ = status;
}

} // namespace kavio
