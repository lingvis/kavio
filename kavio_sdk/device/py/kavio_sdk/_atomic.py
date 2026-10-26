"""Linux GCC-compatible atomic operations for the shared-memory ABI.

All accesses use libatomic's GCC __atomic entry points. The GIL does not
synchronize with the Platform process. Fail explicitly when libatomic is
unavailable rather than silently falling back to non-atomic ctypes writes.
"""

import ctypes
import ctypes.util

_ACQUIRE = 2
_RELEASE = 3
_ACQ_REL = 4

_name = ctypes.util.find_library("atomic")
if not _name:
    raise RuntimeError("Kavio shared memory requires libatomic")
_lib = ctypes.CDLL(_name)


def _symbol(name, result, args):
    fn = getattr(_lib, name)
    fn.argtypes = args
    fn.restype = result
    return fn

_load4 = _symbol("__atomic_load_4", ctypes.c_uint32, [ctypes.c_void_p, ctypes.c_int])
_load8 = _symbol("__atomic_load_8", ctypes.c_uint64, [ctypes.c_void_p, ctypes.c_int])
_store4 = _symbol("__atomic_store_4", None, [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int])
_add4 = _symbol("__atomic_fetch_add_4", ctypes.c_uint32,
               [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int])
_sub4 = _symbol("__atomic_fetch_sub_4", ctypes.c_uint32,
               [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int])


def load_u32(address):
    return _load4(address, _ACQUIRE)


def load_u64(address):
    return _load8(address, _ACQUIRE)


def store_u32(address, value):
    _store4(address, value, _RELEASE)


def fetch_add_u32(address, value):
    return _add4(address, value, _ACQ_REL)


def fetch_sub_u32(address, value):
    return _sub4(address, value, _ACQ_REL)
