#ifndef KAVIO_C_UI_H_
#define KAVIO_C_UI_H_

/** C ABI for retained UI widgets, appearance, and image input. */

#include <kavio_c/common.h>
#include <kavio_c/presentation.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#  define KAVIO_UI_C_API
#else
#  define KAVIO_UI_C_API __attribute__((visibility("default")))
#endif

/** Opaque handles. Destroy owned widgets/backends with their matching function. */
typedef struct KavioWidget KavioWidget;
typedef struct KavioUiBackend KavioUiBackend;

typedef enum KavioWidgetType {
    KAVIO_WIDGET_PAGE = 0,
    KAVIO_WIDGET_COLUMN = 1,
    KAVIO_WIDGET_ROW = 2,
    KAVIO_WIDGET_LABEL = 3,
    KAVIO_WIDGET_BUTTON = 4,
    KAVIO_WIDGET_IMAGE = 5,
    KAVIO_WIDGET_SLIDER = 6,
    KAVIO_WIDGET_SWITCH = 7
} KavioWidgetType;

/** UI appearance; initialize with kavio_ui_style_init before writing fields. */
typedef struct KavioUiStyle {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t width;
    int32_t height;
    int32_t padding;
    int32_t gap;
    int32_t radius;
    int32_t font_size;
    uint8_t has_background;
    uint8_t has_text_color;
    uint8_t opacity;
    uint8_t reserved;
    uint32_t background;
    uint32_t text_color;
} KavioUiStyle;

typedef void (*KavioWidgetClickCallback)(void* user_data);
typedef void (*KavioWidgetValueCallback)(int32_t value, void* user_data);
typedef void (*KavioWidgetCheckedCallback)(uint8_t checked, void* user_data);

KAVIO_UI_C_API const char* kavio_ui_last_error_message(void);
KAVIO_UI_C_API const char* kavio_ui_backend_last_error_message(void);
KAVIO_UI_C_API void kavio_ui_style_init(KavioUiStyle* style);

/** Creates one widget. out_widget receives an owned handle on success. */
KAVIO_UI_C_API KavioStatusCode kavio_widget_create(
    KavioWidgetType type, const char* text_or_source,
    int32_t minimum, int32_t maximum, int32_t value,
    uint8_t checked, KavioWidget** out_widget);
KAVIO_UI_C_API void kavio_widget_destroy(KavioWidget* widget);
KAVIO_UI_C_API KavioStatusCode kavio_widget_add(KavioWidget* parent, KavioWidget* child);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_style(KavioWidget* widget, const KavioUiStyle* style);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_text(KavioWidget* widget, const char* text);
/** Replaces the Image content with a source string. */
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_source(KavioWidget* widget, const char* source);
/** Copies packed pixels into an Image widget (the input remains caller-owned).
 * Supports KAVIO_PIXEL_FORMAT_UYVY, _YUYV and _BGRA8888. The image is
 * downscaled to fit max_width/max_height without changing its aspect ratio.
 * A valid buffer must contain at least stride * height bytes.
 * Call this and all other Widget setters on the UI thread.
 */
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_image_buffer(
    KavioWidget* widget, const uint8_t* bytes, uint64_t size,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t pixel_format,
    uint32_t max_width, uint32_t max_height);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_range(KavioWidget* widget, int32_t minimum, int32_t maximum);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_value(KavioWidget* widget, int32_t value);
KAVIO_UI_C_API KavioStatusCode kavio_widget_get_value(KavioWidget* widget, int32_t* out_value);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_checked(KavioWidget* widget, uint8_t checked);
KAVIO_UI_C_API KavioStatusCode kavio_widget_get_checked(KavioWidget* widget, uint8_t* out_checked);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_click_callback(
    KavioWidget* widget, KavioWidgetClickCallback callback, void* user_data);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_value_callback(
    KavioWidget* widget, KavioWidgetValueCallback callback, void* user_data);
KAVIO_UI_C_API KavioStatusCode kavio_widget_set_checked_callback(
    KavioWidget* widget, KavioWidgetCheckedCallback callback, void* user_data);

KAVIO_UI_C_API KavioStatusCode kavio_ui_backend_create(KavioUiBackend** out_backend);
KAVIO_UI_C_API void kavio_ui_backend_destroy(KavioUiBackend* backend);
/** Initializes rendering for a ready presentation. */
KAVIO_UI_C_API KavioStatusCode kavio_ui_backend_initialize(
    KavioUiBackend* backend, KavioPresentation* presentation);
KAVIO_UI_C_API void kavio_ui_backend_shutdown(KavioUiBackend* backend);
KAVIO_UI_C_API uint8_t kavio_ui_backend_is_ready(KavioUiBackend* backend);
KAVIO_UI_C_API uint8_t kavio_ui_backend_is_foreground(KavioUiBackend* backend);
KAVIO_UI_C_API KavioStatusCode kavio_ui_backend_process(KavioUiBackend* backend);
KAVIO_UI_C_API KavioStatusCode kavio_ui_backend_set_content(
    KavioUiBackend* backend, KavioWidget* root);
KAVIO_UI_C_API void kavio_ui_backend_clear_content(KavioUiBackend* backend);

#ifdef __cplusplus
}
#endif

#endif  // KAVIO_C_UI_H_
