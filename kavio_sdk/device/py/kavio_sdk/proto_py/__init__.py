"""Generated protobuf modules for the Kavio Python SDK.

The modules in this directory are generated from ``kavio_sdk/device/proto/*.proto``.
Do not edit ``*_pb2.py`` files manually; run ``kavio_sdk/device/build.sh`` to regenerate them.
"""

from __future__ import annotations

import os
import sys

# protoc generates sibling imports such as ``import input_pb2``. Add the
# generated directory to sys.path so those imports resolve inside this package.
_proto_dir = os.path.dirname(os.path.abspath(__file__))
if _proto_dir not in sys.path:
    sys.path.insert(0, _proto_dir)

from . import input_defs_pb2
from . import input_pb2
from . import runtime_data_pb2
from . import user_config_pb2
from . import ipc_pb2

__all__ = [
    "input_defs_pb2",
    "input_pb2",
    "runtime_data_pb2",
    "user_config_pb2",
    "ipc_pb2",
]
