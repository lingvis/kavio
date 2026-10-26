"""Kavio Python SDK type definitions.

Protocol requests and notifications use generated protobuf classes. Python
data classes are reserved for actual local image frames and errors.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field


class StatusCode(enum.IntEnum):
    """Status codes used by the current IPC protocol."""

    OK = 0
    INVALID_ARGUMENT = 1
    TOO_MANY_SESSIONS = 2
    ALREADY_EXISTS = 3
    NOT_FOUND = 4
    SHM_CREATE_FAILED = 5
    SOCKET_ERROR = 6
    PROTOCOL_ERROR = 7
    INTERNAL_ERROR = 8
    AUTH_FAILED = 9
    INVALID_STATE = 10
    UNSUPPORTED = 11
    SHM_ERROR = 200


class PixelFormat(enum.IntEnum):
    UNKNOWN = 0
    UYVY = 1
    YUYV = 2
    BGRA8888 = 3


class StatusError(Exception):
    """Exception wrapping a :class:`Status`."""

    def __init__(self, status: "Status"):
        super().__init__(status.message)
        self.status = status


@dataclass
class Rect:
    x: int = 0
    y: int = 0
    width: int = 0
    height: int = 0


@dataclass
class ImageFrame:
    data: bytes = field(default_factory=bytes)
    frame_id: int = 0
    timestamp_ns: int = 0
    width: int = 0
    height: int = 0
    stride: int = 0
    pixel_format: PixelFormat = PixelFormat.UNKNOWN
    valid_rect: Rect = field(default_factory=Rect)
    dest_pos_x: int = 0
    dest_pos_y: int = 0

    def empty(self) -> bool:
        return not self.data


@dataclass
class Status:
    code: StatusCode = StatusCode.OK
    message: str = ""

    @staticmethod
    def Ok(message: str = "") -> "Status":
        return Status(StatusCode.OK, message)

    @staticmethod
    def Error(code: StatusCode, message: str = "") -> "Status":
        return Status(code, message)

    def ok(self) -> bool:
        return self.code == StatusCode.OK

    def is_error(self) -> bool:
        return not self.ok()
