#!/usr/bin/env bash
set -euo pipefail

# 该 Demo 默认校验 AArch64 产物，防止在 x86 Linux 上误把本机可执行文件暂存进App 包。
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
OUTPUT_DIR="${PROJECT_DIR}/output/device"
TARGET_NAME="demo_communication"

usage() {
    cat <<'USAGE'
Usage: ./build.sh [--release] [--clean] [--toolchain <file>] [--skip-arch-check]
USAGE
}
resolve_file() {
    [[ "$1" = /* ]] && { printf '%s\n' "$1"; return; }
    local d; d="$(cd "$(dirname "$1")" && pwd)"; printf '%s/%s\n' "$d" "$(basename "$1")"
}
verify_aarch64() {
    local executable="$1"
    if command -v file >/dev/null 2>&1; then
        local description; description="$(file -b "${executable}")"
        [[ "${description}" == *"ARM aarch64"* || "${description}" == *"AArch64"* ]] && return 0
        echo "refusing to stage a non-AArch64 executable: ${description}" >&2; return 1
    fi
    if command -v readelf >/dev/null 2>&1; then
        local machine; machine="$(readelf -h "${executable}" 2>/dev/null | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
        [[ "${machine}" == *"AArch64"* ]] && return 0
        echo "refusing to stage a non-AArch64 executable: ${machine:-unknown}" >&2; return 1
    fi
    [[ "$(uname -m 2>/dev/null || true)" =~ ^(aarch64|arm64)$ ]] && return 0
    echo "cannot verify AArch64 output; use --skip-arch-check only after external verification" >&2
    return 1
}
clean() { rm -rf "${BUILD_DIR}"; rm -f "${OUTPUT_DIR}/${TARGET_NAME}"; }
main() {
    local build_type=Debug toolchain='' skip=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -r|--release) build_type=Release ;;
            -c|--clean) clean; return ;;
            --toolchain) shift; [[ $# -gt 0 ]] || { echo "--toolchain requires a path" >&2; exit 2; }; toolchain="$(resolve_file "$1")"; [[ -f "${toolchain}" ]] || { echo "toolchain not found: ${toolchain}" >&2; exit 2; } ;;
            --skip-arch-check) skip=true ;;
            -h|--help) usage; return ;;
            *) echo "unsupported argument: $1" >&2; usage >&2; exit 2 ;;
        esac
        shift
    done
    local args=(-S "${SCRIPT_DIR}" -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE="${build_type}")
    [[ -z "${toolchain}" ]] || args+=("-DCMAKE_TOOLCHAIN_FILE=${toolchain}")
    cmake "${args[@]}"
    local jobs=1; command -v nproc >/dev/null 2>&1 && jobs="$(nproc)"
    cmake --build "${BUILD_DIR}" --target "${TARGET_NAME}" -j"${jobs}"
    local executable="${BUILD_DIR}/bin/${TARGET_NAME}"
    [[ -f "${executable}" ]] || { echo "executable not found: ${executable}" >&2; exit 1; }
    [[ "${skip}" == true ]] || verify_aarch64 "${executable}"
    mkdir -p "${OUTPUT_DIR}"
    cp -f "${executable}" "${OUTPUT_DIR}/${TARGET_NAME}"
    chmod 0755 "${OUTPUT_DIR}/${TARGET_NAME}"
    echo "Staged: ${OUTPUT_DIR}/${TARGET_NAME}"
}
main "$@"
