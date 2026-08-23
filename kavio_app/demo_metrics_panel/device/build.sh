#!/usr/bin/env bash
set -euo pipefail

# 该脚本不是编译器：Python Device Component 以源码形式交付。这里仅做语法检查、
# SDK/依赖暂存和包目录清理，最终由统一 package 工具生成 .kap。

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
REPOSITORY_DIR="$(cd "${PROJECT_DIR}/../.." && pwd)"
SDK_DIR="${REPOSITORY_DIR}/kavio_sdk/device/py/kavio_sdk"
OUTPUT_DIR="${PROJECT_DIR}/output/device"

usage() {
    cat <<'USAGE'
Usage: ./build.sh [--clean] [--sdk-dir <kavio_sdk/device/py/kavio_sdk>]

Python Demo 不编译 ARM 可执行文件，只做语法检查并暂存源码和 Kavio Python SDK。
USAGE
}

main() {
    local sdk_dir="${SDK_DIR}"
    local sdk_requirements=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -c|--clean)
                rm -rf "${OUTPUT_DIR}"
                return
                ;;
            --sdk-dir)
                shift
                [[ $# -gt 0 ]] || { echo "--sdk-dir requires a path" >&2; exit 2; }
                sdk_dir="$(cd "$1" && pwd)"
                ;;
            -h|--help)
                usage
                return
                ;;
            *)
                echo "unsupported argument: $1" >&2
                usage >&2
                exit 2
                ;;
        esac
        shift
    done

    [[ -f "${sdk_dir}/__init__.py" ]] || {
        echo "Kavio Python SDK not found: ${sdk_dir}" >&2
        echo "Use --sdk-dir to specify the current kavio_sdk/device/py/kavio_sdk directory." >&2
        exit 1
    }

    sdk_requirements="$(dirname "${sdk_dir}")/requirements.txt"
    [[ -f "${sdk_requirements}" ]] || {
        echo "Kavio Python SDK requirements not found: ${sdk_requirements}" >&2
        exit 1
    }

    # App builds consume the Python SDK artifacts produced by kavio_sdk/device/build.sh.
    # They must not rebuild the SDK as part of the App build.
    # Fail the App build if a custom/prebuilt Python SDK does not actually
    # carry the current session-version field or cannot preserve its presence on
    # the protobuf wire.
    PYTHONPATH="$(dirname "${sdk_dir}")${PYTHONPATH:+:${PYTHONPATH}}" python3 - <<'PYSDK'
from kavio_sdk.proto_py.ipc_pb2 import ReqSessionCreatePb
from kavio_sdk.sdk_version import (
    SDK_VERSION_MAJOR,
    SDK_VERSION_MINOR,
    SDK_VERSION_PATCH,
)

if "sdk_version" not in ReqSessionCreatePb.DESCRIPTOR.fields_by_name:
    raise SystemExit(
        "Kavio Python SDK is missing ReqSessionCreatePb.sdk_version; "
        "run kavio_sdk/device/build.sh before building the App"
    )

req = ReqSessionCreatePb()
req.sdk_version.major = SDK_VERSION_MAJOR
req.sdk_version.minor = SDK_VERSION_MINOR
req.sdk_version.patch = SDK_VERSION_PATCH
wire = req.SerializeToString()
parsed = ReqSessionCreatePb()
parsed.ParseFromString(wire)
if not parsed.HasField("sdk_version"):
    raise SystemExit("Kavio Python SDK sdk_version protobuf round-trip validation failed")
PYSDK

    python3 -m py_compile \
        "${SCRIPT_DIR}/main.py" \
        "${SCRIPT_DIR}/protocol.py" \
        "${SCRIPT_DIR}/renderer.py" \
        "${SCRIPT_DIR}/kavio_bridge.py"

    rm -rf "${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}/kavio_sdk"
    cp -f "${SCRIPT_DIR}/main.py" "${OUTPUT_DIR}/"
    cp -f "${SCRIPT_DIR}/protocol.py" "${OUTPUT_DIR}/"
    cp -f "${SCRIPT_DIR}/renderer.py" "${OUTPUT_DIR}/"
    cp -f "${SCRIPT_DIR}/kavio_bridge.py" "${OUTPUT_DIR}/"
    # The package-level requirements file is the complete runtime dependency set:
    # Kavio Python SDK dependencies first, followed by App-specific dependencies.
    # pip resolves both sets together and will fail explicitly on incompatible constraints.
    {
        cat "${sdk_requirements}"
        printf '\n'
        cat "${SCRIPT_DIR}/requirements.txt"
    } > "${OUTPUT_DIR}/requirements.txt"
    cp -a "${sdk_dir}/." "${OUTPUT_DIR}/kavio_sdk/"

    # py_compile is only a syntax check. Bytecode caches are host-Python-specific
    # build artifacts and must not be shipped in the App package.
    find "${SCRIPT_DIR}" -type d -name '__pycache__' -prune -exec rm -rf {} +
    find "${OUTPUT_DIR}" -type d -name '__pycache__' -prune -exec rm -rf {} +
    find "${OUTPUT_DIR}" -type f -name '*.pyc' -delete

    chmod 0755 "${OUTPUT_DIR}/main.py"
    echo "Staged Python Device Component: ${OUTPUT_DIR}"
}

main "$@"
