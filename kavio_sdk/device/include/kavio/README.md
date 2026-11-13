# C++ Device SDK API

This directory contains the public C++ headers of the Kavio Device SDK.

## Where to start

- `App.h`: application lifecycle and callbacks.
- `Device.h`: device features, input, and image frames.
- `presentation/Presentation.h`: display presentation and input routing.
- `presentation/FrameBuffer.h`: frame buffer access.
- `ui/Widgets.h`: ready-to-use UI controls, including `Image`.
- `ui/Layout.h` and `ui/Style.h`: layout containers and appearance.
- `ui/LvglBackend.h`: the LVGL-backed UI renderer.

Use `#include <kavio/...>` in C++ applications. Public objects manage their
own resources according to the documented methods. Use SDK UI methods on the
UI thread; do not retain raw pointers to image frames beyond their lifetime.

For in-memory images, call `Image::SetBuffer(...)`. The widget copies pixels,
so the input buffer can be released after the call. `Image::SetSource(...)`
switches back to a normal image source.

C++ example (with a valid image buffer):

```cpp
kavio::ui::Image image;
image.SetBuffer(data, data_size, width, height, stride,
                kavio::PixelFormat::kBGRA8888, 590, 250);
```

## UI development guide

See [`Kavio_UI_Development_Guide.md`](../../../../docs/modules/sdk/Kavio_UI_Development_Guide.md) for architecture, control usage, rendering options, and examples.
