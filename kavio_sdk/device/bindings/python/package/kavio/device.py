from __future__ import annotations

import json
from enum import IntEnum, IntFlag
from typing import Callable, Optional

from . import _native


class PixelFormat(IntEnum):
    UNKNOWN = _native.PIXEL_FORMAT_UNKNOWN
    UYVY = _native.PIXEL_FORMAT_UYVY
    YUYV = _native.PIXEL_FORMAT_YUYV
    BGRA8888 = _native.PIXEL_FORMAT_BGRA8888


class RuntimeVideoStatus(IntEnum):
    UNKNOWN = _native.RUNTIME_VIDEO_STATUS_UNKNOWN
    NOT_CONNECTED = _native.RUNTIME_VIDEO_STATUS_NOT_CONNECTED
    CONNECTED = _native.RUNTIME_VIDEO_STATUS_CONNECTED
    RUNNING = _native.RUNTIME_VIDEO_STATUS_RUNNING
    STOPPED = _native.RUNTIME_VIDEO_STATUS_STOPPED
    ERROR = _native.RUNTIME_VIDEO_STATUS_ERROR


class InputMode(IntEnum):
    BLE = _native.INPUT_MODE_BLE
    RF24 = _native.INPUT_MODE_RF24
    USB = _native.INPUT_MODE_USB


class InputChannel(IntEnum):
    CHANNEL_1 = _native.INPUT_CHANNEL_1
    CHANNEL_2 = _native.INPUT_CHANNEL_2
    CHANNEL_3 = _native.INPUT_CHANNEL_3


class InputConnectionStatus(IntEnum):
    SYNCING = _native.INPUT_CONNECTION_STATUS_SYNCING
    PAIRING = _native.INPUT_CONNECTION_STATUS_PAIRING
    CONNECTED = _native.INPUT_CONNECTION_STATUS_CONNECTED
    DISCONNECTED = _native.INPUT_CONNECTION_STATUS_DISCONNECTED


class H264PacketFlag(IntFlag):
    KEY_FRAME = _native.H264_PACKET_KEY_FRAME
    IDR = _native.H264_PACKET_IDR
    SPS = _native.H264_PACKET_SPS
    PPS = _native.H264_PACKET_PPS
    HEADER = _native.H264_PACKET_HEADER


KeyCode = IntEnum("KeyCode", dict(_native.KEY_CODES), module=__name__)


class KeyOption(IntEnum):
    DOWN = _native.KEY_OPTION_DOWN
    UP = _native.KEY_OPTION_UP


class MouseOption(IntEnum):
    MOVE = _native.MOUSE_OPTION_MOVE
    LEFT_DOWN = _native.MOUSE_OPTION_LBUTTON_DOWN
    LEFT_UP = _native.MOUSE_OPTION_LBUTTON_UP
    RIGHT_DOWN = _native.MOUSE_OPTION_RBUTTON_DOWN
    RIGHT_UP = _native.MOUSE_OPTION_RBUTTON_UP
    MIDDLE_DOWN = _native.MOUSE_OPTION_MBUTTON_DOWN
    MIDDLE_UP = _native.MOUSE_OPTION_MBUTTON_UP
    WHEEL = _native.MOUSE_OPTION_WHEEL


class ImageFrame:
    """An SDK-owned image frame; metadata and a read-only buffer view."""
    def __init__(self, native_frame) -> None:
        self._native = native_frame
        self._buffer_view: Optional[memoryview] = None

    @property
    def info(self) -> dict:
        return self._native.info()

    @property
    def buffer(self) -> memoryview:
        if self._buffer_view is None:
            self._buffer_view = memoryview(self._native)
        return self._buffer_view


class H264Packet:
    """An encoded packet and its associated stream metadata."""
    def __init__(self, native_packet) -> None:
        self._native = native_packet
        self._buffer_view: Optional[memoryview] = None

    @property
    def info(self) -> dict:
        return self._native.info()

    @property
    def buffer(self) -> memoryview:
        if self._buffer_view is None:
            self._buffer_view = memoryview(self._native)
        return self._buffer_view


class TxSurface:
    """A device output surface; write pixel data with explicit dimensions."""
    def __init__(self, native_surface) -> None:
        self._native = native_surface

    @property
    def info(self) -> dict:
        return self._native.info()

    def write(
        self,
        data,
        *,
        width: int,
        height: int,
        stride: int,
        pixel_format: int = PixelFormat.BGRA8888,
        dest_pos_x: int = 0,
        dest_pos_y: int = 0,
        valid_rect=None,
    ) -> None:
        self._native.write(
            data,
            width,
            height,
            stride,
            int(pixel_format),
            dest_pos_x,
            dest_pos_y,
            valid_rect,
        )

    def clear(self) -> None:
        self._native.clear()

    def request_destroy(self) -> int:
        return int(self._native.request_destroy())


