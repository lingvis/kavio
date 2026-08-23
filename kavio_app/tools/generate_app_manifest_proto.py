#!/usr/bin/env python3
"""Regenerate the Python binding for app_manifest.proto used by the packager."""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from typing import Optional

ROOT = Path(__file__).resolve().parents[2]
PROTO_PATH = ROOT / "kavio_platform" / "proto" / "app_manifest.proto"
OUTPUT_DIR = Path(__file__).resolve().parent
OUTPUT_PATH = OUTPUT_DIR / "app_manifest_pb2.py"
HASH_SYMBOL = "KAVIO_SOURCE_PROTO_SHA256"


def find_protoc(explicit: Optional[str]) -> str:
    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not path.is_file():
            raise FileNotFoundError(f"protoc not found: {path}")
        return str(path)

    found = shutil.which("protoc")
    if found:
        return found

    candidates = [
        ROOT / "kavio_platform" / "build" / "protobuf_build" / "protoc",
        ROOT / "kavio_platform" / "build" / "protobuf_build" / "Debug" / "protoc.exe",
        ROOT / "kavio_sdk" / "device" / "build" / "protobuf_build" / "protoc",
        ROOT / "kavio_sdk" / "device" / "build" / "protobuf_build" / "Debug" / "protoc.exe",
    ]
    for path in candidates:
        if path.is_file():
            return str(path)
    raise FileNotFoundError(
        "protoc was not found. Build the bundled protobuf tools first, add protoc to PATH, "
        "or pass --protoc <path>."
    )


def inject_source_hash(generated: str, digest: str) -> str:
    lines = generated.splitlines()
    insert_at = 0
    while insert_at < len(lines) and (
        lines[insert_at].startswith("#") or not lines[insert_at].strip()
    ):
        insert_at += 1
    lines.insert(insert_at, f'{HASH_SYMBOL} = "{digest}"')
    lines.insert(insert_at + 1, "")
    return "\n".join(lines) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--protoc", help="explicit protoc executable path")
    args = parser.parse_args()

    try:
        protoc = find_protoc(args.protoc)
        proto_bytes = PROTO_PATH.read_bytes()
        digest = hashlib.sha256(proto_bytes).hexdigest()
        OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

        with tempfile.TemporaryDirectory(prefix="kavio-app-manifest-proto-") as temp_value:
            temp_dir = Path(temp_value)
            command = [
                protoc,
                f"--proto_path={PROTO_PATH.parent}",
                f"--python_out={temp_dir}",
                str(PROTO_PATH),
            ]
            subprocess.run(command, check=True)
            generated_path = temp_dir / "app_manifest_pb2.py"
            generated = generated_path.read_text(encoding="utf-8")
            OUTPUT_PATH.write_text(inject_source_hash(generated, digest), encoding="utf-8")

        print(f"Generated: {OUTPUT_PATH}")
        print(f"Source SHA-256: {digest}")
        return 0
    except (OSError, subprocess.CalledProcessError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
