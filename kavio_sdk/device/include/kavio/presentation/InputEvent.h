#pragma once

/** Input events delivered to presentation subscribers. */

#include <cstdint>
#include <vector>

namespace kavio {

struct TouchPoint {
    int32_t slot = 0;
    int32_t tracking_id = -1;
    int32_t x = 0;
    int32_t y = 0;
    int32_t pressure = 0;
    int32_t touch_major = 0;
    bool active = false;
    bool changed = false;
};

struct TouchFrame {
    uint32_t surface_id = 0;
    uint64_t sequence = 0;
    std::vector<TouchPoint> points;
    bool cancelled = false;
};

}  // namespace kavio
