#pragma once

/** Display presentation, frame acquisition, and input events. */

#include <cstdint>
#include <functional>
#include <memory>

#include <kavio/AppApi.h>
#include <kavio/Status.h>
#include <kavio/presentation/FrameBuffer.h>
#include <kavio/presentation/InputEvent.h>

namespace kavio {
namespace detail {
class PresentationAccess;
}
namespace ui { class LvglBackend; }

/**
 * @brief Full-screen App presentation owned by Kavio App Runtime.
 *
 * Presentation is always backed by the AppView surface. It does not require a
 * widget framework: custom renderers can acquire the BGRA8888 frame buffer and
 * draw directly into SDK-owned memory.
 */
class KAVIO_APP_API Presentation {
public:
    using TouchHandler = std::function<void(const TouchFrame&)>;
    using VisibilityHandler = std::function<void(bool)>;

    Presentation();
    ~Presentation();

    Presentation(Presentation&&) noexcept;
    Presentation& operator=(Presentation&&) noexcept;

    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;

    bool IsReady() const noexcept;
    bool IsVisible() const noexcept;
    uint64_t Generation() const noexcept;
    FrameBufferInfo FrameInfo() const noexcept;

    Status AcquireFrame(FrameLease& out_frame);

    /** Fill the current AppView canvas with one BGRA color and publish it. */
    Status Clear(uint8_t blue, uint8_t green, uint8_t red, uint8_t alpha = 255);

    /** Handlers execute on the App run thread, never on the SDK IPC dispatch thread. */
    void SetTouchHandler(TouchHandler handler);
    void SetVisibilityHandler(VisibilityHandler handler);

private:
    friend class detail::PresentationAccess;
    friend class ui::LvglBackend;

    using ObserverId = uint64_t;
    ObserverId RuntimeAddTouchObserver(TouchHandler handler);
    ObserverId RuntimeAddVisibilityObserver(VisibilityHandler handler);
    void RuntimeRemoveTouchObserver(ObserverId id) noexcept;
    void RuntimeRemoveVisibilityObserver(ObserverId id) noexcept;


    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kavio
