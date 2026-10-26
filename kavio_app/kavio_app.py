#!/usr/bin/env python3
"""Inspect and package Kavio Apps."""

from __future__ import annotations

import argparse
import importlib.util
import json
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys
import tempfile
from typing import Optional

ROOT = Path(__file__).resolve().parent
TOOLS_DIR = ROOT / "tools"
PACKAGE_TOOL = TOOLS_DIR / "build_package.py"
PACKAGE_CONFIG_NAME = "kavio_package.json"
APP_ICON_WIDTH = 64
APP_ICON_HEIGHT = 64
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


class ProjectError(ValueError):
    pass


def project_dir(project_name: str) -> Path:
    if not project_name or Path(project_name).name != project_name:
        raise ProjectError("project_name must be a direct child directory name")
    path = ROOT / project_name
    if not path.is_dir():
        raise ProjectError(f"Kavio App does not exist: {project_name}")
    return path


def load_config(project_name: str) -> tuple[Path, Path, Path, dict[str, object]]:
    directory = project_dir(project_name)
    package_dir = directory / "package"
    output_dir = directory / "output"
    config_path = package_dir / PACKAGE_CONFIG_NAME
    if not config_path.is_file():
        raise ProjectError(f"package config not found: {config_path}")
    try:
        value = json.loads(config_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ProjectError(f"cannot read {config_path}: {exc}") from exc
    if not isinstance(value, dict):
        raise ProjectError(f"{config_path} must contain a JSON object")
    return directory, package_dir, output_dir, value


def safe_relative_path(value: object, field: str) -> PurePosixPath:
    if not isinstance(value, str) or not value:
        raise ProjectError(f"{field} must be a non-empty string")
    path = PurePosixPath(value)
    if path.is_absolute() or ".." in path.parts or "\\" in value or ":" in value:
        raise ProjectError(f"{field} must be a safe package-relative path")
    return path


def joined(root: Path, relative: PurePosixPath) -> Path:
    return root.joinpath(*relative.parts)


def component_entry(output_dir: Path, section: dict[str, object], entry_key: str,
                    field_prefix: str) -> Path:
    directory = safe_relative_path(section.get("directory"), f"{field_prefix}.directory")
    entry = safe_relative_path(section.get(entry_key), f"{field_prefix}.{entry_key}")
    return joined(output_dir, directory / entry)


def validate_icon_for_status(package_dir: Path, config: dict[str, object]) -> str:
    icon = config.get("icon")
    if icon is None or icon == "":
        return "icon is required and must reference a 64x64 PNG file"
    if not isinstance(icon, str):
        return "icon must be a string"
    try:
        path = joined(package_dir, safe_relative_path(icon, "icon"))
    except ProjectError as exc:
        return str(exc)
    if not path.is_file():
        return f"icon file not found: {path}"
    if path.suffix.lower() != ".png":
        return "icon must be a PNG file"
    try:
        header = path.read_bytes()[:24]
    except OSError as exc:
        return f"cannot read icon: {exc}"
    if len(header) < 24 or header[:8] != PNG_SIGNATURE or header[12:16] != b"IHDR":
        return "icon is not a valid PNG file"
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    if width != APP_ICON_WIDTH or height != APP_ICON_HEIGHT:
        return (f"icon must be exactly {APP_ICON_WIDTH}x{APP_ICON_HEIGHT} pixels; "
                f"got {width}x{height}")
    return ""


def required_files(package_dir: Path, output_dir: Path,
                   config: dict[str, object]) -> list[tuple[str, Path]]:
    result: list[tuple[str, Path]] = []

    device = config.get("device")
    if isinstance(device, dict):
        result.append(("device entry", component_entry(
            output_dir, device, "entry", "device")))

    host = config.get("host")
    if isinstance(host, dict):
        result.append(("host binary", component_entry(
            output_dir, host, "binary", "host")))

    result.append(("schema_description.json", package_dir / "schema_description.json"))

    icon = config.get("icon")
    if isinstance(icon, str) and icon:
        result.append(("icon", joined(package_dir, safe_relative_path(icon, "icon"))))
    return result


def iter_projects() -> list[tuple[str, dict[str, object]]]:
    found: list[tuple[str, dict[str, object]]] = []
    for child in sorted(ROOT.iterdir(), key=lambda path: path.name):
        if not child.is_dir():
            continue
        config_path = child / "package" / PACKAGE_CONFIG_NAME
        if not config_path.is_file():
            continue
        try:
            config = json.loads(config_path.read_text(encoding="utf-8-sig"))
        except Exception:
            config = {}
        found.append((child.name, config if isinstance(config, dict) else {}))
    return found


def show_list() -> int:
    projects = iter_projects()
    if not projects:
        print("No Kavio Apps found.")
        return 0
    print(f"{'APP':36} {'APP_ID':48} COMPONENTS")
    for name, config in projects:
        app_id = str(config.get("appId", "<invalid>"))
        components = ["device"]
        if isinstance(config.get("host"), dict):
            components.append("host")
            if config["host"].get("panel") is True:
                components.append("panel")
        if (ROOT / name / "package" / "schema_description.json").is_file():
            try:
                if json.loads((ROOT / name / "package" / "schema_description.json").read_text(encoding="utf-8-sig")) != {}:
                    components.append("config")
            except (ValueError, OSError):
                components.append("invalid-schema")
        print(f"{name:36} {app_id:48} {','.join(components)}")
    return 0


def reject_symlinks(root: Path, label: str) -> None:
    if not root.exists():
        return
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ProjectError(f"{label} must not contain symbolic links: {path}")


def validate_project_layout(package_dir: Path) -> None:
    if package_dir.is_symlink():
        raise ProjectError(f"package metadata directory must not be a symbolic link: {package_dir}")
    for runtime_name in ("device", "host"):
        runtime_path = package_dir / runtime_name
        if runtime_path.exists():
            raise ProjectError(
                f"runtime directory is not allowed under package/: {runtime_path}; "
                "build artifacts belong in the project output/ directory"
            )


def validate_config_for_status(package_dir: Path) -> str:
    try:
        spec = importlib.util.spec_from_file_location("kavio_build_package", PACKAGE_TOOL)
        if spec is None or spec.loader is None:
            return "cannot load packaging validation tool"
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        module.validate_schema_description(package_dir)
        return ""
    except Exception as exc:
        return str(exc)


def show_status(project_name: str) -> int:
    directory, package_dir, output_dir, config = load_config(project_name)
    validate_project_layout(package_dir)
    reject_symlinks(package_dir, "package metadata directory")
    print(f"App:       {project_name}")
    print(f"appId:  {config.get('appId', '<invalid>')}")
    print(f"version:   {config.get('version', '<invalid>')}")
    print(f"metadata:  {package_dir}")
    print(f"artifacts: {output_dir}")
    print(f"dist:      {directory / 'dist'}")

    failed = False
    for label, path in required_files(package_dir, output_dir, config):
        exists = path.is_file()
        print(f"{'OK' if exists else 'MISSING':8} {label:20} {path}")
        failed = failed or not exists

    config_error = validate_config_for_status(package_dir)
    if config_error:
        print(f"INVALID  {'config':20} {config_error}")
        failed = True

    icon_error = validate_icon_for_status(package_dir, config)
    if icon_error:
        print(f"INVALID  {'icon':20} {icon_error}")
        failed = True
    elif isinstance(config.get("icon"), str) and config.get("icon"):
        print(f"OK       {'icon validation':20} {APP_ICON_WIDTH}x{APP_ICON_HEIGHT} PNG")
    return 2 if failed else 0


def copy_component(output_dir: Path, stage_dir: Path, section: dict[str, object],
                   field_prefix: str) -> None:
    relative = safe_relative_path(section.get("directory"), f"{field_prefix}.directory")
    source = joined(output_dir, relative)
    if not source.is_dir():
        raise ProjectError(f"{field_prefix} output directory not found: {source}")
    if source.is_symlink():
        raise ProjectError(f"{field_prefix} output directory must not be a symbolic link: {source}")
    reject_symlinks(source, f"{field_prefix} output")
    shutil.copytree(source, joined(stage_dir, relative), dirs_exist_ok=True)


def build_package(project_name: str, output: Optional[Path]) -> int:
    directory, package_dir, output_dir, config = load_config(project_name)

    # App Configuration is package metadata, not a build artifact. Validate it first so a
    # declared-but-missing/invalid Config Definition produces an explicit packaging error.
    config_error = validate_config_for_status(package_dir)
    if config_error:
        raise ProjectError("App Configuration validation failed; packaging stopped:\n  " + config_error)
    icon_error = validate_icon_for_status(package_dir, config)
    if icon_error:
        raise ProjectError("Icon validation failed; packaging stopped:\n  " + icon_error)

    missing = [(label, path) for label, path in required_files(package_dir, output_dir, config)
               if not path.is_file()]
    if missing:
        lines = "\n".join(f"  {label}: {path}" for label, path in missing)
        raise ProjectError(
            "App artifacts are not ready; build all declared components first:\n" + lines
        )

    version = str(config.get("version", "unknown"))
    if output is None:
        output = directory / "dist" / f"{project_name}-{version}.kap"
    elif not output.is_absolute():
        output = Path.cwd() / output
    output = output.resolve()

    validate_project_layout(package_dir)
    reject_symlinks(package_dir, "package metadata directory")

    with tempfile.TemporaryDirectory(prefix=f"kavio-{project_name}-") as temp_value:
        stage_dir = Path(temp_value) / "package"
        shutil.copytree(package_dir, stage_dir)

        device = config.get("device")
        if isinstance(device, dict):
            copy_component(output_dir, stage_dir, device, "device")

        host = config.get("host")
        if isinstance(host, dict):
            copy_component(output_dir, stage_dir, host, "host")

        command = [
            sys.executable,
            str(PACKAGE_TOOL),
            "--source-dir",
            str(stage_dir),
            "--output",
            str(output),
        ]
        return subprocess.call(command)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Kavio App inspection and packaging utility"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    subparsers.add_parser("list", help="list all Kavio Apps")

    status_parser = subparsers.add_parser("status", help="check App artifacts")
    status_parser.add_argument("project_name")

    package_parser = subparsers.add_parser(
        "package", help="create an unsigned .kap (Kavio App Package)"
    )
    package_parser.add_argument("project_name")
    package_parser.add_argument("--output", type=Path)

    args = parser.parse_args()
    try:
        if args.command == "list":
            return show_list()
        if args.command == "status":
            return show_status(args.project_name)
        if args.command == "package":
            return build_package(args.project_name, args.output)
        raise ProjectError(f"unsupported command: {args.command}")
    except (ProjectError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
