from __future__ import annotations

from dataclasses import dataclass
from typing import Callable, Generic, Optional, TypeVar

from .. import _ui_native

T = TypeVar("T")


@dataclass
class Style:
    """Optional UI style properties. Sizes of -1 use component defaults."""
    width: int = -1
    height: int = -1
    padding: int = -1
    gap: int = -1
    radius: int = -1
    font_size: int = -1
    background: Optional[int] = None
    text_color: Optional[int] = None
    opacity: int = 255


class State(Generic[T]):
    """Observable value for updating bound UI properties."""
    def __init__(self, value: T) -> None:
        self._value = value
        self._subscribers: list[Callable[[T], None]] = []

    @property
    def value(self) -> T:
        return self._value

    def set(self, value: T) -> None:
        self._value = value
        for subscriber in tuple(self._subscribers):
            subscriber(value)

    def subscribe(self, subscriber: Callable[[T], None]) -> None:
        self._subscribers.append(subscriber)
        subscriber(self._value)


class Widget:
    """Base retained UI element; add children and apply a Style."""
    def __init__(self, native) -> None:
        self._native = native
        self._children: list[Widget] = []
        self._callbacks = []

    def add(self, *children: "Widget") -> "Widget":
        for child in children:
            _ui_native.widget_add(self._native, child._native)
            self._children.append(child)
        return self

    def style(self, value: Style) -> "Widget":
        _ui_native.widget_set_style(
            self._native,
            width=value.width,
            height=value.height,
            padding=value.padding,
            gap=value.gap,
            radius=value.radius,
            font_size=value.font_size,
            background=value.background,
            text_color=value.text_color,
            opacity=value.opacity,
        )
        return self


class Page(Widget):
    """Root page container for UI content."""
    def __init__(self, *children: Widget) -> None:
        super().__init__(_ui_native.widget_create(_ui_native.PAGE))
        self.add(*children)


class Column(Widget):
    """Stacks children vertically."""
    def __init__(self, *children: Widget) -> None:
        super().__init__(_ui_native.widget_create(_ui_native.COLUMN))
        self.add(*children)


class Row(Widget):
    """Arranges children horizontally."""
    def __init__(self, *children: Widget) -> None:
        super().__init__(_ui_native.widget_create(_ui_native.ROW))
        self.add(*children)


class Label(Widget):
    def __init__(self, text="") -> None:
        initial = text.value if isinstance(text, State) else text
        super().__init__(_ui_native.widget_create(_ui_native.LABEL, text=str(initial)))
        if isinstance(text, State):
            text.subscribe(self.set_text)

    def set_text(self, text) -> "Label":
        _ui_native.widget_set_text(self._native, str(text))
        return self


class Button(Widget):
    def __init__(self, text="", on_click: Optional[Callable[[], None]] = None) -> None:
        super().__init__(_ui_native.widget_create(_ui_native.BUTTON, text=str(text)))
        if on_click is not None:
            self.on_click(on_click)

    def set_text(self, text) -> "Button":
        _ui_native.widget_set_text(self._native, str(text))
        return self

    def on_click(self, handler: Optional[Callable[[], None]]) -> "Button":
        self._callbacks.append(handler)
        _ui_native.widget_set_click_callback(self._native, handler)
        return self


class Image(Widget):
    """Image widget supporting a file source or an SDK image-frame buffer.

    set_buffer() copies converted pixels, so the frame can be released after
    the call. The image fits inside the requested bounds without distortion.
    Call UI methods from the UI thread.
    """

    def __init__(self, source="") -> None:
        super().__init__(_ui_native.widget_create(_ui_native.IMAGE, text=str(source)))

    def set_source(self, source) -> "Image":
        """Display the specified image source instead of the previous buffer."""
        _ui_native.widget_set_source(self._native, str(source))
        return self

    def set_buffer(self, frame, max_width=620, max_height=320) -> "Image":
        """Copy an SDK image frame; accepts UYVY, YUYV or BGRA8888 frames."""
        _ui_native.widget_set_buffer(self._native, frame._native, max_width, max_height)
        return self


class Slider(Widget):
    """Integer slider with value/range updates."""
    def __init__(self, minimum=0, maximum=100, value=0, on_change=None) -> None:
        super().__init__(_ui_native.widget_create(
            _ui_native.SLIDER, minimum=minimum, maximum=maximum, value=value))
        if on_change is not None:
            self.on_change(on_change)

    @property
    def value(self) -> int:
        return int(_ui_native.widget_get_value(self._native))

    def set_value(self, value: int) -> "Slider":
        _ui_native.widget_set_value(self._native, value)
        return self

    def set_range(self, minimum: int, maximum: int) -> "Slider":
        _ui_native.widget_set_range(self._native, minimum, maximum)
        return self

    def on_change(self, handler) -> "Slider":
        self._callbacks.append(handler)
        _ui_native.widget_set_value_callback(self._native, handler)
        return self


class Switch(Widget):
    """On/off widget with a change handler."""
    def __init__(self, checked=False, on_change=None) -> None:
        super().__init__(_ui_native.widget_create(_ui_native.SWITCH, checked=checked))
        if on_change is not None:
            self.on_change(on_change)

    @property
    def checked(self) -> bool:
        return bool(_ui_native.widget_get_checked(self._native))

    def set_checked(self, checked: bool) -> "Switch":
        _ui_native.widget_set_checked(self._native, checked)
        return self

    def on_change(self, handler) -> "Switch":
        self._callbacks.append(handler)
        _ui_native.widget_set_checked_callback(self._native, handler)
        return self


class LvglBackend:
    def __init__(self) -> None:
        self._native = _ui_native.backend_create()
        self._presentation = None
        self._root = None

    def initialize(self, presentation) -> None:
        _ui_native.backend_initialize(self._native, presentation._handle)
        self._presentation = presentation

    def process(self) -> None:
        _ui_native.backend_process(self._native)

    def set_content(self, root: Widget) -> None:
        _ui_native.backend_set_content(self._native, root._native)
        self._root = root

    def clear_content(self) -> None:
        _ui_native.backend_clear_content(self._native)
        self._root = None

    def shutdown(self) -> None:
        _ui_native.backend_shutdown(self._native)
        self._root = None
        self._presentation = None
