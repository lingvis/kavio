#include <kavio/ui/LvglBackend.h>

#include "ui/WidgetInternal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <utility>
#include <vector>

namespace kavio::ui {
namespace {
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kPartialDrawLines = 64;
constexpr size_t kMaxPointerSamples = 64;
constexpr size_t kMaxDirtyRects = 32;

uint32_t LvglTickMs()
{
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now() - start).count());
}

const lv_font_t* SelectFont(int32_t size)
{
    if (size >= 24) return &lv_font_montserrat_24;
    if (size >= 20) return &lv_font_montserrat_20;
    if (size >= 18) return &lv_font_montserrat_18;
    return &lv_font_montserrat_16;
}
}

struct LvglBackend::Impl {
    struct PointerSample {
        int32_t x = 0;
        int32_t y = 0;
        bool pressed = false;
    };

    Presentation* presentation = nullptr;
    uint64_t touch_observer_id = 0;
    uint64_t visibility_observer_id = 0;
    lv_display_t* display = nullptr;
    lv_indev_t* pointer_device = nullptr;
    std::vector<uint8_t> draw_buffer;
    FrameLease frame;
    std::array<Rect, kMaxDirtyRects> dirty_rects{};
    size_t dirty_count = 0;
    bool dirty_overflow = false;
    Status last_status = Status::Ok();
    bool foreground = false;
    bool lvgl_initialized = false;
    Widget mounted_root;
    // Image descriptors must outlive LVGL source references.
    std::map<const detail::WidgetNode*, std::unique_ptr<lv_image_dsc_t>> image_descriptors;

    std::deque<PointerSample> pointer_queue;
    PointerSample last_pointer{};
    uint64_t last_pointer_sequence = 0;
    int32_t last_received_x = 0;
    int32_t last_received_y = 0;
    int32_t primary_tracking_id = -1;
    int32_t primary_slot = -1;
    bool primary_sequence_active = false;
    bool suppress_until_all_released = false;

    static void FlushCallback(lv_display_t* display, const lv_area_t* area, uint8_t* px_map)
    {
        auto* self = static_cast<Impl*>(lv_display_get_user_data(display));
        if (self) self->Flush(area, px_map, lv_display_flush_is_last(display));
        lv_display_flush_ready(display);
    }

    static void ReadPointerCallback(lv_indev_t* indev, lv_indev_data_t* data)
    {
        auto* self = static_cast<Impl*>(lv_indev_get_user_data(indev));
        if (self) self->ReadPointer(data);
    }

    void ReadPointer(lv_indev_data_t* data)
    {
        if (!data) return;
        if (!pointer_queue.empty()) {
            last_pointer = pointer_queue.front();
            pointer_queue.pop_front();
        }
        data->continue_reading = !pointer_queue.empty();
        if (!foreground) last_pointer.pressed = false;
        data->point.x = last_pointer.x;
        data->point.y = last_pointer.y;
        data->state = last_pointer.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    }

    void QueuePointer(const PointerSample& sample)
    {
        if (!pointer_queue.empty() && pointer_queue.back().pressed && sample.pressed) {
            pointer_queue.back() = sample;
            return;
        }
        if (pointer_queue.size() >= kMaxPointerSamples) {
            pointer_queue.clear();
            pointer_queue.push_back(PointerSample{last_received_x, last_received_y, false});
        }
        pointer_queue.push_back(sample);
    }

    void OnTouch(const TouchFrame& event)
    {
        if (!foreground || event.sequence <= last_pointer_sequence) return;
        last_pointer_sequence = event.sequence;

        if (event.cancelled) {
            pointer_queue.clear();
            pointer_queue.push_back(PointerSample{last_received_x, last_received_y, false});
            primary_tracking_id = -1;
            primary_slot = -1;
            primary_sequence_active = false;
            suppress_until_all_released = true;
            return;
        }

        const TouchPoint* primary = nullptr;
        bool any_active = false;
        for (const auto& point : event.points) {
            if (!point.active) continue;
            any_active = true;
            if (primary_sequence_active) {
                const bool same = primary_tracking_id >= 0
                    ? point.tracking_id == primary_tracking_id
                    : point.slot == primary_slot;
                if (same) primary = &point;
            }
        }

        if (suppress_until_all_released) {
            if (!any_active) suppress_until_all_released = false;
            return;
        }

        if (!primary_sequence_active) {
            if (!any_active) return;
            for (const auto& point : event.points) {
                if (point.active) {
                    primary = &point;
                    break;
                }
            }
            if (!primary) return;
            primary_tracking_id = primary->tracking_id;
            primary_slot = primary->slot;
            primary_sequence_active = true;
        }

        if (primary) {
            last_received_x = primary->x;
            last_received_y = primary->y;
            QueuePointer(PointerSample{last_received_x, last_received_y, true});
        } else {
            QueuePointer(PointerSample{last_received_x, last_received_y, false});
            primary_tracking_id = -1;
            primary_slot = -1;
            primary_sequence_active = false;
            suppress_until_all_released = any_active;
        }
    }

