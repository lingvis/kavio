from kavio import App


def draw_background(presentation):
    with presentation.acquire_frame() as frame:
        info = frame.info
        buffer = frame.buffer
        width = info.width
        height = info.height
        stride = info.stride

        row = bytearray(stride)
        active_bytes = width * 4
        row[0:active_bytes:4] = bytes(
            (x * 255) // max(1, width - 1)
            for x in range(width)
        )
        row[2:active_bytes:4] = bytes([56]) * width
        row[3:active_bytes:4] = bytes([255]) * width

        for y in range(height):
            row[1:active_bytes:4] = bytes(
                [(y * 255) // max(1, height - 1)]
            ) * width
            start = y * stride
            buffer[start : start + stride] = row

        frame.present()


def draw_touch_marker(presentation, marker_x, marker_y):
    with presentation.acquire_frame() as frame:
        info = frame.info
        buffer = frame.buffer

        x0 = max(0, marker_x - 18)
        y0 = max(0, marker_y - 18)
        x1 = min(info.width, marker_x + 18)
        y1 = min(info.height, marker_y + 18)
        solid_row = bytes((255, 255, 255, 255)) * (x1 - x0)

        for y in range(y0, y1):
            start = y * info.stride + x0 * 4
            buffer[start : start + len(solid_row)] = solid_row

        frame.present([(x0, y0, x1 - x0, y1 - y0)])


app = App()


def on_ready(context):
    # Demonstrate a cancelled write transaction before the first custom frame.
    with context.presentation.acquire_frame() as discarded:
        discarded.discard()

    draw_background(context.presentation)

    def on_touch(event):
        for point in event.get("points", []):
            if point.get("active") and point.get("changed"):
                draw_touch_marker(
                    context.presentation,
                    point["x"],
                    point["y"],
                )
                break

    context.presentation.on_touch(on_touch)


app.on_ready(on_ready)
raise SystemExit(app.run())
