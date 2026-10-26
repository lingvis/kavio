"""Read-only H.264 packet ring for the Kavio encoded-stream SHM ABI.

The reader copies each packet and verifies the Platform's payload-wide
seqlock before returning it. Packet loss is normal for a slow subscriber;
callers should resume from a valid SPS/PPS/IDR sequence after a gap.
"""

from __future__ import annotations

import ctypes
import mmap
import os
import threading
from dataclasses import dataclass
from typing import Optional

from . import _atomic

MAGIC = 0x4553484D
HEADER_SIZE = 96
PACKET_META_SIZE = 56

PACKET_FLAG_KEY_FRAME = 1 << 0
PACKET_FLAG_IDR = 1 << 1
PACKET_FLAG_SPS = 1 << 2
PACKET_FLAG_PPS = 1 << 3
PACKET_FLAG_HEADER = 1 << 4


@dataclass(frozen=True)
class H264Packet:
    seq: int
    pts_us: int
    dts_us: int
    timestamp_ns: int
    flags: int
    nal_type: int
    data: bytes


class _Header(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32), ("header_size", ctypes.c_uint32),
        ("packet_meta_size", ctypes.c_uint32), ("reserved_layout", ctypes.c_uint32),
        ("stream_id", ctypes.c_uint32), ("session_id", ctypes.c_uint32),
        ("codec", ctypes.c_uint32), ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32), ("fps", ctypes.c_uint32),
        ("bitrate_kbps", ctypes.c_uint32), ("gop", ctypes.c_uint32),
        ("packet_capacity", ctypes.c_uint32), ("payload_capacity", ctypes.c_uint32),
        ("total_shm_size", ctypes.c_uint32), ("write_seq", ctypes.c_uint64),
        ("write_index", ctypes.c_uint32), ("payload_write_offset", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint64), ("reserved1", ctypes.c_uint64),
    ]


class _PacketMeta(ctypes.Structure):
    _fields_ = [
        ("writing", ctypes.c_uint32), ("valid", ctypes.c_uint32),
        ("seq", ctypes.c_uint64), ("pts_us", ctypes.c_uint64),
        ("dts_us", ctypes.c_uint64), ("timestamp_ns", ctypes.c_uint64),
        ("offset", ctypes.c_uint32), ("size", ctypes.c_uint32),
        ("flags", ctypes.c_uint32), ("nal_type", ctypes.c_uint32),
    ]


assert ctypes.sizeof(_Header) == HEADER_SIZE
assert ctypes.sizeof(_PacketMeta) == PACKET_META_SIZE

# libc mmap allows an actual read-only shared mapping of an O_RDONLY SHM
# descriptor. No writable reference to the encoded data is exposed to Python.
_libc = ctypes.CDLL(None, use_errno=True)
_libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                       ctypes.c_int, ctypes.c_int, ctypes.c_long]
_libc.mmap.restype = ctypes.c_void_p
_libc.munmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
_libc.munmap.restype = ctypes.c_int
_MAP_FAILED = ctypes.c_void_p(-1).value


class EncodedStreamShm:
    """Owns a read-only mapping. `read_packet` returns None if unavailable."""

    def __init__(self, name: str, expected_size: int = 0, expected_stream_id: int = 0):
        if not name or name in ("/", ".") or "/" in name.lstrip("/"):
            raise ValueError("invalid POSIX shared memory name")
        path = "/dev/shm/" + name.lstrip("/")
        self._lock = threading.Lock()
        self._fd = os.open(path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
        self._address = None
        self._size = 0
        try:
            size = os.fstat(self._fd).st_size
            if size < HEADER_SIZE or (expected_size and size < expected_size):
                raise ValueError("encoded SHM is smaller than expected")
            address = _libc.mmap(None, size, mmap.PROT_READ, mmap.MAP_SHARED, self._fd, 0)
            if address == _MAP_FAILED or address is None:
                raise OSError(ctypes.get_errno(), "read-only mmap failed")
            self._address, self._size = address, size
            header = _Header.from_address(address)
            cap, payload_cap = int(header.packet_capacity), int(header.payload_capacity)
            expected = HEADER_SIZE + cap * PACKET_META_SIZE + payload_cap
            if (header.magic != MAGIC or header.header_size != HEADER_SIZE or
                    header.packet_meta_size != PACKET_META_SIZE or
                    cap == 0 or cap > 1024 or payload_cap == 0 or
                    expected != size or header.total_shm_size != size or
                    (expected_stream_id and header.stream_id != expected_stream_id)):
                raise ValueError("invalid encoded SHM layout or stream ID")
            self._packet_capacity = cap
            self._payload_capacity = payload_cap
            self._payload_address = address + HEADER_SIZE + cap * PACKET_META_SIZE
        except Exception:
            self.close()
            raise

    def close(self):
        with self._lock:
            if self._address is not None:
                _libc.munmap(self._address, self._size)
                self._address = None
            if self._fd >= 0:
                os.close(self._fd)
                self._fd = -1

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def latest_seq(self) -> int:
        with self._lock:
            if self._address is None:
                return 0
            return _atomic.load_u64(self._address + _Header.write_seq.offset)

    def read_packet(self, seq: int) -> Optional[H264Packet]:
        """Return a copy of packet `seq`, or None if busy/overwritten."""
        if seq <= 0:
            return None
        with self._lock:
            if self._address is None:
                return None
            address = self._address
            epoch_addr = address + _Header.reserved0.offset
            epoch = _atomic.load_u64(epoch_addr)
            if epoch & 1:
                return None
            meta_addr = address + HEADER_SIZE + (seq % self._packet_capacity) * PACKET_META_SIZE
            if (_atomic.load_u32(meta_addr + _PacketMeta.writing.offset) or
                    not _atomic.load_u32(meta_addr + _PacketMeta.valid.offset)):
                return None
            # C++ writer publishes metadata before storing valid=1 (release).
            # The acquire loads above make the snapshot visible.
            meta = _PacketMeta.from_address(meta_addr)
            actual_seq, offset, size = int(meta.seq), int(meta.offset), int(meta.size)
            if (actual_seq != seq or size <= 0 or offset >= self._payload_capacity or
                    offset + size > self._payload_capacity):
                return None
            packet = H264Packet(actual_seq, meta.pts_us, meta.dts_us,
                                meta.timestamp_ns, meta.flags, meta.nal_type,
                                ctypes.string_at(self._payload_address + offset, size))
            if (epoch != _atomic.load_u64(epoch_addr) or
                    _atomic.load_u32(meta_addr + _PacketMeta.writing.offset) or
                    not _atomic.load_u32(meta_addr + _PacketMeta.valid.offset) or
                    _atomic.load_u64(meta_addr + _PacketMeta.seq.offset) != seq):
                return None
            return packet
