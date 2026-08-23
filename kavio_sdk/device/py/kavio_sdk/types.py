"""Kavio Python SDK type definitions.

The public data structures mirror the corresponding C++ SDK structures where
possible. Protocol message classes are generated separately in ``proto_py``.
"""

from __future__ import annotations

import enum
from dataclasses import dataclass, field
from typing import Any, List, Optional


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


class ProtocolState(enum.IntEnum):
    """Internal state of the blocking Python IPC client."""

    IDLE = 0
    CONNECTING = 1
    SEND_SESSION_CREATE = 2
    WAIT_SESSION_CREATED = 3
    WAIT_RX_SHM = 4
    WAIT_TX_SHM = 5
    INITIALIZED = 6
    ERROR = 7
    DISCONNECTED = 8


class SessionEventType(enum.IntEnum):
    UNKNOWN = 0
    NEW_RX_FRAME = 1


class PixelFormat(enum.IntEnum):
    UNKNOWN = 0
    UYVY = 1
    YUYV = 2
    BGRA8888 = 3


class ShmType(enum.IntEnum):
    UNKNOWN = 0
    RX = 1
    TX = 2


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
class ShmRegionInfo:
    type: ShmType = ShmType.UNKNOWN
    id: int = 0
    name: str = ""
    fd: int = -1
    addr: Any = None
    size: int = 0
    width: int = 0
    height: int = 0
    stride: int = 0
    pixel_format: PixelFormat = PixelFormat.UNKNOWN


@dataclass
class ShmInfo:
    id: int = 0
    name: str = ""
    size: int = 0
    width: int = 0
    height: int = 0
    stride: int = 0
    pixel_format: PixelFormat = PixelFormat.UNKNOWN


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
class RxParams:
    enabled: bool = True
    frame_sample_interval: int = 1


@dataclass
class TxParams:
    enabled: bool = False
    z_order: int = 0


@dataclass
class SessionInfo:
    session_id: int = 0
    max_sessions: int = 0
    max_rx_shms_per_session: int = 1
    max_tx_shms_per_session: int = 10
    comm_channel_id: int = 0
    comm_peer_online: bool = False
    rx_shm: Optional[ShmInfo] = None
    tx_shms: List[ShmInfo] = field(default_factory=list)


@dataclass
class ClientOptions:
    socket_path: str = "/tmp/kavio.sock"
    request_timeout_ms: int = 3000


@dataclass
class SessionOptions:
    client_name: str = "kavio_sdk_client"
    client_pid: int = 0
    rx_params: RxParams = field(default_factory=RxParams)
    tx_params: TxParams = field(default_factory=TxParams)
    auto_query_runtime_on_connect: bool = True
    auto_query_user_config_on_connect: bool = True


@dataclass
class QueryStatsResult:
    """Compatibility wrapper used by the existing callback API."""

    stats: Any = None


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