    void OnVisibility(bool visible)
    {
        foreground = visible;
        if (!foreground) {
            pointer_queue.clear();
            pointer_queue.push_back(PointerSample{last_received_x, last_received_y, false});
            primary_tracking_id = -1;
            primary_slot = -1;
            primary_sequence_active = false;
            suppress_until_all_released = false;
        }
    }

    void Flush(const lv_area_t* area, const uint8_t* px_map, bool last_flush)
    {
        if (!presentation) return;
        if (!frame.IsValid()) {
            last_status = presentation->AcquireFrame(frame);
            if (!last_status.ok()) return;
            dirty_count = 0;
            dirty_overflow = false;
        }

        if (area && px_map) {
            const auto& info = frame.Info();
            const int32_t x1 = std::max<int32_t>(0, area->x1);
            const int32_t y1 = std::max<int32_t>(0, area->y1);
            const int32_t x2 = std::min<int32_t>(static_cast<int32_t>(info.width) - 1, area->x2);
            const int32_t y2 = std::min<int32_t>(static_cast<int32_t>(info.height) - 1, area->y2);
            const int32_t source_width_signed = area->x2 - area->x1 + 1;
            if (x1 <= x2 && y1 <= y2 && source_width_signed > 0) {
                const uint32_t source_width = static_cast<uint32_t>(source_width_signed);
                const uint32_t copy_width = static_cast<uint32_t>(x2 - x1 + 1);
                const uint32_t copy_height = static_cast<uint32_t>(y2 - y1 + 1);
                const uint32_t skip_x = static_cast<uint32_t>(x1 - area->x1);
                const uint32_t skip_y = static_cast<uint32_t>(y1 - area->y1);
                uint8_t* canvas = frame.Data();
                if (!canvas) {
                    last_status = Status::Error(StatusCode::Common_InvalidState,
                                                "AppView frame buffer is unavailable during LVGL flush");
                    dirty_overflow = true;
                } else {
                    for (uint32_t row = 0; row < copy_height; ++row) {
                        const uint8_t* src = px_map +
                            (static_cast<size_t>(skip_y + row) * source_width + skip_x) * kBytesPerPixel;
                        uint8_t* dst = canvas +
                            static_cast<size_t>(y1 + static_cast<int32_t>(row)) * info.stride +
                            static_cast<size_t>(x1) * kBytesPerPixel;
                        std::memcpy(dst, src, static_cast<size_t>(copy_width) * kBytesPerPixel);
                    }
                    if (!dirty_overflow) {
                        if (dirty_count < dirty_rects.size()) {
                            dirty_rects[dirty_count++] = Rect{
                                x1, y1, copy_width, copy_height};
                        } else {
                            dirty_overflow = true;
                        }
                    }
                }
            }
        }

        if (!last_flush || !frame.IsValid()) return;
        if (!last_status.ok()) {
            (void)frame.Discard();
            return;
        }
        last_status = dirty_overflow
            ? frame.Present()
            : frame.Present(dirty_rects.data(), dirty_count);
        dirty_count = 0;
        dirty_overflow = false;
    }

