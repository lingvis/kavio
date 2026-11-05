from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Optional, Sequence

from . import _native


@dataclass(frozen=True)
class FrameInfo:
    width: int
    height: int
    stride: int
    pixel_format: int
    buffer_size: int
    surface_id: int
    generation: int


class FrameLease:
    """Writable frame lease. Present or discard it exactly once."""
    def __init__(self, native_frame) -> None:
        self._native = native_frame
        self._finished = False
        self._buffer_view: Optional[memoryview] = None

    @property
    def info(self) -> FrameInfo:
        return FrameInfo(**self._native.info())

    @property
    def buffer(self) -> memoryview:
        if self._finished:
            raise RuntimeError("FrameLease has already been completed")
        if self._buffer_view is None:
            self._buffer_view = memoryview(self._native)
        return self._buffer_view

    def _release_buffer(self) -> None:
        if self._buffer_view is not None:
            self._buffer_view.release()
            self._buffer_view = None

    def present(self, dirty_rects: Optional[Sequence[Sequence[int]]] = None) -> None:
        if self._finished:
            raise RuntimeError("FrameLease has already been completed")
        self._release_buffer()
        self._native.present(dirty_rects)
        self._finished = True

    def discard(self) -> None:
        if self._finished:
            return
        self._release_buffer()
        self._native.discard()
        self._finished = True

    def __enter__(self) -> "FrameLease":
        return self

    def __exit__(self, exc_type, exc, tb) -> bool:
        if not self._finished:
            self.discard()
        return False


class Presentation:
    """Display frames and touch/visibility callbacks for the current app."""
    def __init__(self, handle, owner) -> None:
        self._handle = handle
        self._owner = owner
        self._touch_handler: Optional[Callable[[dict], None]] = None
        self._visibility_handler: Optional[Callable[[bool], None]] = None

    @property
    def is_ready(self) -> bool:
        return bool(_native.presentation_is_ready(self._handle))

    @property
    def is_visible(self) -> bool:
        return bool(_native.presentation_is_visible(self._handle))

    @property
    def generation(self) -> int:
        return int(_native.presentation_generation(self._handle))

    @property
    def frame_info(self) -> FrameInfo:
        return FrameInfo(**_native.presentation_frame_info(self._handle))

    def acquire_frame(self) -> FrameLease:
        return FrameLease(_native.presentation_acquire_frame(self._handle))

    def clear(self, blue: int = 0, green: int = 0, red: int = 0, alpha: int = 255) -> None:
        _native.presentation_clear(self._handle, blue, green, red, alpha)

    def on_touch(self, handler: Optional[Callable[[dict], None]]) -> None:
        self._touch_handler = handler
        _native.presentation_set_touch_callback(self._handle, handler)

    def on_visibility(self, handler: Optional[Callable[[bool], None]]) -> None:
        self._visibility_handler = handler
        _native.presentation_set_visibility_callback(self._handle, handler)
