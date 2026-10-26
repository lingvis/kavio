"""
Kavio Python SDK - FrameShm

POSIX shared memory double-buffer wrapper for latest-only read/write of image frames.

Mirrors kavio_sdk/device/src/internal/FrameShm.h and kavio_sdk/device/src/internal/FrameShm.cpp

Memory layout:
  [SharedMemoryHeader]  (sizeof = 80 bytes)
  [ShmBufferMeta x 2]   (sizeof = 80 bytes each)
  [payload0]            (payload_capacity bytes)
  [payload1]            (payload_capacity bytes)
"""

from __future__ import annotations

import ctypes
import errno
import mmap
import os
from typing import Optional, Tuple

from .types import PixelFormat, Rect, Status, StatusCode, StatusError
from . import _atomic

# Constants matching C++ kavio::kShmMagic and kShmBufferCount
K_SHM_MAGIC = 0x56495348
K_SHM_BUFFER_COUNT = 2

# Offsets and sizes computed from C++ struct layouts
# SharedMemoryHeader fixed layout. SHM does not carry an independent version.
# magic/header_size/buffer_meta_size/reserved_layout + 12 uint32_t runtime fields +
# reserved0/reserved1 = 80 bytes.
SHARED_MEMORY_HEADER_SIZE = 80

# ShmBufferMeta fixed layout:
# frame_id(8) + timestamp_ns(8) + width/height/stride/pixel_format(16) +
# payload_size(4) + write_seq(4) + valid_rect_pos_x/y/width/height(16) +
# valid(4) + read_in_progress(4) + dest_pos_x/y(8) + reserved1(8) = 80 bytes
SHM_BUFFER_META_SIZE = 80

MINIMUM_HEADER_SIZE = SHARED_MEMORY_HEADER_SIZE + SHM_BUFFER_META_SIZE * K_SHM_BUFFER_COUNT


def _compute_payload_capacity(height: int, stride: int) -> int:
    return height * stride


def _compute_total_size(payload_capacity: int) -> int:
    return MINIMUM_HEADER_SIZE + payload_capacity * K_SHM_BUFFER_COUNT


class SharedMemoryHeader(ctypes.Structure):
    """Matching C++ kavio::SharedMemoryHeader."""
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("header_size", ctypes.c_uint32),
        ("buffer_meta_size", ctypes.c_uint32),
        ("reserved_layout", ctypes.c_uint32),
        ("shm_type", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("session_id", ctypes.c_uint32),
        ("shm_id", ctypes.c_uint32),
        ("buffer_count", ctypes.c_uint32),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("stride", ctypes.c_uint32),
        ("payload_capacity", ctypes.c_uint32),
        ("total_shm_size", ctypes.c_uint32),
        ("published_index", ctypes.c_uint32),
        ("consumer_index", ctypes.c_uint32),
        ("reserved0", ctypes.c_uint64),
        ("reserved1", ctypes.c_uint64),
    ]


class ShmBufferMeta(ctypes.Structure):
    """Matching C++ kavio::ShmBufferMeta."""
    _fields_ = [
        ("frame_id", ctypes.c_uint64),
        ("timestamp_ns", ctypes.c_uint64),
        ("width", ctypes.c_uint32),
        ("height", ctypes.c_uint32),
        ("stride", ctypes.c_uint32),
        ("pixel_format", ctypes.c_uint32),
        ("payload_size", ctypes.c_uint32),
        ("write_seq", ctypes.c_uint32),
        ("valid_rect_pos_x", ctypes.c_int32),
        ("valid_rect_pos_y", ctypes.c_int32),
        ("valid_rect_width", ctypes.c_uint32),
        ("valid_rect_height", ctypes.c_uint32),
        ("valid", ctypes.c_uint32),
        ("read_in_progress", ctypes.c_uint32),
        ("dest_pos_x", ctypes.c_int32),
        ("dest_pos_y", ctypes.c_int32),
        ("reserved1", ctypes.c_uint64),
    ]


if ctypes.sizeof(SharedMemoryHeader) != SHARED_MEMORY_HEADER_SIZE:
    raise RuntimeError(
        f"SharedMemoryHeader ctypes layout mismatch: actual={ctypes.sizeof(SharedMemoryHeader)}, "
        f"expected={SHARED_MEMORY_HEADER_SIZE}"
    )