    static void WidgetEvent(lv_event_t* event)
    {
        auto* node = static_cast<detail::WidgetNode*>(lv_event_get_user_data(event));
        if (!node) return;
        const auto code = lv_event_get_code(event);
        if (code == LV_EVENT_CLICKED && node->on_click) {
            node->on_click();
        } else if (code == LV_EVENT_VALUE_CHANGED) {
            auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(event));
            if (node->type == WidgetType::kSlider) {
                node->value = lv_slider_get_value(obj);
                if (node->on_value) node->on_value(node->value);
            } else if (node->type == WidgetType::kSwitch) {
                node->checked = lv_obj_has_state(obj, LV_STATE_CHECKED);
                if (node->on_checked) node->on_checked(node->checked);
            }
        }
    }

    static void ApplyStyle(lv_obj_t* obj, const Style& style)
    {
        if (!obj) return;
        if (style.width >= 0) lv_obj_set_width(obj, style.width);
        if (style.height >= 0) lv_obj_set_height(obj, style.height);
        if (style.padding >= 0) lv_obj_set_style_pad_all(obj, style.padding, 0);
        if (style.gap >= 0) {
            lv_obj_set_style_pad_row(obj, style.gap, 0);
            lv_obj_set_style_pad_column(obj, style.gap, 0);
        }
        if (style.radius >= 0) lv_obj_set_style_radius(obj, style.radius, 0);
        if (style.has_background) {
            lv_obj_set_style_bg_color(obj, lv_color_hex(style.background), 0);
            lv_obj_set_style_bg_opa(obj, style.opacity, 0);
        }
        if (style.has_text_color) lv_obj_set_style_text_color(obj, lv_color_hex(style.text_color), 0);
        if (style.font_size > 0) lv_obj_set_style_text_font(obj, SelectFont(style.font_size), 0);
    }

    void RefreshNode(detail::WidgetNode& node)
    {
        auto* obj = static_cast<lv_obj_t*>(node.native);
        if (!obj) return;
        ApplyStyle(obj, node.style);
        switch (node.type) {
        case WidgetType::kLabel:
            lv_label_set_text(obj, node.text.c_str());
            break;
        case WidgetType::kButton:
            if (lv_obj_get_child_count(obj) > 0) {
                lv_obj_t* label = lv_obj_get_child(obj, 0);
                lv_label_set_text(label, node.text.c_str());
            }
            break;
        case WidgetType::kImage:
            if (!node.rgb565.empty() && node.image_width && node.image_height) {
                // Use LVGL's normal image widget for both files and pixel data.
                // The stable descriptor and its pixels remain owned by this UI tree.
                auto desc = std::make_unique<lv_image_dsc_t>();
                *desc = {};
                desc->header.magic = LV_IMAGE_HEADER_MAGIC;
                desc->header.cf = LV_COLOR_FORMAT_RGB565;
                desc->header.w = node.image_width;
                desc->header.h = node.image_height;
                desc->data_size = static_cast<uint32_t>(node.rgb565.size() * sizeof(uint16_t));
                desc->data = reinterpret_cast<const uint8_t*>(node.rgb565.data());
                lv_image_set_src(obj, desc.get());
                // Keep the new descriptor alive while LVGL references it.
                image_descriptors[&node] = std::move(desc);
            } else {
                lv_image_set_src(obj, node.source.c_str());
                image_descriptors.erase(&node);
            }
            break;
        case WidgetType::kSlider:
            lv_slider_set_range(obj, node.minimum, node.maximum);
            lv_slider_set_value(obj, node.value, LV_ANIM_OFF);
            break;
        case WidgetType::kSwitch:
            if (node.checked) lv_obj_add_state(obj, LV_STATE_CHECKED);
            else lv_obj_remove_state(obj, LV_STATE_CHECKED);
            break;
        default:
            break;
        }
    }

    lv_obj_t* MountNode(const std::shared_ptr<detail::WidgetNode>& node, lv_obj_t* parent)
    {
        if (!node || !parent) return nullptr;
        lv_obj_t* obj = nullptr;
        switch (node->type) {
        case WidgetType::kColumn:
        case WidgetType::kRow:
        case WidgetType::kContainer:
            obj = lv_obj_create(parent);
            lv_obj_set_flex_flow(obj, node->type == WidgetType::kRow
                                      ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
            // Layout nodes should fill the available horizontal space while
            // growing vertically with their children. LVGL's default object
            // size (100x50) would otherwise clip nested Columns and give
            // each one its own tiny scrollable viewport. Explicit Style
            // dimensions are applied by RefreshNode below and take priority.
            lv_obj_set_width(obj, LV_PCT(100));
            lv_obj_set_height(obj, LV_SIZE_CONTENT);
            break;
        case WidgetType::kLabel:
            obj = lv_label_create(parent);
            break;
        case WidgetType::kButton: {
            obj = lv_button_create(parent);
            lv_obj_t* label = lv_label_create(obj);
            lv_obj_center(label);
            lv_obj_add_event_cb(obj, WidgetEvent, LV_EVENT_CLICKED, node.get());
            break;
        }
        case WidgetType::kImage:
            obj = lv_image_create(parent);
            break;
        case WidgetType::kSlider:
            obj = lv_slider_create(parent);
            lv_obj_add_event_cb(obj, WidgetEvent, LV_EVENT_VALUE_CHANGED, node.get());
            break;
        case WidgetType::kSwitch:
            obj = lv_switch_create(parent);
            lv_obj_add_event_cb(obj, WidgetEvent, LV_EVENT_VALUE_CHANGED, node.get());
            break;
        }
        if (!obj) return nullptr;
        node->native = obj;
        node->refresh = [this](detail::WidgetNode& changed) { RefreshNode(changed); };
        RefreshNode(*node);
        for (const auto& child : node->children) {
            if (!MountNode(child, obj)) return nullptr;
        }
        return obj;
    }

    void UnmountNode(const std::shared_ptr<detail::WidgetNode>& node)
    {
        if (!node) return;
        for (const auto& child : node->children) UnmountNode(child);
        node->refresh = {};
        node->native = nullptr;
    }
};

