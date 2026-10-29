#pragma once

#include <cstddef>
#include <cstdint>
#include <pthread.h>

#include <kavio/AppViewRect.h>

namespace kavio::detail {

constexpr uint32_t kAppViewShmMagic = 0x4B415653u; // "KAVS"
constexpr uint32_t kAppViewShmVersion = 2;
constexpr uint32_t kAppViewPixelFormatBgra8888 = 3;
constexpr uint32_t kAppViewMaxDirtyRects = 32;

/**
 * Shared ABI for the full-screen AppView canvas.
 * This layout is intentionally separate from FrameShm/TX Overlay surfaces.
 * The process-shared robust mutex protects canvas writes, dirty-region metadata,
 * and Platform reads. Dirty regions accumulate until Platform consumes a frame.
 */
struct AppViewSharedHeader {
    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t header_size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t pixel_format = 0;
    uint32_t reserved0 = 0;
    uint64_t payload_size = 0;
    uint64_t frame_sequence = 0;
    uint32_t dirty_rect_count = 0;
    uint32_t full_dirty = 0;
    AppViewRect dirty_rects[kAppViewMaxDirtyRects]{};
    pthread_mutex_t mutex{};
};

} // namespace kavio::detail
