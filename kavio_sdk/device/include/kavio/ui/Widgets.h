#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <kavio/Types.h>
#include <kavio/ui/Widget.h>

namespace kavio::ui {

/** Text label; use SetText to update a mounted label. */
class KAVIO_UI_API Label : public Widget {
public:
    explicit Label(std::string text = {});
    Label& SetText(std::string text);
    std::string Text() const;
};

/** Clickable control with a text caption. */
class KAVIO_UI_API Button : public Widget {
public:
    explicit Button(std::string text = {});
    Button& SetText(std::string text);
    Button& OnClick(std::function<void()> handler);
};

/** Read-only RGB565 result for native image integrations.
 *
 * The data belongs to Image and becomes invalid when SetBuffer or SetSource
 * is called again or when the Image is destroyed. Do not store the pointer,
 * free the storage, or modify its contents.
 */
struct KAVIO_UI_API ImageBufferView {
    const uint16_t* data = nullptr;
    uint32_t width = 0;   ///< Converted width in pixels.
    uint32_t height = 0;  ///< Converted height in pixels.
};

/** Displays an image from a source path or a packed pixel buffer.
 *
 * Image keeps a private copy of the converted pixels. The caller may release
 * its source buffer when SetBuffer returns. Updates must run on the UI thread.
 */
class KAVIO_UI_API Image : public Widget {
public:
    explicit Image(std::string source = {});

    /** Returns the current converted RGB565 buffer without copying it.
     *
     * The view is empty when no memory image has been set. Callers using
     * native LVGL objects must rebind the pixel pointer after every SetBuffer.
     */
    ImageBufferView BufferView() const noexcept;

    /** Selects an image source; clears any previously supplied pixel buffer. */
    Image& SetSource(std::string source);

    /** Copies packed pixels, preserving aspect ratio within max_width/height.
     * Use kavio::PixelFormat::kUYVY, kYUYV or kBGRA8888.
     * Returns false for invalid dimensions, strides, or buffer sizes.
     */
    bool SetBuffer(const uint8_t* bytes, uint64_t size, uint32_t width, uint32_t height,
                   uint32_t stride, PixelFormat pixel_format,
                   uint32_t max_width = 620, uint32_t max_height = 320);
};

/** Integer value control with a configurable range and change callback. */
class KAVIO_UI_API Slider : public Widget {
public:
    Slider(int32_t minimum = 0, int32_t maximum = 100, int32_t value = 0);
    Slider& SetRange(int32_t minimum, int32_t maximum);
    Slider& SetValue(int32_t value);
    Slider& OnChange(std::function<void(int32_t)> handler);
    int32_t Value() const;
};

/** On/off control with a checked-state callback. */
class KAVIO_UI_API Switch : public Widget {
public:
    explicit Switch(bool checked = false);
    Switch& SetChecked(bool checked);
    Switch& OnChange(std::function<void(bool)> handler);
    bool Checked() const;
};

}  // namespace kavio::ui
