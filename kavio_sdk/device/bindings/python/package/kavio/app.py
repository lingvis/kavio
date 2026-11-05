from __future__ import annotations

from typing import Callable, Optional

from . import _native
from .device import Device
from .presentation import Presentation


class AppContext:
    """Access to presentation and device features within application callbacks."""
    def __init__(self, presentation: Presentation, device: Device) -> None:
        self.presentation = presentation
        self.device = device


class App:
    """Runs SDK application callbacks and optional retained UI content."""
    def __init__(
        self,
        *,
        client_name: Optional[str] = None,
        idle_sleep_ms: int = 5,
        install_signal_handlers: bool = True,
        rx_enabled: bool = False,
        rx_frame_sample_interval: int = 1,
        tx_enabled: bool = False,
        tx_z_order: int = 0,
        content=None,
    ) -> None:
        self._handle = _native.app_create(
            client_name=client_name,
            idle_sleep_ms=idle_sleep_ms,
            install_signal_handlers=install_signal_handlers,
            rx_enabled=rx_enabled,
            rx_frame_sample_interval=rx_frame_sample_interval,
            tx_enabled=tx_enabled,
            tx_z_order=tx_z_order,
        )
        self.presentation = Presentation(_native.app_presentation(self._handle), self)
        self.device = Device(_native.app_device(self._handle), self)
        self.context = AppContext(self.presentation, self.device)
        self._ready_handlers: list[Callable[[AppContext], None]] = []
        self._tick_handlers: list[Callable[[AppContext], None]] = []
        self._stopping_handlers: list[Callable[[AppContext], None]] = []
        self._content = content
        self._ui_backend = None

    # Invoked after the device and presentation are ready.
    def on_ready(self, handler: Callable[[AppContext], None]) -> "App":
        self._ready_handlers.append(handler)
        return self

    def on_tick(self, handler: Callable[[AppContext], None]) -> "App":
        self._tick_handlers.append(handler)
        return self

    def on_stopping(self, handler: Callable[[AppContext], None]) -> "App":
        self._stopping_handlers.append(handler)
        return self

    def set_content(self, content) -> "App":
        self._content = content
        return self

    def request_stop(self) -> None:
        _native.app_request_stop(self._handle)

    @property
    def last_status(self) -> int:
        return int(_native.app_last_status(self._handle))

    def _ready(self) -> None:
        if self._content is not None:
            from .ui import LvglBackend
            self._ui_backend = LvglBackend()
            self._ui_backend.initialize(self.presentation)
            self._ui_backend.set_content(self._content)
        for handler in tuple(self._ready_handlers):
            handler(self.context)

    def _tick(self) -> None:
        if self._ui_backend is not None:
            self._ui_backend.process()
        for handler in tuple(self._tick_handlers):
            handler(self.context)

    def _stopping(self) -> None:
        try:
            for handler in tuple(self._stopping_handlers):
                handler(self.context)
        finally:
            if self._ui_backend is not None:
                self._ui_backend.shutdown()
                self._ui_backend = None

    def run(self) -> int:
        _native.app_set_ready_callback(self._handle, self._ready)
        _native.app_set_tick_callback(self._handle, self._tick)
        _native.app_set_stopping_callback(self._handle, self._stopping)
        try:
            return int(_native.app_run(self._handle))
        finally:
            _native.app_set_ready_callback(self._handle, None)
            _native.app_set_tick_callback(self._handle, None)
            _native.app_set_stopping_callback(self._handle, None)
