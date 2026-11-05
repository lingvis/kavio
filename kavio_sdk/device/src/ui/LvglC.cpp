#include <kavio_c/ui.h>

#include "c_api/KavioCInternal.h"
#include "ui/UiCInternal.h"

#include <exception>
#include <memory>
#include <new>
#include <string>

#include <kavio/ui/LvglBackend.h>

struct KavioUiBackend {
    kavio::ui::LvglBackend backend;
};

namespace {
thread_local std::string g_lvgl_c_error;

KavioStatusCode UiError(KavioStatusCode code, const char* message)
{
    g_lvgl_c_error = message ? message : "LVGL backend operation failed";
    return code;
}

KavioStatusCode UiStatus(const kavio::Status& status)
{
    if (status.ok()) {
        g_lvgl_c_error.clear();
        return KAVIO_STATUS_OK;
    }
    g_lvgl_c_error = status.message;
    return static_cast<KavioStatusCode>(status.code);
}
}

extern "C" {

const char* kavio_ui_backend_last_error_message(void)
{
    return g_lvgl_c_error.c_str();
}

KavioStatusCode kavio_ui_backend_create(KavioUiBackend** out_backend)
{
    g_lvgl_c_error.clear();
    if (!out_backend) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "out_backend is null");
    *out_backend = new (std::nothrow) KavioUiBackend();
    return *out_backend ? KAVIO_STATUS_OK
                        : UiError(KAVIO_STATUS_COMMON_INTERNAL_ERROR, "memory allocation failed");
}

void kavio_ui_backend_destroy(KavioUiBackend* backend)
{
    g_lvgl_c_error.clear();
    delete backend;
}

KavioStatusCode kavio_ui_backend_initialize(
    KavioUiBackend* backend, KavioPresentation* presentation)
{
    g_lvgl_c_error.clear();
    if (!backend || !presentation || !presentation->presentation) {
        return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "backend or presentation is invalid");
    }
    return UiStatus(backend->backend.Initialize(*presentation->presentation));
}

void kavio_ui_backend_shutdown(KavioUiBackend* backend)
{
    g_lvgl_c_error.clear();
    if (backend) backend->backend.Shutdown();
}

uint8_t kavio_ui_backend_is_ready(KavioUiBackend* backend)
{
    g_lvgl_c_error.clear();
    return backend && backend->backend.IsReady() ? 1u : 0u;
}

uint8_t kavio_ui_backend_is_foreground(KavioUiBackend* backend)
{
    g_lvgl_c_error.clear();
    return backend && backend->backend.IsForeground() ? 1u : 0u;
}

KavioStatusCode kavio_ui_backend_process(KavioUiBackend* backend)
{
    g_lvgl_c_error.clear();
    if (!backend) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "backend is null");
    backend->backend.Process();
    return UiStatus(backend->backend.LastStatus());
}

KavioStatusCode kavio_ui_backend_set_content(KavioUiBackend* backend, KavioWidget* root)
{
    g_lvgl_c_error.clear();
    if (!backend || !root) return UiError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, "backend or root is null");
    return UiStatus(backend->backend.SetContent(KavioWidgetBase(*root)));
}

void kavio_ui_backend_clear_content(KavioUiBackend* backend)
{
    g_lvgl_c_error.clear();
    if (backend) backend->backend.ClearContent();
}

}  // extern "C"
