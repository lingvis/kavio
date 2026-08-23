"""Kavio Python SDK.

The package provides the IPC client, POSIX shared-memory helpers, public types,
and protobuf modules generated from the same ``kavio_sdk/device/proto`` definitions
used by the C++ SDK.
"""

from . import frame_shm, ipc_client, proto_py, types
from .frame_shm import FrameShm
from .ipc_client import IPCClient
from .sdk_version import (
    SDK_VERSION,
    SDK_VERSION_MAJOR,
    SDK_VERSION_MINOR,
    SDK_VERSION_PATCH,
)
from .types import (
    ClientOptions,
    ImageFrame,
    PixelFormat,
    ProtocolState,
    QueryStatsResult,
    Rect,
    RxParams,
    SessionEventType,
    SessionInfo,
    SessionOptions,
    ShmInfo,
    ShmRegionInfo,
    ShmType,
    Status,
    StatusCode,
    StatusError,
    TxParams,
)

__all__ = [
    "types",
    "frame_shm",
    "ipc_client",
    "proto_py",
    "StatusCode",
    "Status",
    "StatusError",
    "PixelFormat",
    "ProtocolState",
    "SessionEventType",
    "ShmType",
    "ShmInfo",
    "SessionInfo",
    "ImageFrame",
    "ClientOptions",
    "SessionOptions",
    "RxParams",
    "TxParams",
    "Rect",
    "ShmRegionInfo",
    "QueryStatsResult",
    "FrameShm",
    "IPCClient",
    "SDK_VERSION",
    "SDK_VERSION_MAJOR",
    "SDK_VERSION_MINOR",
    "SDK_VERSION_PATCH",
]
