#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include <kavio/detail/AppViewSurfaceShared.h>

namespace kavio::detail {

inline uint64_t AppViewRectPixels(const AppViewRect& rect)
{
    return rect.width > 0 && rect.height > 0
        ? static_cast<uint64_t>(rect.width) * static_cast<uint64_t>(rect.height)
        : 0;
}

inline AppViewRect AppViewBoundingRect(const AppViewRect& a, const AppViewRect& b)
{
    const int64_t left = std::min<int64_t>(a.x, b.x);
    const int64_t top = std::min<int64_t>(a.y, b.y);
    const int64_t right = std::max<int64_t>(static_cast<int64_t>(a.x) + a.width,
                                            static_cast<int64_t>(b.x) + b.width);
    const int64_t bottom = std::max<int64_t>(static_cast<int64_t>(a.y) + a.height,
                                             static_cast<int64_t>(b.y) + b.height);
    return AppViewRect{static_cast<int32_t>(left), static_cast<int32_t>(top),
                       static_cast<int32_t>(right - left), static_cast<int32_t>(bottom - top)};
}

inline bool AppViewRectsMeet(const AppViewRect& a, const AppViewRect& b)
{
    return static_cast<int64_t>(a.x) <= static_cast<int64_t>(b.x) + b.width &&
           static_cast<int64_t>(b.x) <= static_cast<int64_t>(a.x) + a.width &&
           static_cast<int64_t>(a.y) <= static_cast<int64_t>(b.y) + b.height &&
           static_cast<int64_t>(b.y) <= static_cast<int64_t>(a.y) + a.height;
}

inline uint64_t AppViewDirtyPixels(const AppViewRect* rects, uint32_t count)
{
    uint64_t result = 0;
    for (uint32_t i = 0; i < count; ++i) result += AppViewRectPixels(rects[i]);
    return result;
}

/**
 * Store a conservative set of invalid regions in at most capacity slots.
 *
 * Unlike unconditional bounding-box merging, do not merge touching regions
 * if that increases the number of pixels to copy. When capacity is exhausted,
 * merge the pair whose bounding union has the least *additional* area, rather
 * than invalidating the entire 720x720 screen.
 *
 * Returns false if a full canvas copy is no more expensive than the region set.
 * The caller then marks the whole canvas dirty. All inputs must be clipped to
 * the canvas and have positive dimensions.
 */
inline bool AppendAppViewRegion(AppViewRect* rects,
                                uint32_t& count,
                                uint32_t capacity,
                                const AppViewRect& input,
                                uint64_t canvas_pixels)
{
    if (!rects || !capacity || !canvas_pixels || count > capacity ||
        AppViewRectPixels(input) == 0) return false;

    AppViewRect merged = input;
    uint32_t index = 0;
    while (index < count) {
        const AppViewRect bound = AppViewBoundingRect(merged, rects[index]);
        // Corner contacts and L-shaped intersections can produce an enormous
        // bounding box despite touching. Merge only if copying it is cheaper.
        if (!AppViewRectsMeet(merged, rects[index]) ||
            AppViewRectPixels(bound) > AppViewRectPixels(merged) + AppViewRectPixels(rects[index])) {
            ++index;
            continue;
        }
        merged = bound;
        for (uint32_t j = index + 1; j < count; ++j) rects[j - 1] = rects[j];
        --count;
        index = 0; // Expanded bounds may now overlap an earlier region.
    }

    if (count < capacity) {
        rects[count++] = merged;
    } else {
        // Consider all pairs among the existing regions and the incoming one.
        // Avoid a full-screen fallback just because 33 isolated LVGL objects
        // generated updates when the protocol has room for only 32 rectangles.
        AppViewRect candidates[kAppViewMaxDirtyRects + 1]{};
        if (capacity > kAppViewMaxDirtyRects) return false;
        for (uint32_t i = 0; i < count; ++i) candidates[i] = rects[i];
        candidates[count] = merged;
        const uint32_t total = count + 1;
        int64_t best_excess = std::numeric_limits<int64_t>::max();
        uint32_t best_i = 0;
        uint32_t best_j = 1;
        AppViewRect best_bound{};
        for (uint32_t i = 0; i < total; ++i) {
            for (uint32_t j = i + 1; j < total; ++j) {
                const AppViewRect bound = AppViewBoundingRect(candidates[i], candidates[j]);
                const int64_t excess = static_cast<int64_t>(AppViewRectPixels(bound)) -
                    static_cast<int64_t>(AppViewRectPixels(candidates[i])) -
                    static_cast<int64_t>(AppViewRectPixels(candidates[j]));
                if (excess < best_excess) {
                    best_excess = excess;
                    best_i = i;
                    best_j = j;
                    best_bound = bound;
                }
            }
        }
        candidates[best_i] = best_bound;
        for (uint32_t j = best_j + 1; j < total; ++j) candidates[j - 1] = candidates[j];
        count = capacity;
        for (uint32_t i = 0; i < count; ++i) rects[i] = candidates[i];
    }

    return AppViewDirtyPixels(rects, count) < canvas_pixels;
}

} // namespace kavio::detail
