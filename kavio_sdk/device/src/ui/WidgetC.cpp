#include <kavio_c/ui.h>

#include "ui/UiCInternal.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>
#include <string>

namespace {
thread_local std::string g_ui_error;

KavioStatusCode UiError(KavioStatusCode code, const char* message)
{
    g_ui_error = message ? message : "UI operation failed";
    return code;
}

bool ValidStyle(const KavioUiStyle* style)
{
    return style && style->abi_version == KAVIO_ABI_VERSION && style->struct_size >= sizeof(KavioUiStyle);
}
}

extern "C" {

const char* kavio_ui_last_error_message(void)
{
    return g_ui_error.c_str();
}

void kavio_ui_style_init(KavioUiStyle* style)
{
    g_ui_error.clear();
    if (!style) return;
    *style = KavioUiStyle{sizeof(KavioUiStyle), KAVIO_ABI_VERSION,
                          -1, -1, -1, -1, -1, -1,
                          0u, 0u, 255u, 0u, 0u, 0u};
}

KavioStatusCode kavio_widget_create(
    KavioWidgetType type, const char* text_or_source,
    int32_t minimum, int32_t maximum, int32_t value,
    uint8_t checked, KavioWidget** out_widget)
{
    g_ui_error.clear();
    if (!out_widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "out_widget is null");
    *out_widget = nullptr;
    try {
        const std::string text = text_or_source ? text_or_source : "";
        std::unique_ptr<KavioWidget> result;
        switch (type) {
        case KAVIO_WIDGET_PAGE: result = std::make_unique<KavioWidget>(kavio::ui::Page{}); break;
        case KAVIO_WIDGET_COLUMN: result = std::make_unique<KavioWidget>(kavio::ui::Column{}); break;
        case KAVIO_WIDGET_ROW: result = std::make_unique<KavioWidget>(kavio::ui::Row{}); break;
        case KAVIO_WIDGET_LABEL: result = std::make_unique<KavioWidget>(kavio::ui::Label{text}); break;
        case KAVIO_WIDGET_BUTTON: result = std::make_unique<KavioWidget>(kavio::ui::Button{text}); break;
        case KAVIO_WIDGET_IMAGE: result = std::make_unique<KavioWidget>(kavio::ui::Image{text}); break;
        case KAVIO_WIDGET_SLIDER:
            result = std::make_unique<KavioWidget>(kavio::ui::Slider{minimum, maximum, value});
            break;
        case KAVIO_WIDGET_SWITCH:
            result = std::make_unique<KavioWidget>(kavio::ui::Switch{checked != 0});
            break;
        default:
            return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "unsupported Widget type");
        }
        *out_widget = result.release();
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return UiError(KAVIO_STATUS_COMMON_INTERNAL_ERROR, exc.what());
    } catch (...) {
        return UiError(KAVIO_STATUS_COMMON_INTERNAL_ERROR, "kavio_widget_create failed");
    }
}

void kavio_widget_destroy(KavioWidget* widget)
{
    g_ui_error.clear();
    delete widget;
}

KavioStatusCode kavio_widget_add(KavioWidget* parent, KavioWidget* child)
{
    g_ui_error.clear();
    if (!parent || !child) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "parent or child is null");
    KavioWidgetBase(*parent).Add(KavioWidgetBase(*child));
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_style(KavioWidget* widget, const KavioUiStyle* style)
{
    g_ui_error.clear();
    if (!widget || !ValidStyle(style)) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget or style is invalid");
    kavio::ui::Style value;
    value.width = style->width;
    value.height = style->height;
    value.padding = style->padding;
    value.gap = style->gap;
    value.radius = style->radius;
    value.font_size = style->font_size;
    value.has_background = style->has_background != 0;
    value.background = style->background;
    value.has_text_color = style->has_text_color != 0;
    value.text_color = style->text_color;
    value.opacity = style->opacity;
    KavioWidgetBase(*widget).SetStyle(value);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_text(KavioWidget* widget, const char* text)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    if (auto* label = std::get_if<kavio::ui::Label>(&widget->value)) {
        label->SetText(text ? text : "");
        return KAVIO_STATUS_OK;
    }
    if (auto* button = std::get_if<kavio::ui::Button>(&widget->value)) {
        button->SetText(text ? text : "");
        return KAVIO_STATUS_OK;
    }
    return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget does not support text");
}

KavioStatusCode kavio_widget_set_source(KavioWidget* widget, const char* source)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* image = std::get_if<kavio::ui::Image>(&widget->value);
    if (!image) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not an Image");
    image->SetSource(source ? source : "");
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_image_buffer(
    KavioWidget* widget, const uint8_t* bytes, uint64_t size,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t pixel_format,
    uint32_t max_width, uint32_t max_height)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* image = std::get_if<kavio::ui::Image>(&widget->value);
    if (!image) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not an Image");
    if (!image->SetBuffer(bytes, size, width, height, stride,
                          static_cast<kavio::PixelFormat>(pixel_format),
                          max_width, max_height))
        return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "invalid image buffer or pixel format");
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_range(KavioWidget* widget, int32_t minimum, int32_t maximum)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* slider = std::get_if<kavio::ui::Slider>(&widget->value);
    if (!slider) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Slider");
    slider->SetRange(minimum, maximum);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_value(KavioWidget* widget, int32_t value)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* slider = std::get_if<kavio::ui::Slider>(&widget->value);
    if (!slider) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Slider");
    slider->SetValue(value);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_get_value(KavioWidget* widget, int32_t* out_value)
{
    g_ui_error.clear();
    if (!widget || !out_value) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget or out_value is null");
    auto* slider = std::get_if<kavio::ui::Slider>(&widget->value);
    if (!slider) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Slider");
    *out_value = slider->Value();
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_checked(KavioWidget* widget, uint8_t checked)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* sw = std::get_if<kavio::ui::Switch>(&widget->value);
    if (!sw) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Switch");
    sw->SetChecked(checked != 0);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_get_checked(KavioWidget* widget, uint8_t* out_checked)
{
    g_ui_error.clear();
    if (!widget || !out_checked) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget or out_checked is null");
    auto* sw = std::get_if<kavio::ui::Switch>(&widget->value);
    if (!sw) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Switch");
    *out_checked = sw->Checked() ? 1u : 0u;
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_click_callback(
    KavioWidget* widget, KavioWidgetClickCallback callback, void* user_data)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* button = std::get_if<kavio::ui::Button>(&widget->value);
    if (!button) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Button");
    button->OnClick(callback ? [callback, user_data]() { callback(user_data); }
                             : std::function<void()>{});
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_value_callback(
    KavioWidget* widget, KavioWidgetValueCallback callback, void* user_data)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* slider = std::get_if<kavio::ui::Slider>(&widget->value);
    if (!slider) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Slider");
    slider->OnChange(callback ? [callback, user_data](int32_t value) { callback(value, user_data); }
                              : std::function<void(int32_t)>{});
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_widget_set_checked_callback(
    KavioWidget* widget, KavioWidgetCheckedCallback callback, void* user_data)
{
    g_ui_error.clear();
    if (!widget) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "widget is null");
    auto* sw = std::get_if<kavio::ui::Switch>(&widget->value);
    if (!sw) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "Widget is not a Switch");
    sw->OnChange(callback ? [callback, user_data](bool value) { callback(value ? 1u : 0u, user_data); }
                          : std::function<void(bool)>{});
    return KAVIO_STATUS_OK;
}

}  // extern "C"
