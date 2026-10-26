"""Python Metrics Demo adapter for the Kavio asynchronous Python SDK.

The IPC callback only handles protobuf status and opens the TX SHM; the main
thread renders frames. An asynchronous TX-create request never waits here.
"""

from __future__ import annotations

import importlib
import threading
import time
from typing import Callable


class KavioBridge:
    def __init__(self, client_name: str, on_ready: Callable[[bool], None],
                 on_channel: Callable[[bool], None], on_data: Callable[[bytes], None]):
        self._sdk = importlib.import_module("kavio_sdk")
        self._proto = self._sdk.proto_py.ipc_pb2
        self._on_ready = on_ready
        self._on_channel = on_channel
        self._on_data = on_data
        self._state_lock = threading.RLock()
        self._client_ready = False
        self._usb_host_online = False
        self._channel_id = 0
        self._channel_ready = False
        self._surface_id = None
        self._pending_surface_request = None
        self._next_surface_attempt = 0.0
        self._client = self._sdk.IPCClient()

        # This Demo does not consume RX and uses one TX overlay at z_order 100.
        # The SDK accepts the protocol types directly, without shadow classes.
        session = self._proto.A2PReqCreateSessionPb(client_name=client_name)
        session.rx_params.enabled = False
        session.rx_params.frame_sample_interval = 1
        session.tx_params.enabled = True
        session.tx_params.z_order = 100
        info = self._client.connect(self._sdk.ClientOptions(), session,
                                    self._handle_message,
                                    self._handle_disconnected,
                                    auto_query_runtime=False)
        self._handle_initialized(info)

    def _handle_initialized(self, info):
        with self._state_lock:
            self._client_ready = True
            self._channel_id = int(info.comm_channel_id)
            self._channel_ready = self._client.is_comm_channel_ready()
            ready = self._channel_ready
        self._on_ready(True)
        self._on_channel(ready)

    def _handle_message(self, package):
        proto = self._proto
        message_type = package.header.msg_type
        if message_type == proto.MSG_P2A_RSP_CREATE_TX_SHM:
            info = package.payload.p2a_rsp_create_tx_shm.tx_shm
            with self._state_lock:
                if package.header.track_id != self._pending_surface_request:
                    # Unexpected Surface: do not map it or keep a remote leak.
                    try:
                        self._client.destroy_tx_shm(info.id)
                    except Exception:
                        pass
                    return
                try:
                    self._client.open_tx_shm(info)
                except Exception:
                    self._pending_surface_request = None
                    self._next_surface_attempt = time.monotonic() + 1.0
                    self._client.destroy_tx_shm(info.id)
                    return
                self._surface_id = int(info.id)
                self._pending_surface_request = None
            return
        if message_type == proto.MSG_P2A_RSP_ERROR:
            with self._state_lock:
                if (package.payload.p2a_rsp_error.track_id == self._pending_surface_request or
                        package.header.track_id == self._pending_surface_request):
                    self._pending_surface_request = None
                    self._next_surface_attempt = time.monotonic() + 1.0
            return
        if message_type == proto.MSG_P2A_NTF_USB_HOST_STATE:
            self._update_channel(usb=package.payload.p2a_ntf_usb_host_state.online)
            return
        if message_type == proto.MSG_P2A_NTF_COMM_PEER_STATE:
            state = package.payload.p2a_ntf_comm_peer_state
            if state.channel_id == self._channel_id:
                self._update_channel()
            return
        if message_type == proto.MSG_P2A_DATA_COMM_CHANNEL:
            data = package.payload.p2a_data_comm_channel
            with self._state_lock:
                accepted = self._channel_ready and data.channel_id == self._channel_id
            if accepted:
                self._on_data(bytes(data.payload))

    def _update_channel(self, usb=None):
        with self._state_lock:
            if usb is not None:
                self._usb_host_online = bool(usb)
            ready = self._client.is_comm_channel_ready()
            changed = ready != self._channel_ready
            self._channel_ready = ready
        if changed:
            self._on_channel(ready)

    def _handle_disconnected(self):
        with self._state_lock:
            self._client_ready = False
            self._channel_ready = False
            self._channel_id = 0
            self._surface_id = None
            self._pending_surface_request = None
            self._next_surface_attempt = 0.0
        self._on_channel(False)
        self._on_ready(False)

    def send_channel_data(self, payload: bytes) -> bool:
        with self._state_lock:
            if not payload or not self._channel_ready:
                return False
        try:
            self._client.send_comm_channel_data(payload)
            return True
        except (OSError, ValueError, ConnectionError):
            return False

    def ensure_surface(self, width: int, height: int) -> bool:
        """Send TX-create once; future calls return True only when mapped."""
        with self._state_lock:
            if self._surface_id is not None:
                return True
            if (not self._client_ready or self._pending_surface_request is not None or
                    time.monotonic() < self._next_surface_attempt):
                return False
            try:
                # Holding the lock across the short send avoids a race between
                # the reply arriving and publishing our track_id.
                self._pending_surface_request = self._client.create_tx_shm(
                    width, height, self._sdk.PixelFormat.BGRA8888)
            except (OSError, ValueError, ConnectionError):
                self._next_surface_attempt = time.monotonic() + 1.0
                return False
        return False

    def write_frame(self, pixels: bytes, width: int, height: int, stride: int,
                    x: int, y: int, frame_id: int) -> bool:
        with self._state_lock:
            surface_id = self._surface_id
        if surface_id is None:
            return False
        try:
            return self._client.write_tx_frame(
                surface_id, pixels, width=width, height=height, stride=stride,
                valid_rect=self._sdk.Rect(0, 0, width, height),
                frame_id=frame_id, dest_pos_x=x, dest_pos_y=y)
        except Exception:
            return False

    def close(self):
        with self._state_lock:
            surface_id = self._surface_id
            self._surface_id = None
        if surface_id is not None:
            try:
                self._client.destroy_tx_shm(surface_id)
            except (OSError, ConnectionError):
                pass
        self._client.disconnect()
