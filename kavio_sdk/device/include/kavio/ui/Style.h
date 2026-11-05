#pragma once

/** Optional widget appearance properties; negative dimensions mean unspecified. */

#include <cstdint>

namespace kavio::ui {

/** Appearance settings. Set only the fields you need; negative sizes mean
 * "use the widget default" and colors apply only when their flags are set.
 */
struct Style {
    int32_t width = -1;
    int32_t height = -1;
    int32_t padding = -1;
    int32_t gap = -1;
    int32_t radius = -1;
    int32_t font_size = -1;

    bool has_background = false;
    uint32_t background = 0;
    bool has_text_color = false;
    uint32_t text_color = 0;
    uint8_t opacity = 255;
};

}  // namespace kavio::ui
