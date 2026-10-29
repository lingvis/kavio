#pragma once

#include <cstdint>

namespace kavio {

/**
 * @brief Rectangle in an AppView BGRA canvas. Coordinates are top-left based;
 * width and height are positive pixel counts. Present() clips rectangles to the canvas.
 */
struct AppViewRect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

} // namespace kavio
