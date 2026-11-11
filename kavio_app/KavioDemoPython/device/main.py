"""Kavio Python device demonstration.

Demonstrates public SDK lifecycle callbacks, LVGL widgets, device input,
App Channel messages, HDMI RX/TX surfaces, H.264 capture, and configuration.
All UI callbacks run through the SDK application loop. Buffer ownership is
managed by the native SDK; the demo does not retain borrowed frame pointers.
"""

import json
import logging
import os
import sys
import time
from pathlib import Path

from kavio import App, ui
from kavio.device import KeyCode, KeyOption, MouseOption, PixelFormat

import demo_protocol as proto


# Display geometry matches the AppView surface supplied by the device.
DISPLAY_SIZE = 720  # Pixels; used for the top-level page layout.
PREVIEW_WIDTH = 620  # Same image-conversion bound as C++ and Lua (pixels).
PREVIEW_HEIGHT = 360  # Same image-conversion bound as C++ and Lua (pixels).



class KavioDemo:
    """Owns one SDK application, its active widgets, and device resources."""
    def __init__(self):
        """Initialize the SDK app, UI state, counters, and callback registrations."""
        # KAVIO_APP_LOG_DIR is provided by the application runtime.
        # Keep the standard Python logging API instead of a custom SDK wrapper.
        log_dir = os.environ.get("KAVIO_APP_LOG_DIR")
        if not log_dir:
            raise RuntimeError("KAVIO_APP_LOG_DIR is required for Python app logging")
        os.makedirs(log_dir, exist_ok=True)
        self.logger = logging.getLogger("KavioDemoPython")
        self.logger.setLevel(logging.INFO)
        self.logger.propagate = False
        if not self.logger.handlers:
            formatter = logging.Formatter(
                "[%(asctime)s] [%(process)d:%(thread)d] [%(levelname)s] "
                "[%(filename)s:%(lineno)d] %(message)s"
            )
            formatter.converter = time.gmtime
            file_handler = logging.FileHandler(
                os.path.join(log_dir, "KavioDemoPython.log"), encoding="utf-8"
            )
            file_handler.setFormatter(formatter)
            self.logger.addHandler(file_handler)
            # stderr is captured by AppService and followed by `kavio run`.
            stderr_handler = logging.StreamHandler(sys.stderr)
            stderr_handler.setFormatter(formatter)
            self.logger.addHandler(stderr_handler)
        self.logger.info("KavioDemo Python starting")
        self.app = App(rx_enabled=True, rx_frame_sample_interval=1,
                       tx_enabled=True, content=None)
        self.backend = ui.LvglBackend()

        self.page = "home"
        self.status = None
        self.detail = None
        self.rx_preview = None
        # An Image widget is owned by the current UI tree, not by this field.
        self.rx_live = False  # Explicitly enabled only on the RX page.
        self.rx_live_button = None  # Owned by the active page.
        self.rx_fps_label = None
        self.rx_fps_count = 0  # Distinct frames presented during one FPS window.
        self.rx_fps_window_start = 0.0
        self.last_rx_frame_id = None  # Prevent redundant redraws of unchanged frames.

        self.tx_surface = None
        self.h264_stream = 0
        self.h264_file = None
        self.next_h264_seq = 0
        self.last_tick = 0.0

        self.sequence = 1
        self.rx_reads = 0
        self.transfer_packets = 0
        self.protocol_errors = 0
        self.hello_sent = False

        self.app.on_ready(self.on_ready)
        self.app.on_tick(self.on_tick)
        self.app.on_stopping(self.on_stopping)

    # --- SDK lifecycle and event callbacks ---
    def on_ready(self, context):
        """Initialize device callbacks and the presentation after SDK startup.

        Args:
            context: Ready application context exposing device and presentation."""
        self.context = context
        self.device = context.device
        self.logger.info("SDK ready; runtime_data=%s", self.device.runtime_data)

        self.device.on_comm_data(self.on_comm_data)
        self.device.on_tx_surface(self.on_tx_surface)
        self.device.on_h264_stream(self.on_h264_stream)
        self.device.on_h264_invalidated(
            lambda _event: self.set_status("H.264 invalidated")
        )
        self.device.on_error(
            lambda event: self.set_status(f"Device error: {event}")
        )
        self.device.on_config_changed(
            lambda: self.set_status("Config changed")
        )

        self.backend.initialize(context.presentation)
        self.logger.info("LVGL backend initialized")

        try:
            initial_page = self.device.app_config.get("default_page", "home")
        except Exception:
            initial_page = "home"

        if initial_page not in proto.PAGES:
            initial_page = "home"
        self.logger.info("Initial page=%s", initial_page)
        self.show_page(initial_page)

    def on_stopping(self, _context):
        """Release TX/H.264 resources and shut down the LVGL backend.

        Args:
            _context: Application context supplied by the SDK."""
        self.logger.info("App stopping; page=%s rx_reads=%d transfer_packets=%d protocol_errors=%d", self.page, self.rx_reads, self.transfer_packets, self.protocol_errors)
        if self.h264_file is not None:
            self.h264_file.close()
            self.h264_file = None

        if self.tx_surface is not None:
            try:
                self.tx_surface.request_destroy()
            except Exception:
                pass
            self.tx_surface = None

        self.backend.shutdown()
        for handler in self.logger.handlers:
            handler.flush()

    # --- App Channel transport and state reporting ---
    def set_status(self, text):
        """Update the current page status label, if it is still mounted.

        Args:
            text: Human-readable status string."""
        if self.status is not None:
            self.status.set_text(text)

    def send_protocol(self, message_type, payload=b"", sequence=None, checksum=None):
        """Send a demo-protocol packet over the App Channel.

        Args:
            message_type: Protocol message identifier.
            payload: Bytes carried by the packet.
            sequence: Optional reply sequence; None allocates a new sequence.
            checksum: Optional precomputed payload checksum."""
        if sequence is None:
            sequence = self.sequence
            self.sequence = (self.sequence + 1) & 0xFFFFFFFF

        if self.device.comm_channel_ready:
            packet = proto.packet(message_type, sequence, payload, checksum)
            self.logger.info("App Channel send type=%d sequence=%d payload_bytes=%d", message_type, sequence, len(payload))
            self.device.send_comm_data(packet)
        else:
            self.logger.warning("App Channel send skipped: channel not ready type=%d", message_type)

    def state_text(self):
        """Build the compact host-readable device status report."""
        runtime = self.device.runtime_data
        return (
            f"lang=python;page={self.page};"
            f"usb={int(self.device.comm_channel_ready)};"
            f"rx={runtime['rx']['width']}x{runtime['rx']['height']};"
            f"tx={runtime['tx']['width']}x{runtime['tx']['height']};"
            f"rx_reads={self.rx_reads};"
            f"transfer={self.transfer_packets};"
            f"errors={self.protocol_errors}"
        )

    def on_comm_data(self, data):
        """Handle one validated App Channel datagram.

        Args:
            data: Received packet bytes supplied by the SDK."""
        parsed = proto.parse(bytes(data))
        if parsed is None:
            self.protocol_errors += 1
            self.logger.warning("Invalid App Channel packet bytes=%d errors=%d", len(data), self.protocol_errors)
            return

        message_type, sequence, payload, checksum = parsed
        self.logger.info("App Channel received type=%d sequence=%d payload_bytes=%d", message_type, sequence, len(payload))

        if message_type == proto.HELLO:
            self.send_protocol(
                proto.HELLO_ACK,
                b"KavioDemo Python",
                sequence=sequence,
            )
        elif message_type == proto.PING:
            self.send_protocol(proto.PONG, payload, sequence=sequence)
        elif message_type == proto.NAVIGATE:
            if len(payload) == 1 and payload[0] < len(proto.PAGES):
                self.show_page(proto.PAGES[payload[0]])
        elif message_type == proto.TRANSFER_DATA:
            self.transfer_packets += 1
            self.send_protocol(
                proto.TRANSFER_ACK,
                proto.transfer_ack(len(payload), checksum),
                sequence=sequence,
            )
        elif message_type == proto.QUERY_STATE:
            self.send_protocol(
                proto.STATE_REPORT,
                self.state_text().encode(),
                sequence=sequence,
            )

    # UI presentation helpers. Keep all device operations in the existing callbacks.
    # --- UI widget builders and navigation ---
    def label(self, text, size=16, color=0xC7D5ED):
        """Construct a text widget with optional pixel font size and RGB color."""
        return ui.Label(text).style(ui.Style(font_size=size, text_color=color))

    def action(self, text, handler, primary=False):
        """Construct a touch-sized button with its click handler.

        Args:
            text: Visible button caption.
            handler: Zero-argument action callback.
            primary: Whether to use the accent background."""
        return ui.Button(text, on_click=handler).style(ui.Style(
            width=290, height=58, radius=14, font_size=18,
            background=0x276BBD if primary else 0x233756,
            text_color=0xFFFFFF,
        ))

    def shell(self, title, subtitle, *sections):
        """Compose a 720-pixel page using a heading and content widgets."""
        header = ui.Column(
            self.label("KAVIO  /  DEVICE SDK", 16, 0x6FA3FF),
            self.label(title, 24, 0xFFFFFF),
            self.label(subtitle, 16, 0xB4C6E3),
        ).style(ui.Style(width=654, padding=20, gap=10,
                         background=0x132238, radius=22))
        return ui.Column(header, *sections).style(ui.Style(
            width=DISPLAY_SIZE, height=DISPLAY_SIZE, padding=24, gap=16,
            background=0x0A1220,
        ))

    def navigation(self):
        """Construct the two-column navigation cards for the available pages."""
        descriptions = {
            "render": "Touch, slider and switch",
            "transfer": "Host messaging and packets",
            "hdmi_rx": "Read incoming video frames",
            "hdmi_tx": "Create output surfaces",
            "runtime": "Live device information",
            "keyboard": "Send virtual key events",
            "h264": "Stream and capture video",
            "schema": "App settings and defaults",
            "mouse": "Pointer and wheel events",
        }
        pages = [p for p in proto.PAGES if p != "home"]
        cards = []
        for index in range(0, len(pages), 2):
            buttons = []
            for page in pages[index:index + 2]:
                selected = page
                title = page.replace("_", " ").upper()
                buttons.append(self.action(title,
                    lambda target=selected: self.show_page(target)))
            cards.append(ui.Row(*buttons).style(ui.Style(
                width=654, padding=6, gap=12, background=0x16243B,
                radius=16,
            )))
        return ui.Column(
            self.label("CAPABILITIES", 16, 0x85A3D0),
            *cards,
        ).style(ui.Style(width=654, padding=4, gap=10,
                         background=0x0A1220))

    def page_root(self, title, *items):
        """Construct a function page with controls and live feedback."""
        self.status = self.label("Ready", 18, 0xFFFFFF)
        self.detail = self.label("Select an action to inspect this API", 16, 0x9CB3D4)
        controls = []
        for item in items:
            if isinstance(item, ui.Button):
                item.style(ui.Style(
                    width=590, height=60, radius=14, font_size=18,
                    background=0x1F4269, text_color=0xFFFFFF,
                ))
            elif isinstance(item, ui.Slider):
                item.style(ui.Style(width=560))
            controls.append(item)
        actions = ui.Column(
            self.label("INTERACTIVE CONTROLS", 16, 0x85A3D0),
            *controls,
        ).style(ui.Style(width=654, padding=14, gap=16,
                         background=0x132238, radius=22))
        feedback = ui.Column(
            self.label("LIVE FEEDBACK", 16, 0x85A3D0),
            self.status, self.detail,
        ).style(ui.Style(width=654, padding=20, gap=10,
                         background=0x132238, radius=22))
        back = self.action("BACK TO OVERVIEW", lambda: self.show_page("home"), True)
        return self.shell(title.upper(), "Python  /  Public SDK capability demo",
                          actions, feedback, back)

    def show_page(self, page):
        """Replace the current widget tree with the requested demo page.

        Args:
            page: Page key defined by the shared demo protocol."""
        self.logger.info("Page navigation: %s -> %s", self.page, page)
        self.page = page
        self.rx_preview = None
        self.rx_live_button = None
        self.rx_fps_label = None
        self.rx_live = False
        self.last_rx_frame_id = None
        self.rx_fps_count = 0
        self.rx_fps_window_start = 0.0

        if page == "home":
            root = self.shell(
                "Explore capabilities", "Python  /  Interactive hardware API showcase",
                self.navigation(),
            )
        elif page == "render":
            root = self.page_root(
                "Render",
                ui.Button(
                    "Touch/Widget active",
                    on_click=lambda: self.set_status("Button clicked"),
                ),
                ui.Slider(
                    5,
                    60,
                    20,
                    on_change=lambda value: self.set_status(
                        f"Render FPS {value}"
                    ),
                ),
                ui.Switch(
                    True,
                    on_change=lambda enabled: self.set_status(
                        f"Metrics {enabled}"
                    ),
                ),
            )
        elif page == "transfer":
            root = self.page_root(
                "Transfer",
                ui.Button(
                    "Send Hello",
                    on_click=lambda: self.send_protocol(
                        proto.HELLO,
                        b"KavioDemo Python",
                    ),
                ),
                ui.Button(
                    "Send state report",
                    on_click=lambda: self.send_protocol(
                        proto.STATE_REPORT,
                        self.state_text().encode(),
                    ),
                ),
            )
        elif page == "hdmi_rx":
            self.rx_preview = ui.Image()
            self.rx_live_button = ui.Button("START LIVE", on_click=self.toggle_rx_live)
            self.rx_fps_label = self.label("FPS: 0.0", 18, 0x78E6BB)
            root = self.page_root(
                "HDMI RX",
                ui.Button("Read latest RX frame", on_click=self.read_rx),
                self.rx_live_button,
                self.rx_fps_label,
                self.label("RX FRAME PREVIEW  /  native RGB565", 16, 0x85A3D0),
                self.rx_preview,
            )
        elif page == "hdmi_tx":
            root = self.page_root(
                "HDMI TX",
                ui.Button(
                    "Create TX surface",
                    on_click=lambda: self.device.create_tx_surface(320, 180),
                ),
                ui.Button("Clear TX", on_click=self.clear_tx),
            )
        elif page == "keyboard":
            root = self.page_root(
                "Keyboard",
                ui.Button("Send A", on_click=lambda: self.send_key(KeyCode.A)),
                ui.Button("Send Enter", on_click=lambda: self.send_key(KeyCode.ENTER)),
                ui.Button("Send Left", on_click=lambda: self.send_key(KeyCode.LEFT)),
            )
        elif page == "mouse":
            root = self.page_root(
                "Mouse",
                ui.Button(
                    "Move center",
                    on_click=lambda: self.device.send_mouse(
                        MouseOption.MOVE,
                        640,
                        360,
                    ),
                ),
                ui.Button("Left click", on_click=self.left_click),
                ui.Button(
                    "Wheel +",
                    on_click=lambda: self.device.send_mouse(
                        MouseOption.WHEEL,
                        wheel=120,
                    ),
                ),
            )
        elif page == "h264":
            root = self.page_root(
                "H.264",
                ui.Button("Start capture", on_click=self.start_h264),
                ui.Button("Stop capture", on_click=self.stop_h264),
            )
        elif page == "schema":
            root = self.page_root(
                "App Configuration",
                ui.Button("Read config", on_click=self.read_config),
                ui.Button(
                    "Set render_fps=30",
                    on_click=self.update_config,
                ),
                ui.Button(
                    "Reset defaults",
                    on_click=self.device.reset_app_config,
                ),
            )
        else:
            root = self.page_root(
                "Runtime",
                ui.Button("Refresh runtime", on_click=self.refresh_runtime),
            )

        self.backend.set_content(root)

    # --- HDMI RX image presentation ---
    def toggle_rx_live(self):
        """Start/stop continuous RX reading with the same touch button."""
        self.rx_live = not self.rx_live
        self.logger.info("RX live preview %s", "enabled" if self.rx_live else "disabled")
        self.rx_fps_count = 0
        self.rx_fps_window_start = time.monotonic()
        if self.rx_live_button is not None:
            self.rx_live_button.set_text("STOP LIVE" if self.rx_live else "START LIVE")
        if self.rx_fps_label is not None:
            self.rx_fps_label.set_text("FPS: 0.0")
        self.set_status("RX live preview on" if self.rx_live else "RX live preview paused")

    def update_rx_fps(self, now):
        """Count distinct frames committed to Image over real elapsed seconds.

        Args:
            now: Monotonic timestamp in seconds, sampled by the UI loop.
        """
        elapsed = now - self.rx_fps_window_start
        if elapsed < 1.0:
            return
        fps = self.rx_fps_count / elapsed
        self.rx_fps_count = 0
        self.rx_fps_window_start = now
        if self.rx_fps_label is not None:
            self.rx_fps_label.set_text(f"FPS: {fps:.1f}")

    def read_rx(self, automatic=False):
        """Copy the latest RX frame into Image; automatic refresh skips duplicates.

        Args:
            automatic: True for periodic updates; suppresses no-frame errors
                and redundant redraws of the same frame identifier.
        """
        try:
            frame = self.device.read_latest_rx_frame()
            info = frame.info
            frame_id = info.get("frame_id")
            if automatic and frame_id is not None and frame_id == self.last_rx_frame_id:
                return
            if self.rx_preview is not None:
                self.rx_preview.set_buffer(frame, PREVIEW_WIDTH, PREVIEW_HEIGHT)
            self.last_rx_frame_id = frame_id
            self.rx_reads += 1
            if not automatic:
                self.logger.info("RX frame read width=%s height=%s stride=%s frame_id=%s bytes=%d", info.get("width"), info.get("height"), info.get("stride"), frame_id, len(frame.buffer))
            if automatic:
                self.rx_fps_count += 1
            else:
                self.set_status(
                    f"RX {info['width']}x{info['height']} "
                    f"stride={info['stride']} bytes={len(frame.buffer)}"
                )
        except Exception as exc:
            if not automatic:
                self.logger.exception("RX frame read failed")
                self.set_status(f"RX: {exc}")

    # --- TX surfaces and input events ---
    def on_tx_surface(self, _request_id, status, surface):
        """Populate a newly created TX surface with a BGRA8888 test image."""
        if status != 0 or surface is None:
            self.logger.error("TX surface creation failed request_id=%s status=%s", _request_id, status)
            self.set_status(f"TX create failed: {status}")
            return

        self.logger.info("TX surface created request_id=%s info=%s", _request_id, surface.info)
        self.tx_surface = surface
        info = surface.info
        width = info["width"]
        height = info["height"]
        stride = info["stride"]

        row = bytes((32, 120, 240, 255)) * width
        data = bytearray(stride * height)
        for y in range(height):
            start = y * stride
            data[start : start + width * 4] = row

        surface.write(
            data,
            width=width,
            height=height,
            stride=stride,
            pixel_format=PixelFormat.BGRA8888,
            valid_rect=(
                40,
                30,
                min(160, width - 40),
                min(100, height - 30),
            ),
        )
        self.set_status(f"TX surface {info['surface_id']} ready")

    def clear_tx(self):
        """Clear the active TX overlay without releasing its surface."""
        if self.tx_surface is not None:
            self.tx_surface.clear()
            self.set_status("TX cleared")

    def send_key(self, key_code):
        """Send a complete keyboard key press (DOWN followed by UP)."""
        self.logger.info("Keyboard key requested key_code=%s", key_code)
        self.device.send_keyboard(KeyOption.DOWN, key_code)
        self.device.send_keyboard(KeyOption.UP, key_code)
        self.set_status(f"Key {key_code} sent")

    def left_click(self):
        """Send a complete left mouse click."""
        self.logger.info("Mouse left click requested")
        self.device.send_mouse(MouseOption.LEFT_DOWN)
        self.device.send_mouse(MouseOption.LEFT_UP)
        self.set_status("Left click sent")

    # --- H.264 capture lifecycle ---
    def start_h264(self):
        """Request H.264 subscription; completion is asynchronous."""
        self.logger.info("H.264 subscribe requested")
        self.device.subscribe_h264()
        self.set_status("H.264 subscribe requested")

    def on_h264_stream(self, _request_id, status, info):
        """Open an H.264 capture file after successful subscription."""
        if status != 0:
            self.logger.error("H.264 subscription failed request_id=%s status=%s", _request_id, status)
            self.set_status(f"H.264 failed: {status}")
            return

        self.logger.info("H.264 stream ready request_id=%s info=%s", _request_id, info)
        self.h264_stream = info["stream_id"]
        self.next_h264_seq = 0
        path = Path("/tmp/kavio_demo_python.h264")
        self.h264_file = path.open("wb")
        self.set_status(f"H.264 stream {self.h264_stream} -> {path}")

    def stop_h264(self):
        """Unsubscribe and close the H.264 capture file."""
        if self.h264_stream:
            self.logger.info("H.264 unsubscribe stream_id=%d", self.h264_stream)
            self.device.unsubscribe_h264(self.h264_stream)
            self.h264_stream = 0

        if self.h264_file is not None:
            self.h264_file.close()
            self.h264_file = None

        self.set_status("H.264 stopped")

    # --- Configuration, runtime, and periodic application work ---
    def read_config(self):
        """Read and display a bounded configuration JSON excerpt."""
        try:
            config = self.device.get_app_config_json()
            self.logger.info("App config read bytes=%d", len(config))
            self.set_status(config[:160])
        except Exception as exc:
            self.logger.exception("App config read failed")
            self.set_status(str(exc))

    def update_config(self):
        """Demonstrate a typed configuration update."""
        try:
            config = self.device.app_config
            config["render_fps"] = 30
            self.logger.info("App config update requested: render_fps=30")
            self.device.update_app_config(config)
            self.set_status("render_fps updated to 30")
        except Exception as exc:
            self.logger.exception("App config update failed")
            self.set_status(str(exc))

    def refresh_runtime(self):
        """Update the visible page using current runtime data."""
        self.set_status(
            json.dumps(
                self.device.runtime_data,
                separators=(",", ":"),
            )[:180]
        )

    def on_tick(self, _context):
        """Process UI work and attempt RX preview on every tick while enabled."""
        self.backend.process()

        if self.device.comm_channel_ready:
            if not self.hello_sent:
                self.send_protocol(proto.HELLO, b"KavioDemo Python")
                self.hello_sent = True
        else:
            self.hello_sent = False

        now = time.monotonic()
        if self.page == "hdmi_rx" and self.rx_live:
            self.read_rx(automatic=True)
            self.update_rx_fps(time.monotonic())
        if now - self.last_tick < 0.05:
            return
        self.last_tick = now

        if self.h264_stream and self.h264_file is not None:
            latest = self.device.latest_h264_packet_seq(self.h264_stream)
            if self.next_h264_seq == 0 and latest:
                self.next_h264_seq = latest

            while self.next_h264_seq and self.next_h264_seq <= latest:
                try:
                    packet = self.device.read_h264_packet(
                        self.h264_stream,
                        self.next_h264_seq,
                    )
                    self.h264_file.write(packet.buffer)
                    self.next_h264_seq += 1
                except Exception:
                    break

        if self.page == "runtime":
            self.refresh_runtime()

    def run(self):
        """Start the SDK application loop and return its exit status."""
        return self.app.run()


raise SystemExit(KavioDemo().run())
