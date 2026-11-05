#include <kavio/ui/Layout.h>
#include <kavio/ui/Widgets.h>

#include "ui/WidgetInternal.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace kavio::ui {

Widget::Widget() = default;
Widget::Widget(WidgetType type) : node_(std::make_shared<detail::WidgetNode>(type)) {}
Widget::~Widget() = default;

bool Widget::IsValid() const noexcept { return static_cast<bool>(node_); }
WidgetType Widget::Type() const noexcept { return node_ ? node_->type : WidgetType::kContainer; }

Widget& Widget::SetStyle(const Style& style)
{
    if (node_) {
        node_->style = style;
        detail::Refresh(*node_);
    }
    return *this;
}

const Style& Widget::GetStyle() const
{
    static const Style empty;
    return node_ ? node_->style : empty;
}

Widget& Widget::Add(const Widget& child)
{
    if (node_ && child.node_) {
        node_->children.push_back(child.node_);
        detail::Refresh(*node_);
    }
    return *this;
}

Page::Page() : Widget(WidgetType::kContainer) {}
Column::Column() : Widget(WidgetType::kColumn) {}
Row::Row() : Widget(WidgetType::kRow) {}

Label::Label(std::string text) : Widget(WidgetType::kLabel)
{
    node_->text = std::move(text);
}
Label& Label::SetText(std::string text)
{
    node_->text = std::move(text);
    detail::Refresh(*node_);
    return *this;
}
std::string Label::Text() const { return node_ ? node_->text : std::string{}; }

Button::Button(std::string text) : Widget(WidgetType::kButton)
{
    node_->text = std::move(text);
}
Button& Button::SetText(std::string text)
{
    node_->text = std::move(text);
    detail::Refresh(*node_);
    return *this;
}
Button& Button::OnClick(std::function<void()> handler)
{
    node_->on_click = std::move(handler);
    return *this;
}

Image::Image(std::string source) : Widget(WidgetType::kImage)
{
    node_->source = std::move(source);
}
ImageBufferView Image::BufferView() const noexcept
{
    ImageBufferView view;
    if (node_ && !node_->rgb565.empty() && node_->image_width && node_->image_height) {
        view.data = node_->rgb565.data();
        view.width = node_->image_width;
        view.height = node_->image_height;
    }
    return view;
}

Image& Image::SetSource(std::string source)
{
    if (!node_) return *this;
    // Keep the previous pixel storage alive until LVGL stops using it.
    auto previous_pixels = std::move(node_->rgb565);
    node_->source = std::move(source);
    node_->image_width = 0;
    node_->image_height = 0;
    detail::Refresh(*node_);
    return *this;
}

namespace {
uint8_t ClampRgb(int value)
{
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}
uint16_t Pack565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}
uint16_t ConvertYuv(uint8_t y, uint8_t u, uint8_t v)
{
    const int c = std::max(0, static_cast<int>(y) - 16);
    const int d = static_cast<int>(u) - 128;
    const int e = static_cast<int>(v) - 128;
    return Pack565(ClampRgb((298*c + 409*e + 128) >> 8),
                   ClampRgb((298*c - 100*d - 208*e + 128) >> 8),
                   ClampRgb((298*c + 516*d + 128) >> 8));
}
}

