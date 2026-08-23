"""Host Component 与 Python Device Component 共用的定长二进制协议。"""

from __future__ import annotations

import dataclasses
import struct
from typing import List

MAGIC = 0x4D59504B  # "KPYM"
VERSION = 1
# 必须与 common/DemoPythonMetricsProtocol.h 完全一致。
MAX_DISKS = 8

TYPE_QUERY = 1
TYPE_RESPONSE = 2
TYPE_POSITION = 3
TYPE_DEVICE_READY = 4

# 所有格式显式使用 '<' 固定 little-endian，并避免 Python 本机对齐规则影响线上布局。
_HEADER = struct.Struct("<IHHI")
_QUERY = struct.Struct("<IHHIQ")
_DISK = struct.Struct("<16sQQ")
_RESPONSE_PREFIX = struct.Struct("<IHHIQdQQI")
_POSITION = struct.Struct("<IHHIii")


@dataclasses.dataclass(frozen=True)
class DiskUsage:
    name: str
    total_bytes: int
    used_bytes: int


@dataclasses.dataclass(frozen=True)
class Metrics:
    sequence: int
    cpu_usage_percent: float
    memory_total_bytes: int
    memory_used_bytes: int
    disks: List[DiskUsage]


def _header(message_type: int, size: int) -> tuple[int, int, int, int]:
    return MAGIC, VERSION, message_type, size


# 所有解析入口都先通过公共头校验 magic/version/size，防止误解析其他 Channel 数据。
def peek_type(data: bytes) -> int | None:
    if len(data) < _HEADER.size:
        return None
    magic, version, message_type, size = _HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION or size != len(data):
        return None
    return message_type


def build_query(sequence: int) -> bytes:
    return _QUERY.pack(*_header(TYPE_QUERY, _QUERY.size), sequence)


def build_device_ready() -> bytes:
    return _HEADER.pack(*_header(TYPE_DEVICE_READY, _HEADER.size))


def parse_position(data: bytes) -> tuple[int, int] | None:
    if len(data) != _POSITION.size or peek_type(data) != TYPE_POSITION:
        return None
    _, _, _, _, x, y = _POSITION.unpack(data)
    return x, y


# Response 为固定 304 字节；disk_count 只决定前多少个预留槽位有效。
def parse_response(data: bytes) -> Metrics | None:
    expected_size = _RESPONSE_PREFIX.size + MAX_DISKS * _DISK.size
    if len(data) != expected_size or peek_type(data) != TYPE_RESPONSE:
        return None

    unpacked = _RESPONSE_PREFIX.unpack_from(data)
    sequence = unpacked[4]
    cpu = unpacked[5]
    memory_total = unpacked[6]
    memory_used = unpacked[7]
    disk_count = min(unpacked[8], MAX_DISKS)

    disks: List[DiskUsage] = []
    offset = _RESPONSE_PREFIX.size
    for index in range(MAX_DISKS):
        raw_name, total, used = _DISK.unpack_from(data, offset)
        offset += _DISK.size
        if index >= disk_count:
            continue
        name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="replace")
        disks.append(DiskUsage(name=name, total_bytes=total, used_bytes=used))

    return Metrics(
        sequence=sequence,
        cpu_usage_percent=cpu,
        memory_total_bytes=memory_total,
        memory_used_bytes=memory_used,
        disks=disks,
    )