if ctypes.sizeof(ShmBufferMeta) != SHM_BUFFER_META_SIZE:
    raise RuntimeError(
        f"ShmBufferMeta ctypes layout mismatch: actual={ctypes.sizeof(ShmBufferMeta)}, "
        f"expected={SHM_BUFFER_META_SIZE}"
    )


class FrameShm:
    """
    Python implementation of kavio::FrameShm.

    Provides open/read/write access to POSIX shared memory with a double-buffer
    layout for latest-only frame access.
    """

    def __init__(self):
        self._name: str = ""
        self._fd: int = -1
        self._mmap: Optional[mmap.mmap] = None
        self._size: int = 0
        self._header: Optional[SharedMemoryHeader] = None
        self._meta: Tuple[Optional[ShmBufferMeta], Optional[ShmBufferMeta]] = (None, None)
        self._payload_offset: int = 0
        self._payload_capacity: int = 0
        self._layout_valid: bool = False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    @property
    def name(self) -> str:
        return self._name

    @property
    def is_open(self) -> bool:
        return self._mmap is not None

    @staticmethod
    def open(shm_name: str) -> "FrameShm":
        """
        Open an existing POSIX shared memory region.

        Raises Status on failure.
        """
        shm = FrameShm()
        shm._open_internal(shm_name)
        return shm

    def close(self):
        """Unmap and close the shared memory."""
        # Release buffer references first to avoid "cannot close exported pointers"
        self._header = None
        self._meta = (None, None)
        if self._mmap is not None:
            self._mmap.close()
            self._mmap = None
        if self._fd >= 0:
            os.close(self._fd)
            self._fd = -1
        self._size = 0
        self._payload_offset = 0
        self._payload_capacity = 0
        self._layout_valid = False

    def _layout_validation_error(self) -> Optional[str]:
        """Return a detailed SHM header/layout error, or None when valid.

        Keep this validation in one place so callers that only need a boolean can
        use is_valid(), while open() can expose the exact incompatible field.
        SHM layout compatibility is checked with magic, fixed structure sizes and
        mapped-size boundaries; SHM has no independent protocol version.
        """
        if self._header is None:
            return "shm header is not attached"
        if self._mmap is None:
            return "shm mapping is not attached"

        h = self._header

        if h.magic != K_SHM_MAGIC:
            return (
                "shm magic mismatch: "
                f"actual=0x{int(h.magic):08X}, expected=0x{K_SHM_MAGIC:08X}"
            )

        if h.header_size != SHARED_MEMORY_HEADER_SIZE:
            return (
                "shm header_size mismatch: "
                f"actual={int(h.header_size)}, expected={SHARED_MEMORY_HEADER_SIZE}"
            )

        if h.buffer_meta_size != SHM_BUFFER_META_SIZE:
            return (
                "shm buffer_meta_size mismatch: "
                f"actual={int(h.buffer_meta_size)}, expected={SHM_BUFFER_META_SIZE}"
            )

        if h.buffer_count != K_SHM_BUFFER_COUNT:
            return (
                "shm buffer_count mismatch: "
                f"actual={int(h.buffer_count)}, expected={K_SHM_BUFFER_COUNT}"
            )

        if h.payload_capacity == 0:
            return (
                "shm payload_capacity is 0; "
                f"width={int(h.width)}, height={int(h.height)}, stride={int(h.stride)}"
            )

        required_size = _compute_total_size(int(h.payload_capacity))
        if int(h.total_shm_size) != required_size:
            return (
                "shm total_shm_size mismatch: "
                f"actual={int(h.total_shm_size)}, expected={required_size}, "
                f"payload_capacity={int(h.payload_capacity)}"
            )

        if self._size < required_size:
            return (
                "shm mapped size is smaller than the layout requires: "
                f"actual={self._size}, required={required_size}, "
                f"header_total_shm_size={int(h.total_shm_size)}, "
                f"payload_capacity={int(h.payload_capacity)}, "
                f"header_size={SHARED_MEMORY_HEADER_SIZE}, "
                f"buffer_meta_size={SHM_BUFFER_META_SIZE}, "
                f"buffer_count={K_SHM_BUFFER_COUNT}"
            )

        return None

    def is_valid(self) -> bool:
        """Return the cached result of the one-time open/attach layout validation."""
        return self._layout_valid

    def read_latest(self) -> Optional["ReadView"]:
        """
        Begin reading the latest published frame.

        Returns a ReadView if a valid frame is available, or None if no frame
        has been published yet.

        The caller must call end_read() with the view's index when done.
        """
        if not self.is_valid():
            return None

        for retry in range(4):
            index = self._atomic_load_u32(self._field_ptr(self._header, 'published_index'))
            if index >= K_SHM_BUFFER_COUNT:
                return None

            meta = self._meta[index]
            if meta is None:
                return None
            if self._atomic_load_u32(self._field_ptr(meta, 'valid')) == 0:
                return None

            self._atomic_fetch_add_u32(self._field_ptr(meta, 'read_in_progress'), 1)

            index_after_ref = self._atomic_load_u32(self._field_ptr(self._header, 'published_index'))
            if index_after_ref != index or self._atomic_load_u32(self._field_ptr(meta, 'valid')) == 0:
                self._atomic_fetch_sub_u32(self._field_ptr(meta, 'read_in_progress'), 1)
                continue

            self._atomic_store_u32(self._field_ptr(self._header, 'consumer_index'), index)

            payload_addr = self._payload_ptr(index)
            width = self._atomic_load_u32(self._field_ptr(meta, 'width'))
            height = self._atomic_load_u32(self._field_ptr(meta, 'height'))
            stride = self._atomic_load_u32(self._field_ptr(meta, 'stride'))
            pixel_format_value = self._atomic_load_u32(self._field_ptr(meta, 'pixel_format'))
            payload_size = self._atomic_load_u32(self._field_ptr(meta, 'payload_size'))
            frame_id = self._atomic_load_u64(self._field_ptr(meta, 'frame_id'))
            timestamp_ns = self._atomic_load_u64(self._field_ptr(meta, 'timestamp_ns'))
            valid = self._atomic_load_u32(self._field_ptr(meta, 'valid')) != 0

            # Read valid_rect and destination position from the same published meta snapshot.
            vr_x = self._atomic_load_s32(self._field_ptr(meta, 'valid_rect_pos_x'))
            vr_y = self._atomic_load_s32(self._field_ptr(meta, 'valid_rect_pos_y'))
            vr_w = self._atomic_load_u32(self._field_ptr(meta, 'valid_rect_width'))
            vr_h = self._atomic_load_u32(self._field_ptr(meta, 'valid_rect_height'))
            dest_x = self._atomic_load_s32(self._field_ptr(meta, 'dest_pos_x'))
            dest_y = self._atomic_load_s32(self._field_ptr(meta, 'dest_pos_y'))

            try:
                pixel_format = PixelFormat(pixel_format_value)
            except ValueError:
                pixel_format = PixelFormat.UNKNOWN

            if payload_size > self._payload_capacity or height > self._header.height:
                self._atomic_fetch_sub_u32(self._field_ptr(meta, 'read_in_progress'), 1)
                return None

            return ReadView(
                index=index,
                payload=bytes(payload_addr[:payload_size]),
                width=width,
                height=height,
                stride=stride,
                pixel_format=pixel_format,
                payload_size=payload_size,
                frame_id=frame_id,
                timestamp_ns=timestamp_ns,
                dest_pos_x=dest_x,
                dest_pos_y=dest_y,
                valid=valid,
                valid_rect=Rect(x=vr_x, y=vr_y, width=vr_w, height=vr_h),
                meta=meta,
                header=self._header,
            )

        return None

    def end_read(self, index: int):
        """End reading for the given buffer index."""
        if not self.is_valid() or index >= K_SHM_BUFFER_COUNT:
            return
        meta = self._meta[index]
        if meta is None:
            return
        old = self._atomic_load_u32(self._field_ptr(meta, 'read_in_progress'))
        if old == 0:
            return
        self._atomic_fetch_sub_u32(self._field_ptr(meta, 'read_in_progress'), 1)

    def write_latest(self, data: bytes, frame_id: int, timestamp_ns: int,
                     width: int, height: int, stride: int, pixel_format: PixelFormat,
                     valid_rect: Optional[Rect] = None,
                     dest_pos_x: int = 0, dest_pos_y: int = 0) -> bool:
        """Write and publish one frame snapshot using the fixed SHM layout."""
        if not self.is_valid():
            return False

        try:
            pixel_format = PixelFormat(pixel_format)
        except (TypeError, ValueError):
            return False

        if pixel_format in (PixelFormat.UYVY, PixelFormat.YUYV):
            bytes_per_pixel = 2
        elif pixel_format == PixelFormat.BGRA8888:
            bytes_per_pixel = 4
        else:
            return False

        src_size = len(data) if data else 0
        if (src_size > self._payload_capacity or width <= 0 or height <= 0 or stride <= 0 or
                width > self._header.width or height > self._header.height or
                stride > self._header.stride):
            return False

        if width * bytes_per_pixel > stride or height * stride > src_size:
            return False

        if valid_rect is None:
            valid_rect = Rect()

        writable = self._choose_writable_buffer()
        if writable < 0:
            return False

        index = writable
        if src_size > 0:
            dst = self._payload_ptr(index)
            if dst is None:
                return False
            dst[:src_size] = data

        meta = self._meta[index]
        if meta is None:
            return False
        meta.frame_id = frame_id
        meta.timestamp_ns = timestamp_ns
        meta.width = width
        meta.height = height
        meta.stride = stride
        meta.pixel_format = int(pixel_format)
        meta.payload_size = src_size
        meta.valid_rect_pos_x = valid_rect.x
        meta.valid_rect_pos_y = valid_rect.y
        meta.valid_rect_width = valid_rect.width
        meta.valid_rect_height = valid_rect.height
        meta.dest_pos_x = dest_pos_x
        meta.dest_pos_y = dest_pos_y
        meta.valid = 1

        self._atomic_fetch_add_u32(self._field_ptr(meta, 'write_seq'), 1)
        self._atomic_store_u32(self._field_ptr(self._header, 'published_index'), index)
        return True

    def clear_all_buffers(self):
        """Clear all buffers and reset published/consumer indices."""
        if not self.is_valid():
            return
        for index in range(K_SHM_BUFFER_COUNT):
            meta = self._meta[index]
            if meta is not None:
                meta.valid = 0
                meta.width = 0
                meta.height = 0
                meta.stride = 0
                meta.pixel_format = int(PixelFormat.UNKNOWN)
                meta.payload_size = 0
                meta.valid_rect_pos_x = 0
                meta.valid_rect_pos_y = 0
                meta.valid_rect_width = 0
                meta.valid_rect_height = 0
                meta.dest_pos_x = 0
                meta.dest_pos_y = 0
                meta.frame_id = 0
                meta.timestamp_ns = 0
                meta.write_seq = 0
        self._atomic_store_u32(self._field_ptr(self._header, 'published_index'), 0)
        self._atomic_store_u32(self._field_ptr(self._header, 'consumer_index'), 0)

    # --- Internal methods ---

    def _open_internal(self, shm_name: str):
        """Open existing SHM and attach layout.

        Uses /dev/shm/ path directly since Python 3.13 removed os.shm_open.
        The shm_name should be like '/kavio_rx_xxx' or 'kavio_rx_xxx'.
        """
        # Ensure the name starts with /dev/shm/
        if shm_name.startswith('/'):
            shm_path = '/dev/shm' + shm_name
        else:
            shm_path = '/dev/shm/' + shm_name

        try:
            fd = os.open(shm_path, os.O_RDWR)
        except OSError as e:
            raise StatusError(Status.Error(StatusCode.SHM_ERROR,
                               f"open shm '{shm_path}' failed: {e}"))

        try:
            st = os.fstat(fd)
            total_size = st.st_size
            if total_size < MINIMUM_HEADER_SIZE:
                raise StatusError(Status.Error(
                    StatusCode.SHM_ERROR,
                    f"shm '{shm_name}' size is too small for this SDK layout: "
                    f"actual={total_size}, minimum={MINIMUM_HEADER_SIZE}, "
                    f"header_size={SHARED_MEMORY_HEADER_SIZE}, "
                    f"buffer_meta_size={SHM_BUFFER_META_SIZE}, "
                    f"buffer_count={K_SHM_BUFFER_COUNT}"
                ))

            self._mmap = mmap.mmap(fd, total_size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
            self._fd = fd
            self._size = total_size
            self._name = shm_name

            self._attach_layout()
            layout_error = self._layout_validation_error()
            if layout_error is not None:
                raise StatusError(Status.Error(
                    StatusCode.SHM_ERROR,
                    f"invalid shm header/layout for '{shm_name}': {layout_error}"
                ))
            self._layout_valid = True
        except StatusError:
            self.close()
            raise
        except Exception as e:
            self.close()
            raise StatusError(Status.Error(StatusCode.SHM_ERROR, f"open shm failed: {e}"))

    def _attach_layout(self):
        """Attach the typed pointers to the mmap'd memory."""
        buf = self._mmap
        if buf is None:
            raise StatusError(Status.Error(
                StatusCode.SHM_ERROR,
                "cannot attach shm layout: mmap object is not available"
            ))
        if self._size < MINIMUM_HEADER_SIZE:
            raise StatusError(Status.Error(
                StatusCode.SHM_ERROR,
                f"cannot attach shm layout: mapped_size={self._size}, "
                f"minimum={MINIMUM_HEADER_SIZE}"
            ))

        # Create SharedMemoryHeader at offset 0
        self._header = SharedMemoryHeader.from_buffer(buf)

        # Create ShmBufferMeta at offset sizeof(SharedMemoryHeader)
        meta_offset = SHARED_MEMORY_HEADER_SIZE
        self._meta = (
            ShmBufferMeta.from_buffer(buf, meta_offset),
            ShmBufferMeta.from_buffer(buf, meta_offset + SHM_BUFFER_META_SIZE),
        )

        self._payload_offset = MINIMUM_HEADER_SIZE
        self._payload_capacity = self._header.payload_capacity

    def _payload_ptr(self, index: int):
        """Get a writable memoryview for the payload at given index."""
        if self._mmap is None:
            return None
        offset = self._payload_offset + index * self._payload_capacity
        return memoryview(self._mmap)[offset:offset + self._payload_capacity]

    def _choose_writable_buffer(self) -> int:
        """
        Pick the writable buffer that is not currently published and not being read.
        If the preferred buffer is busy, fall back to the currently published buffer
        (acceptable in double-buffer mode — may overwrite a stale published frame).
        Returns -1 only if both buffers are read-locked (should not happen).
        """
        published = self._atomic_load_u32(self._field_ptr(self._header, 'published_index'))
        writable = 1 if published == 0 else 0
        meta = self._meta[writable]
        if meta is None:
            return -1
        if self._atomic_load_u32(self._field_ptr(meta, 'read_in_progress')) != 0:
            return -1
        return writable

    # All cross-process synchronization uses libatomic with GCC-compatible
    # acquire/release semantics. In particular, reference increments are
    # atomic RMW operations; a plain ctypes increment is not safe here.
    @staticmethod
    def _field_ptr(struct_instance, field_name: str):
        offset = getattr(struct_instance.__class__, field_name).offset
        return ctypes.c_void_p(ctypes.addressof(struct_instance) + offset)

    @staticmethod
    def _atomic_load_u32(ptr):
        return _atomic.load_u32(ptr)

    @staticmethod
    def _atomic_store_u32(ptr, value):
        _atomic.store_u32(ptr, value)

    @staticmethod
    def _atomic_load_s32(ptr):
        value = _atomic.load_u32(ptr)
        return value if value < 0x80000000 else value - 0x100000000

    @staticmethod
    def _atomic_load_u64(ptr):
        return _atomic.load_u64(ptr)

    @staticmethod
    def _atomic_fetch_add_u32(ptr, value):
        return _atomic.fetch_add_u32(ptr, value)

    @staticmethod
    def _atomic_fetch_sub_u32(ptr, value):
        return _atomic.fetch_sub_u32(ptr, value)



class ReadView:
    """
    Result of a read_latest() call.

    Mirrors C++ kavio::FrameShm::ReadView.
    """

    __slots__ = (
        'index', 'payload', 'width', 'height', 'stride', 'pixel_format',
        'payload_size', 'frame_id', 'timestamp_ns', 'dest_pos_x', 'dest_pos_y',
        'valid', 'valid_rect', 'meta', 'header'
    )

    def __init__(self, index: int, payload: bytes, width: int, height: int,
                 stride: int, pixel_format: PixelFormat, payload_size: int,
                 frame_id: int, timestamp_ns: int, dest_pos_x: int, dest_pos_y: int,
                 valid: bool, valid_rect: 'Rect',
                 meta: ShmBufferMeta, header: SharedMemoryHeader):
        self.index = index
        self.payload = payload
        self.width = width
        self.height = height
        self.stride = stride
        self.pixel_format = pixel_format
        self.payload_size = payload_size
        self.frame_id = frame_id
        self.timestamp_ns = timestamp_ns
        self.dest_pos_x = dest_pos_x
        self.dest_pos_y = dest_pos_y
        self.valid = valid
        self.valid_rect = valid_rect
        self.meta = meta
        self.header = header