class Device:
    def __init__(self, handle, owner) -> None:
        self._handle = handle
        self._owner = owner
        self._callbacks = {}

    @property
    def is_ready(self) -> bool:
        return bool(_native.device_is_ready(self._handle))

    @property
    def usb_host_online(self) -> bool:
        return bool(_native.device_is_usb_host_online(self._handle))

    @property
    def comm_channel_ready(self) -> bool:
        return bool(_native.device_is_comm_channel_ready(self._handle))

    @property
    def comm_channel_id(self) -> int:
        return int(_native.device_get_comm_channel_id(self._handle))

    @property
    def runtime_data(self) -> dict:
        return _native.device_get_runtime_data(self._handle)

    @property
    def rx_info(self) -> dict:
        return _native.device_get_rx_info(self._handle)

    def set_rx_params(self, enabled: bool, frame_sample_interval: int = 1) -> int:
        return int(_native.device_set_rx_params(self._handle, enabled, frame_sample_interval))

    def set_tx_params(self, enabled: bool, z_order: int = 0) -> int:
        return int(_native.device_set_tx_params(self._handle, enabled, z_order))

    def read_latest_rx_frame(self) -> ImageFrame:
        return ImageFrame(_native.device_read_latest_rx_frame(self._handle))

    def create_tx_surface(self, width: int, height: int) -> int:
        return int(_native.device_create_tx_surface(self._handle, width, height))

    def destroy_tx_surface(self, surface_id: int) -> int:
        return int(_native.device_destroy_tx_surface(self._handle, surface_id))

    def send_keyboard(self, option: int, key_code: KeyCode, timestamp_ns: int = 0) -> None:
        _native.device_send_keyboard(self._handle, int(option), int(key_code), timestamp_ns)

    def send_mouse(
        self,
        option: int,
        x: int = 0,
        y: int = 0,
        wheel: int = 0,
        timestamp_ns: int = 0,
    ) -> None:
        _native.device_send_mouse(self._handle, int(option), x, y, wheel, timestamp_ns)

    def send_comm_data(self, data) -> None:
        _native.device_send_comm_data(self._handle, data)

    def get_app_config_json(self) -> str:
        return str(_native.device_get_app_config_json(self._handle))

    def update_app_config_json(self, value: str) -> None:
        _native.device_update_app_config_json(self._handle, value)

    @property
    def app_config(self):
        return json.loads(self.get_app_config_json())

    def update_app_config(self, value) -> None:
        self.update_app_config_json(
            json.dumps(value, ensure_ascii=False, separators=(",", ":"))
        )

    def reset_app_config(self) -> None:
        _native.device_reset_app_config(self._handle)

    def subscribe_h264(self) -> int:
        return int(_native.device_subscribe_h264(self._handle))

    def unsubscribe_h264(self, stream_id: int) -> int:
        return int(_native.device_unsubscribe_h264(self._handle, stream_id))

    def latest_h264_packet_seq(self, stream_id: int) -> int:
        return int(_native.device_get_latest_h264_packet_seq(self._handle, stream_id))

    def read_h264_packet(self, stream_id: int, seq: int) -> H264Packet:
        return H264Packet(_native.device_read_h264_packet(self._handle, stream_id, seq))

    def on_comm_data(self, handler: Optional[Callable[[bytes], None]]) -> None:
        self._callbacks["comm"] = handler
        _native.device_set_comm_data_callback(self._handle, handler)

    def on_tx_surface(self, handler: Optional[Callable[[int, int, Optional[TxSurface]], None]]) -> None:
        if handler is None:
            wrapped = None
        else:
            def wrapped(request_id, status, native_surface):
                handler(request_id, status, None if native_surface is None else TxSurface(native_surface))
        self._callbacks["tx_surface"] = wrapped
        _native.device_set_tx_surface_callback(self._handle, wrapped)

    def on_h264_stream(self, handler: Optional[Callable[[int, int, dict], None]]) -> None:
        self._callbacks["h264_stream"] = handler
        _native.device_set_h264_stream_callback(self._handle, handler)

    def on_h264_invalidated(self, handler: Optional[Callable[[dict], None]]) -> None:
        self._callbacks["h264_invalidated"] = handler
        _native.device_set_h264_invalidated_callback(self._handle, handler)

    def on_error(self, handler: Optional[Callable[[dict], None]]) -> None:
        self._callbacks["error"] = handler
        _native.device_set_error_callback(self._handle, handler)

    def on_config_changed(self, handler: Optional[Callable[[], None]]) -> None:
        self._callbacks["config_changed"] = handler
        _native.device_set_config_changed_callback(self._handle, handler)
