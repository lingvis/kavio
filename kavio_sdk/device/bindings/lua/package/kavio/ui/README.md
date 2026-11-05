# Lua UI API

Use `Page`, `Column`, and `Row` to build a widget tree. `Image` displays an
image source or pixels from an SDK image frame.

```lua
local ui = require("kavio.ui")
local image = ui.Image()
image:set_buffer(frame, 590, 250)  -- Copies frame pixels.
image:set_source("assets/icon.png") -- Switches to an image source.
```

The frame is returned by the SDK device interface. Supported pixel formats
are UYVY, YUYV, and BGRA8888. The image keeps its aspect ratio. The input
frame is not retained by the widget. Call UI setters on the UI thread.
