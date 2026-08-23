"""Kavio Python Metrics Demo 的 Python SDK 适配层。

本文件使用 ``kavio_sdk/device/py/kavio_sdk`` 的公开接口，并集中校验 Demo 所需的
SDK 导出。接口不完整时直接报告缺失项，避免业务层通过动态探测掩盖实际错误。
"""

from __future__ import annotations

import importlib
import threading
from typing import Any, Callable


class KavioBridge:
    """把当前 Kavio Python SDK 的生命周期/Channel/TX API 收敛成 Demo 所需的最小接口。

    业务层不直接接触 IPCClient，是为了让未来 SDK 接口调整只影响这一处。
    """
    def __init__(self, client_name: str, on_ready: Callable[[bool], None],
                 on_channel: Callable[[bool], None], on_data: Callable[[bytes], None]):
        self._sdk = importlib.import_module("kavio_sdk")
        self._on_ready = on_ready
        self._on_channel = on_channel
        self._on_data = on_data

        self._state_lock = threading.Lock()
        self._client_ready = False
        self._usb_host_online = False
        self._channel_id = 0
        self._channel_ready = False
        self._surface_id: int | None = None

        self._client = self._create_client(client_name)

    @staticmethod
    def _ok(result: Any) -> bool:
        if result is None or result is True:
            return True
        value = getattr(result, "ok", None)
        if callable(value):
            return bool(value())
        if value is not None:
            return bool(value)
        return bool(result)

    @staticmethod
    def _status_text(result: Any) -> str:
        message = getattr(result, "message", "")
        code = getattr(result, "code", None)
        if message and code is not None:
            return f"code={code}, message={message}"
        if message:
            return str(message)
        return repr(result)

    def _create_client(self, client_name: str) -> Any:
        # 集中校验 Demo 依赖的 SDK 导出；缺少接口时立即报告明确错误。
        required = (
            "IPCClient", "ClientOptions", "SessionOptions", "RxParams", "TxParams",
            "PixelFormat", "Rect",
        )
        missing = [name for name in required if not hasattr(self._sdk, name)]
        if missing:
            raise RuntimeError(
                "kavio_sdk missing required exports: " + ", ".join(missing)
            )

        client = self._sdk.IPCClient()
        callbacks_type = getattr(self._sdk.IPCClient, "NetworkCallbacks", None)
        if callbacks_type is None:
            raise RuntimeError("kavio_sdk.IPCClient must provide NetworkCallbacks")

        client_options = self._sdk.ClientOptions()
        session_options = self._sdk.SessionOptions(
            client_name=client_name,
            rx_params=self._sdk.RxParams(enabled=False),
            tx_params=self._sdk.TxParams(enabled=True, z_order=100),
            auto_query_runtime_on_connect=False,
            auto_query_user_config_on_connect=False,
        )
        callbacks = callbacks_type(
            on_initialized=self._handle_initialized,
            on_disconnected=self._handle_disconnected,
            on_comm_channel_data=self._handle_channel_data,
            on_usb_online=self._handle_usb_online,
            on_comm_channel_ready_changed=self._handle_channel_ready_changed,
        )

        status = client.connect(client_options, session_options, callbacks)
        if not self._ok(status):
            raise RuntimeError(
                "Kavio IPCClient connect failed: " + self._status_text(status)
            )
        return client

    def _set_client_ready(self, ready: bool) -> None:
        notify = False
        with self._state_lock:
            ready = bool(ready)
            if self._client_ready != ready:
                self._client_ready = ready
                notify = True
        if notify:
            self._on_ready(ready)

    def _handle_initialized(self, status: Any, session_info: Any) -> None:
        ready = self._ok(status)
        self._set_client_ready(ready)
        if not ready:
            return
        notify_channel = False
        with self._state_lock:
            self._channel_id = int(session_info.comm_channel_id)
            channel_ready = (
                self._usb_host_online
                and self._channel_id != 0
                and bool(session_info.comm_peer_online)
            )
            if self._channel_ready != channel_ready:
                self._channel_ready = channel_ready
                notify_channel = True
        if notify_channel:
            self._on_channel(channel_ready)

    def _handle_disconnected(self) -> None:
        notify_ready = False
        notify_channel = False
        with self._state_lock:
            if self._client_ready:
                self._client_ready = False
                notify_ready = True
            self._usb_host_online = False
            self._channel_id = 0
            if self._channel_ready:
                self._channel_ready = False
                notify_channel = True
            self._surface_id = None
        if notify_channel:
            self._on_channel(False)
        if notify_ready:
            self._on_ready(False)

    def _handle_usb_online(self, online: bool, _reason: str) -> None:
        with self._state_lock:
            self._usb_host_online = bool(online)

    def _handle_channel_ready_changed(self, ready: bool) -> None:
        notify = False
        with self._state_lock:
            ready = bool(ready)
            if self._channel_ready != ready:
                self._channel_ready = ready
                notify = True
        if notify:
            self._on_channel(ready)

    def _handle_channel_data(self, payload: bytes) -> None:
        with self._state_lock:
            accepted = self._channel_ready
        if accepted:
            self._on_data(bytes(payload))

    def send_channel_data(self, payload: bytes) -> bool:
        if not payload:
            return False
        with self._state_lock:
            if not self._channel_ready:
                return False
        return self._ok(self._client.send_comm_channel_data(payload))

    def ensure_surface(self, width: int, height: int) -> bool:
        # Surface 绑定当前 session；session 失效后旧 id 不能复用，必须重新创建。
        with self._state_lock:
            if self._surface_id is not None:
                return True
            if not self._client_ready:
                return False

        before_ids = {info.id for info in self._client.get_session_info().tx_shms}
        status = self._client.create_tx_shm(
            width, height, self._sdk.PixelFormat.BGRA8888
        )
        if not self._ok(status):
            return False

        created_id = None
        for info in self._client.get_session_info().tx_shms:
            if info.id not in before_ids:
                created_id = int(info.id)
                break
        if created_id is None:
            return False

        with self._state_lock:
            if self._surface_id is None:
                self._surface_id = created_id
                return True

        # 当前 Demo 只需要一个 TX Surface。若并发创建导致已有 Surface，清理多余 SHM。
        self._client.destroy_tx_shm(created_id)
        return True

    def write_frame(self, pixels: bytes, width: int, height: int, stride: int,
                    x: int, y: int, frame_id: int) -> bool:
        with self._state_lock:
            surface_id = self._surface_id
        if surface_id is None:
            return False

        valid_rect = self._sdk.Rect(x=0, y=0, width=width, height=height)
        status = self._client.write_tx_frame(
            surface_id,
            pixels,
            width=width,
            height=height,
            stride=stride,
            valid_rect=valid_rect,
            frame_id=frame_id,
            dest_pos_x=x,
            dest_pos_y=y,
        )
        return self._ok(status)

    def close(self) -> None:
        with self._state_lock:
            surface_id = self._surface_id
            self._surface_id = None
        if surface_id is not None:
            try:
                self._client.destroy_tx_shm(surface_id)
            except Exception:
                pass
        self._client.disconnect()