LvglBackend::LvglBackend() : impl_(std::make_unique<Impl>()) {}
LvglBackend::~LvglBackend() { Shutdown(); }

Status LvglBackend::Initialize(Presentation& presentation)
{
    if (impl_->display) {
        return Status::Error(StatusCode::Common_AlreadyExists,
                             "LVGL backend is already initialized");
    }
    if (!presentation.IsReady()) {
        return Status::Error(StatusCode::Common_InvalidState,
                             "Presentation must be ready before LVGL backend initialization");
    }
    const auto info = presentation.FrameInfo();
    if (info.width == 0 || info.height == 0 || info.pixel_format != PixelFormat::kBGRA8888 ||
        info.stride < info.width * kBytesPerPixel) {
        return Status::Error(StatusCode::Common_Unsupported,
                             "LVGL backend requires a valid BGRA8888 AppView surface");
    }

    lv_init();
    lv_tick_set_cb(LvglTickMs);
    impl_->lvgl_initialized = true;
    impl_->presentation = &presentation;
    impl_->foreground = presentation.IsVisible();
    impl_->draw_buffer.assign(static_cast<size_t>(info.stride) *
                              std::min<uint32_t>(info.height, kPartialDrawLines), 0);

    impl_->display = lv_display_create(static_cast<int32_t>(info.width), static_cast<int32_t>(info.height));
    if (!impl_->display) {
        Shutdown();
        return Status::Error(StatusCode::Common_InternalError, "LVGL display creation failed");
    }
    lv_display_set_user_data(impl_->display, impl_.get());
    lv_display_set_color_format(impl_->display, LV_COLOR_FORMAT_ARGB8888);
    lv_display_set_flush_cb(impl_->display, Impl::FlushCallback);
    lv_display_set_buffers(impl_->display, impl_->draw_buffer.data(), nullptr,
                           static_cast<uint32_t>(impl_->draw_buffer.size()),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_default(impl_->display);

    impl_->pointer_device = lv_indev_create();
    if (!impl_->pointer_device) {
        Shutdown();
        return Status::Error(StatusCode::Common_InternalError, "LVGL pointer creation failed");
    }
    lv_indev_set_type(impl_->pointer_device, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(impl_->pointer_device, impl_.get());
    lv_indev_set_read_cb(impl_->pointer_device, Impl::ReadPointerCallback);
    lv_indev_set_display(impl_->pointer_device, impl_->display);

    impl_->touch_observer_id = presentation.RuntimeAddTouchObserver([this](const TouchFrame& frame) {
        if (impl_) impl_->OnTouch(frame);
    });
    impl_->visibility_observer_id = presentation.RuntimeAddVisibilityObserver([this](bool visible) {
        if (impl_) impl_->OnVisibility(visible);
    });
    impl_->last_status = Status::Ok();
    return Status::Ok();
}

void LvglBackend::Shutdown() noexcept
{
    if (!impl_) return;
    if (impl_->presentation) {
        impl_->presentation->RuntimeRemoveTouchObserver(impl_->touch_observer_id);
        impl_->presentation->RuntimeRemoveVisibilityObserver(impl_->visibility_observer_id);
    }
    impl_->touch_observer_id = 0;
    impl_->visibility_observer_id = 0;
    if (impl_->frame.IsValid()) (void)impl_->frame.Discard();
    ClearContent();
    if (impl_->pointer_device) {
        lv_indev_delete(impl_->pointer_device);
        impl_->pointer_device = nullptr;
    }
    if (impl_->display) {
        lv_display_delete(impl_->display);
        impl_->display = nullptr;
    }
    impl_->draw_buffer.clear();
    impl_->pointer_queue.clear();
    impl_->presentation = nullptr;
    impl_->foreground = false;
    if (impl_->lvgl_initialized) {
        lv_deinit();
        impl_->lvgl_initialized = false;
    }
}

bool LvglBackend::IsReady() const noexcept { return impl_ && impl_->display; }
bool LvglBackend::IsForeground() const noexcept { return impl_ && impl_->foreground; }
Status LvglBackend::LastStatus() const { return impl_ ? impl_->last_status : Status::Error(StatusCode::Common_InvalidState, "LVGL backend is unavailable"); }

void LvglBackend::Process()
{
    if (!impl_ || !impl_->display) return;
    lv_display_set_default(impl_->display);
    (void)lv_timer_handler();
}

Status LvglBackend::SetContent(const Widget& root)
{
    if (!impl_ || !impl_->display || !root.node_) {
        return Status::Error(StatusCode::Common_InvalidState,
                             "LVGL backend or Widget root is not ready");
    }
    ClearContent();
    lv_display_set_default(impl_->display);
    lv_obj_t* screen = lv_screen_active();
    if (!screen) return Status::Error(StatusCode::Common_InternalError, "LVGL active screen is unavailable");
    lv_obj_clean(screen);
    impl_->mounted_root = root;
    lv_obj_t* mounted = impl_->MountNode(root.node_, screen);
    if (!mounted) {
        ClearContent();
        return Status::Error(StatusCode::Common_InternalError, "Failed to mount Widget tree");
    }
    // The root widget is the page viewport. Use the display's resolved pixel
    // dimensions instead of percentage sizing: percentage coordinates depend
    // on LVGL's parent-layout resolution and can leave a newly mounted flex
    // root at its content-sized dimensions on the first layout pass.
    const int32_t display_width = lv_display_get_horizontal_resolution(impl_->display);
    const int32_t display_height = lv_display_get_vertical_resolution(impl_->display);
    if (display_width <= 0 || display_height <= 0) {
        ClearContent();
        return Status::Error(StatusCode::Common_InvalidState,
                             "LVGL display resolution is invalid");
    }
    lv_obj_set_size(mounted, display_width, display_height);
    lv_obj_set_pos(mounted, 0, 0);
    lv_obj_update_layout(screen);
    return Status::Ok();
}

void LvglBackend::ClearContent()
{
    if (!impl_) return;
    if (impl_->mounted_root.node_) impl_->UnmountNode(impl_->mounted_root.node_);
    impl_->mounted_root = Widget{};
    if (impl_->display) {
        lv_display_set_default(impl_->display);
        if (lv_obj_t* screen = lv_screen_active()) lv_obj_clean(screen);
    }
    // Release descriptors after the image widgets are destroyed.
    impl_->image_descriptors.clear();
}

lv_display_t* LvglBackend::Display() const noexcept { return impl_ ? impl_->display : nullptr; }
lv_obj_t* LvglBackend::Screen() const noexcept
{
    if (!impl_ || !impl_->display) return nullptr;
    lv_display_set_default(impl_->display);
    return lv_screen_active();
}
void LvglBackend::MakeDefault() { if (impl_ && impl_->display) lv_display_set_default(impl_->display); }

}  // namespace kavio::ui
