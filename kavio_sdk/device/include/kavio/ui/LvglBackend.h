#pragma once

/** LVGL renderer for a retained Widget tree. Drive it from the UI thread. */

#include <cstdint>
#include <memory>

#include <kavio/Status.h>
#include <kavio/presentation/Presentation.h>
#include <kavio/ui/UiApi.h>
#include <kavio/ui/Widget.h>

#include <lvgl.h>

namespace kavio::ui {

#if defined(_WIN32)
#  if defined(KAVIO_UI_LVGL_BUILD)
#    define KAVIO_UI_LVGL_API __declspec(dllexport)
#  else
#    define KAVIO_UI_LVGL_API __declspec(dllimport)
#  endif
#else
#  define KAVIO_UI_LVGL_API __attribute__((visibility("default")))
#endif

/**
 * @brief LVGL Widget backend for a Kavio AppView Presentation.
 *
 * The backend does not create a Device Session or AppView. It consumes the
 * Presentation already owned by AppRuntime and renders LVGL partial flushes
 * directly into Presentation FrameLease memory.
 */
class KAVIO_UI_LVGL_API LvglBackend {
public:
    LvglBackend();
    ~LvglBackend();

    LvglBackend(const LvglBackend&) = delete;
    LvglBackend& operator=(const LvglBackend&) = delete;

    Status Initialize(Presentation& presentation);
    void Shutdown() noexcept;

    bool IsReady() const noexcept;
    bool IsForeground() const noexcept;
    Status LastStatus() const;

    /** Drive LVGL timers/rendering. Call from the App run thread. */
    void Process();

    /** Mount a retained Widget tree onto the active LVGL screen. */
    Status SetContent(const Widget& root);
    void ClearContent();

    /** C++ native extension point for specialized controls/canvas usage. */
    lv_display_t* Display() const noexcept;
    lv_obj_t* Screen() const noexcept;
    void MakeDefault();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kavio::ui
