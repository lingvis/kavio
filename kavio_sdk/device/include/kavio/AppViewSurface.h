#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include <kavio/AppViewRect.h>
#include <kavio/Status.h>

namespace kavio {

/**
 * @brief Full-screen AppView canvas owned by the current App IPC Session.
 *
 * This surface is independent from TX Overlay surfaces. It is a persistent
 * BGRA8888 canvas presented to the device's secondary display. The caller must
 * bracket pixel writes with BeginWrite() and Present() so Platform never reads
 * the canvas while it is being modified.
 */
class AppViewSurface {
public:
    AppViewSurface();
    ~AppViewSurface();

    AppViewSurface(AppViewSurface&& other) noexcept;
    AppViewSurface& operator=(AppViewSurface&& other) noexcept;

    AppViewSurface(const AppViewSurface&) = delete;
    AppViewSurface& operator=(const AppViewSurface&) = delete;

    bool IsValid() const noexcept;
    uint32_t id() const noexcept;
    uint32_t width() const noexcept;
    uint32_t height() const noexcept;
    uint32_t stride() const noexcept;

    /** Lock the shared canvas for a write operation. */
    Status BeginWrite();

    /**
     * Get the BGRA8888 canvas pointer.
     * Only write to this pointer between BeginWrite() and Present().
     */
    uint8_t* GetBuffer() noexcept;

    /**
     * Publish the updated canvas and notify Platform; does not wait for rendering.
     * This overload marks the whole canvas dirty and is intended for first frames,
     * recovery, or callers that do not track dirty rectangles.
     */
    Status Present();

    /**
     * Publish the updated canvas and append dirty rectangles to the pending region set.
     * Regions remain pending until Platform consumes them, so several fast Present()
     * calls cannot lose earlier updates. An empty list requests a full-canvas update.
     */
    Status Present(const AppViewRect* dirty_rects, size_t dirty_rect_count);

    /** Close this process's mapping. Platform removes the surface when the Session closes. */
    void Close() noexcept;

private:
    friend class KavioClient;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace kavio
