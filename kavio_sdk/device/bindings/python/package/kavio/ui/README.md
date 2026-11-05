# Python UI API

Build a widget tree with `Page`, `Column`, `Row`, and `Image`. An `Image`
accepts either a source string or a device image frame.

```python
from kavio import ui

image = ui.Image()  # Add this widget to a Page or Column.
image.set_buffer(frame, 590, 250)  # Copies pixels; frame may be released.
image.set_source("assets/icon.png")  # Replaces the buffer source.
```

`frame` is an `ImageFrame` returned by the SDK device interface. Supported
formats are UYVY, YUYV, and BGRA8888. `set_buffer` keeps the image aspect
ratio within the requested maximum dimensions. Use UI methods on the UI
thread. Invalid frame dimensions or unsupported formats raise an exception.