bool Image::SetBuffer(const uint8_t* bytes, uint64_t size, uint32_t width,
                            uint32_t height, uint32_t stride, PixelFormat pixel_format,
                            uint32_t max_width, uint32_t max_height)
{
    if (!node_ || !bytes || !width || !height || !max_width || !max_height ||
        max_width > 2048 || max_height > 2048 || width > 16384 || height > 16384 ||
        (pixel_format != PixelFormat::kUYVY && pixel_format != PixelFormat::kYUYV &&
         pixel_format != PixelFormat::kBGRA8888)) return false;
    const uint64_t bytes_per_pixel = pixel_format == PixelFormat::kBGRA8888 ? 4u : 2u;
    const uint64_t min_stride = pixel_format == PixelFormat::kBGRA8888
        ? static_cast<uint64_t>(width) * 4u
        : static_cast<uint64_t>((width + 1u) / 2u) * 4u;
    if (stride < min_stride || static_cast<uint64_t>(stride) * height > size) return false;

    const double scale = std::min({1.0, static_cast<double>(max_width) / width,
                                  static_cast<double>(max_height) / height});
    const uint32_t dst_w = std::max(1u, static_cast<uint32_t>(width * scale));
    const uint32_t dst_h = std::max(1u, static_cast<uint32_t>(height * scale));
    std::vector<uint16_t> converted(static_cast<size_t>(dst_w) * dst_h);
    for (uint32_t y = 0; y < dst_h; ++y) {
        const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(y) * height / dst_h);
        const uint8_t* row = bytes + static_cast<size_t>(sy) * stride;
        for (uint32_t x = 0; x < dst_w; ++x) {
            const uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(x) * width / dst_w);
            uint16_t pixel;
            if (pixel_format == PixelFormat::kBGRA8888) { // KAVIO_PIXEL_FORMAT_BGRA8888
                const uint8_t* src = row + static_cast<size_t>(sx) * 4u;
                pixel = Pack565(src[2], src[1], src[0]);
            } else {
                const uint8_t* pair = row + static_cast<size_t>(sx & ~1u) * bytes_per_pixel;
                const bool odd = (sx & 1u) != 0;
                const uint8_t yy = pixel_format == PixelFormat::kUYVY ? pair[odd ? 3 : 1] : pair[odd ? 2 : 0];
                const uint8_t uu = pixel_format == PixelFormat::kUYVY ? pair[0] : pair[1];
                const uint8_t vv = pixel_format == PixelFormat::kUYVY ? pair[2] : pair[3];
                pixel = ConvertYuv(yy, uu, vv);
            }
            converted[static_cast<size_t>(y) * dst_w + x] = pixel;
        }
    }
    // LVGL may still refer to the old pixels until Refresh switches its source.
    auto previous_pixels = std::move(node_->rgb565);
    node_->rgb565 = std::move(converted);
    node_->image_width = dst_w;
    node_->image_height = dst_h;
    node_->source.clear();
    detail::Refresh(*node_);
    return true;
}

Slider::Slider(int32_t minimum, int32_t maximum, int32_t value) : Widget(WidgetType::kSlider)
{
    node_->minimum = std::min(minimum, maximum);
    node_->maximum = std::max(minimum, maximum);
    node_->value = std::clamp(value, node_->minimum, node_->maximum);
}
Slider& Slider::SetRange(int32_t minimum, int32_t maximum)
{
    node_->minimum = std::min(minimum, maximum);
    node_->maximum = std::max(minimum, maximum);
    node_->value = std::clamp(node_->value, node_->minimum, node_->maximum);
    detail::Refresh(*node_);
    return *this;
}
Slider& Slider::SetValue(int32_t value)
{
    node_->value = std::clamp(value, node_->minimum, node_->maximum);
    detail::Refresh(*node_);
    return *this;
}
Slider& Slider::OnChange(std::function<void(int32_t)> handler)
{
    node_->on_value = std::move(handler);
    return *this;
}
int32_t Slider::Value() const { return node_ ? node_->value : 0; }

Switch::Switch(bool checked) : Widget(WidgetType::kSwitch)
{
    node_->checked = checked;
}
Switch& Switch::SetChecked(bool checked)
{
    node_->checked = checked;
    detail::Refresh(*node_);
    return *this;
}
Switch& Switch::OnChange(std::function<void(bool)> handler)
{
    node_->on_checked = std::move(handler);
    return *this;
}
bool Switch::Checked() const { return node_ && node_->checked; }

}  // namespace kavio::ui
