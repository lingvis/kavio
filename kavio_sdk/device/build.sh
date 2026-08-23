#!/usr/bin/env bash
set -euo pipefail

# Single build entry for Kavio Device SDK.
# One invocation builds the C++ SDK and regenerates the Python SDK artifacts
# (sdk_version.py and *_pb2.py) from the same source revision.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
SDK_TARGET="kavio_device_sdk_package"
PY_PACKAGE_DIR="${SCRIPT_DIR}/py/kavio_sdk"
PROTO_PY_DIR="${PY_PACKAGE_DIR}/proto_py"

usage() {
    cat <<'USAGE'
Usage:
    ./build.sh [-r|--release]
    ./build.sh -c|--clean
    ./build.sh -h|--help

Builds the complete Kavio Device SDK:
  - C++ Device SDK static libraries and CMake package
  - Python SDK version module
  - Python protobuf modules

Options:
    -r, --release  Configure with Release build type
    -c, --clean    Clean C++ build output and generated Python SDK artifacts
    -h, --help     Show this help
USAGE
}

clean_generated_python() {
    rm -f "${PY_PACKAGE_DIR}/sdk_version.py"
    rm -f "${PROTO_PY_DIR}"/*_pb2.py
    find "${PY_PACKAGE_DIR}" -type d -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
}

clean_build() {
    rm -rf "${BUILD_DIR}"
    clean_generated_python
    echo "Cleaned Device SDK build output and generated Python SDK artifacts"
}

validate_generated_python_sdk() {
    PYTHONPATH="${SCRIPT_DIR}/py${PYTHONPATH:+:${PYTHONPATH}}" python3 - <<'PY'
from kavio_sdk.proto_py.ipc_pb2 import ReqSessionCreatePb
from kavio_sdk.sdk_version import (
    SDK_VERSION_MAJOR,
    SDK_VERSION_MINOR,
    SDK_VERSION_PATCH,
)

field = ReqSessionCreatePb.DESCRIPTOR.fields_by_name.get("sdk_version")
if field is None:
    raise SystemExit(
        "generated ipc_pb2.py is stale: ReqSessionCreatePb.sdk_version is missing"
    )

req = ReqSessionCreatePb()
req.sdk_version.major = SDK_VERSION_MAJOR
req.sdk_version.minor = SDK_VERSION_MINOR
req.sdk_version.patch = SDK_VERSION_PATCH

wire = req.SerializeToString()
parsed = ReqSessionCreatePb()
parsed.ParseFromString(wire)
if not parsed.HasField("sdk_version"):
    raise SystemExit("generated Python protobuf failed sdk_version presence round-trip")
if (
    parsed.sdk_version.major != SDK_VERSION_MAJOR
    or parsed.sdk_version.minor != SDK_VERSION_MINOR
    or parsed.sdk_version.patch != SDK_VERSION_PATCH
):
    raise SystemExit("generated Python protobuf failed sdk_version value round-trip")

print(
    "Validated Python SDK session version payload: "
    f"{SDK_VERSION_MAJOR}.{SDK_VERSION_MINOR}.{SDK_VERSION_PATCH}"
)
PY
}

main() {
    local build_type="Debug"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            -r|--release)
                build_type="Release"
                ;;
            -c|--clean)
                clean_build
                return
                ;;
            -h|--help)
                usage
                return
                ;;
            *)
                echo "unsupported option: $1" >&2
                usage >&2
                exit 1
                ;;
        esac
        shift
    done

    mkdir -p "${PROTO_PY_DIR}"

    # Generated Python files are build products. Remove them first so a copied
    # repository cannot accidentally reuse stale files from another SDK revision.
    clean_generated_python

    echo "Configuring ${SDK_TARGET} (${build_type})"
    cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE="${build_type}"

    # kavio_device_sdk_package depends on kavio_sdk, and kavio_sdk depends on
    # kavio_sdk_proto_py. One target therefore builds C++, regenerates Python
    # artifacts and refreshes KavioDeviceSdkConfig.cmake on every SDK build.
    echo "Building complete Device SDK"
    cmake --build "${BUILD_DIR}" --target "${SDK_TARGET}" -j"$(nproc)"

    validate_generated_python_sdk

    echo "Build completed"
    echo "Build type: ${build_type}"
    echo "Library directory: ${BUILD_DIR}/lib"
    echo "CMake package: ${BUILD_DIR}/KavioDeviceSdkConfig.cmake"
    echo "Python SDK directory: ${SCRIPT_DIR}/py/kavio_sdk"
}

main "$@"
