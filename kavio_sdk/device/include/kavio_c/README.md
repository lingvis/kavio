# C Device SDK API

This directory contains the public C-compatible ABI headers of the Kavio
Device SDK. Use `#include <kavio_c/...>` from C or language bindings.

## Where to start

- `common.h`: status codes, ABI conventions, and shared value types.
- `app.h`: application lifecycle.
- `device.h`: device and image-frame access.
- `presentation.h` and `framebuffer.h`: display and frame buffers.
- `ui.h`: widget creation, configuration, and image-buffer updates.
- `events.h`: input and event types.

This API uses functions, C structures, and opaque handles rather than C++
classes. Initialize versioned structures with their `*_init` function before
passing them to the SDK. Check returned status codes and release objects with
the matching destroy/release function.

`kavio_widget_set_image_buffer(...)` copies and converts image pixels into an
Image widget; the SDK does not retain the caller's input pointer. Call UI
functions from the UI thread.
