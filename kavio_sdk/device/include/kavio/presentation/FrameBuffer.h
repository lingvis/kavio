#pragma once

/** Writable presentation frames and buffer ownership. */

#include <cstddef>
#include <cstdint>
#include <memory>

#include <kavio/AppApi.h>
#include <kavio/Status.h>
#include <kavio/Types.h>

namespace kavio {

class Presentation;

/** Pixel metadata for the current AppView frame buffer. */
struct FrameBufferInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;
    uint64_t buffer_size = 0;
    uint32_t surface_id = 0;
    uint64_t generation = 0;
};

/**
 * @brief Scoped write access to the AppView frame buffer.
 *
 * A lease is created by Presentation::AcquireFrame(). Pixel memory is writable
 * only while the lease is active. Present() publishes the update and releases
 * the underlying AppView write lock. Discard() releases the write lock without
 * publishing a frame. The destructor automatically discards an active lease.
 *
 * A lease must be acquired, written, presented/discarded and destroyed on the
 * same thread. It is movable only to support normal C++ return/value handling;
 * moving a live lease to another thread is unsupported.
 */
class KAVIO_APP_API FrameLease {
public:
    FrameLease();
    ~FrameLease();

    FrameLease(FrameLease&& other) noexcept;
    FrameLease& operator=(FrameLease&& other) noexcept;

    FrameLease(const FrameLease&) = delete;
    FrameLease& operator=(const FrameLease&) = delete;

    bool IsValid() const noexcept;
    uint8_t* Data() noexcept;
    const FrameBufferInfo& Info() const noexcept;

    /** Publish the whole frame. */
    Status Present();

    /** Publish one or more dirty regions. An empty list publishes the whole frame. */
    Status Present(const Rect* dirty_rects, size_t dirty_rect_count);

    /** Release this write transaction without publishing a frame. */
    Status Discard();

private:
    friend class Presentation;
    struct Impl;
    explicit FrameLease(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace kavio
