#!/usr/bin/env python3
"""Generate Python SDK version constants from KavioSdkVersion.h."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


def read_macro(text: str, name: str) -> str:
    match = re.search(rf"^\s*#define\s+{re.escape(name)}\s+([^\s/]+)", text, re.MULTILINE)
    if not match:
        raise RuntimeError(f"missing macro: {name}")
    return match.group(1)


def parse_uint(value: str) -> int:
    return int(value.rstrip("uUlL"), 0)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    text = Path(args.header).read_text(encoding="utf-8")
    major = parse_uint(read_macro(text, "KAVIO_SDK_VERSION_MAJOR"))
    minor = parse_uint(read_macro(text, "KAVIO_SDK_VERSION_MINOR"))
    patch = parse_uint(read_macro(text, "KAVIO_SDK_VERSION_PATCH"))

    generated = (
        "# Auto-generated from KavioSdkVersion.h. Do not edit manually.\n\n"
        f"SDK_VERSION_MAJOR = {major}\n"
        f"SDK_VERSION_MINOR = {minor}\n"
        f"SDK_VERSION_PATCH = {patch}\n"
        'SDK_VERSION = f"{SDK_VERSION_MAJOR}.{SDK_VERSION_MINOR}.{SDK_VERSION_PATCH}"\n'
    )
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(generated, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
