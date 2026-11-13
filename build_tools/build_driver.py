#!/usr/bin/env python3
"""Kavio Device SDK project build, packaging and project creation tool."""

from __future__ import annotations

import argparse
import ast
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import time
import zipfile
from typing import Any

ROOT = Path(__file__).resolve().parent.parent
APP_ROOT = ROOT / "kavio_app"
SDK_SOURCE_DIR = ROOT / "kavio_sdk" / "device"
PACKAGE_TOOL = APP_ROOT / "tools" / "build_package.py"
PROJECT_SCHEMA = APP_ROOT / "tools" / "kavio_project.schema.json"
DEPENDENCY_CHECKER = ROOT / "build_tools" / "check_dependencies.py"
BUILD_ROOT = ROOT / "build"
PROJECT_CONFIG_NAME = "kavio_project.json"
TEMPLATE_CONFIG_NAME = "kavio_project.json.template"
TEMPLATE_ROOT = APP_ROOT / "Templates"
SUPPORTED_LANGUAGES = {"cpp", "python", "lua"}

DEVELOPER_SOCKET = "/run/kavio/developer.sock"
DEVELOPER_PROTOCOL_MAGIC = b"KDEV"
DEVELOPER_PROTOCOL_VERSION = 1
DEVELOPER_RESPONSE_BIT = 0x8000
DEVELOPER_FRAME_HEADER = struct.Struct("!4sHHIiII")
DEVELOPER_CHUNK_SIZE = 1024 * 1024

DEV_CMD_DEPLOY_BEGIN = 1
DEV_CMD_DEPLOY_CHUNK = 2
DEV_CMD_DEPLOY_COMMIT = 3
DEV_CMD_DEPLOY_ABORT = 4
DEV_CMD_START_APP = 5
DEV_CMD_STOP_APP = 6
DEV_CMD_GET_APP_LOG_PATH = 7


class BuildError(RuntimeError):
    pass


ROOT_FIELDS = {"projectVersion", "language", "build", "package"}
BUILD_FIELDS = {"source", "target"}
PACKAGE_FIELDS = {"appId", "version", "name", "description", "device", "host"}
DEVICE_FIELDS = {"runtime", "entry", "arguments", "python"}
HOST_FIELDS = {"binary", "panel"}
PYTHON_FIELDS = {"pythonVersion", "installMode", "environmentArchive", "environmentDefinition", "requirements"}


def run(command: list[str], *, cwd: Path | None = None) -> None:
    """Run a build command with live stdout/stderr forwarding and an explicit failure."""
    printable = " ".join(subprocess.list2cmdline([part]) for part in command)
    print(f"+ {printable}", flush=True)
    completed = subprocess.run(command, cwd=cwd, check=False)
    if completed.returncode != 0:
        raise BuildError(
            f"command failed with exit code {completed.returncode}: {printable}"
        )


def require_string(value: object, field: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or (not allow_empty and not value.strip()):
        suffix = "a string" if allow_empty else "a non-empty string"
        raise BuildError(f"{field} must be {suffix}")
    return value.strip() if not allow_empty else value


def read_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildError(f"cannot read {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise BuildError(f"{path} must contain a JSON object")
    return value


def validate_keys(value: dict[str, Any], allowed: set[str], field: str) -> None:
    unknown = sorted(set(value) - allowed)
    if unknown:
        raise BuildError(f"{field} contains unsupported fields: {', '.join(unknown)}")


def safe_relative(base: Path, relative: str, field: str) -> Path:
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts:
        raise BuildError(f"{field} must be a safe path relative to {base}")
    return base / path


def resolve_under(base: Path, path: Path, field: str) -> Path:
    resolved_base = base.resolve()
    resolved = path.resolve()
    try:
        resolved.relative_to(resolved_base)
    except ValueError as exc:
        raise BuildError(f"{field} must stay inside {resolved_base}") from exc
    return resolved


def build_type(release: bool) -> str:
    return "Release" if release else "Debug"


def config_key(release: bool) -> str:
    return "release" if release else "debug"


def job_count(value: int | None) -> int:
    if value is not None:
        if value < 1:
            raise BuildError("--jobs must be >= 1")
        return value
    return max(1, os.cpu_count() or 1)


def infer_builder(language: str) -> str:
    return {"cpp": "cmake", "python": "python", "lua": "lua"}[language]


def infer_dependencies(language: str) -> list[str]:
    # Language runtime is mandatory; UI is determined from the built App.
    if language == "cpp":
        return ["app_runtime"]
    if language == "python":
        return ["app_runtime", "python_binding"]
    if language == "lua":
        return ["app_runtime", "lua_runtime", "lua_binding"]
    raise BuildError(f"unsupported language: {language}")


def python_uses_ui(source: Path) -> bool:
    # Inspect all bundled Python source, not just the entrypoint.
    for path in source.rglob("*.py"):
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except (OSError, UnicodeError, SyntaxError) as exc:
            raise BuildError(f"cannot inspect Python App UI dependencies: {path}: {exc}") from exc
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                if any(item.name == "kavio.ui" or item.name.startswith("kavio.ui.") for item in node.names):
                    return True
            elif isinstance(node, ast.ImportFrom):
                if node.module == "kavio" and any(item.name == "ui" for item in node.names):
                    return True
                if node.module and (node.module == "kavio.ui" or node.module.startswith("kavio.ui.")):
                    return True
    return False


def lua_uses_ui(source: Path) -> bool:
    # Handle regular Lua module imports; runtime-computed require paths are not discoverable.
    pattern = re.compile(r"\brequire\s*\(?\s*['\"]kavio\.ui['\"]")
    return any(pattern.search(path.read_text(encoding="utf-8", errors="replace"))
               for path in source.rglob("*.lua"))


def native_ui_dependencies(executable: Path) -> list[str]:
    # Detect direct ELF dependencies; the two UI SDK libraries are packaged together.
    try:
        result = subprocess.run(["readelf", "-d", str(executable)], text=True,
                                capture_output=True, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BuildError(f"cannot inspect native App runtime dependencies ({executable}): {exc}") from exc
    matches = re.findall(r"\(NEEDED\)\s+Shared library: \[([^]]+)\]", result.stdout)
    if "libkavio_ui_lvgl.so" in matches:
        return ["ui_core", "ui_lvgl"]
    if "libkavio_ui_core.so" in matches:
        return ["ui_core"]
    return []


def runtime_dependencies(project: dict[str, Any], artifact: Path) -> list[str]:
    dependencies = infer_dependencies(str(project["language"]))
    language = project["language"]
    if language == "cpp":
        dependencies += native_ui_dependencies(artifact)
    elif language == "python" and python_uses_ui(artifact):
        dependencies += ["ui_core", "ui_lvgl", "python_ui_binding"]
    elif language == "lua" and lua_uses_ui(artifact):
        dependencies += ["ui_core", "ui_lvgl", "lua_ui_binding"]
    return dependencies


def validate_package(package: dict[str, Any], project_path: Path) -> tuple[dict[str, Any], str, str]:
    validate_keys(package, PACKAGE_FIELDS, f"{project_path}: package")
    for field in ("appId", "version", "name"):
        require_string(package.get(field), f"{project_path}: package.{field}")
    require_string(package.get("description", ""), f"{project_path}: package.description", allow_empty=True)

    device = package.get("device")
    if not isinstance(device, dict):
        raise BuildError(f"{project_path}: package.device must be an object")
    validate_keys(device, DEVICE_FIELDS, f"{project_path}: package.device")
    runtime = require_string(device.get("runtime"), f"{project_path}: package.device.runtime")
    entry = require_string(device.get("entry"), f"{project_path}: package.device.entry")
    arguments = device.get("arguments")
    if not isinstance(arguments, list) or not all(isinstance(item, str) for item in arguments):
        raise BuildError(f"{project_path}: package.device.arguments must be an array of strings")

    python_cfg = device.get("python")
    if python_cfg is not None:
        if not isinstance(python_cfg, dict):
            raise BuildError(f"{project_path}: package.device.python must be an object")
        validate_keys(python_cfg, PYTHON_FIELDS, f"{project_path}: package.device.python")
        if "pythonVersion" in python_cfg:
            python_version = require_string(
                python_cfg.get("pythonVersion"), f"{project_path}: package.device.python.pythonVersion")
            if python_version not in {"3.8", "3.8.10"}:
                raise BuildError(
                    f"{project_path}: Python Device SDK native extensions require CPython 3.8; "
                    f"package.device.python.pythonVersion is {python_version!r}")
        if "installMode" in python_cfg:
            install_mode = require_string(python_cfg.get("installMode"), f"{project_path}: package.device.python.installMode")
            if install_mode not in {"archive", "online", "auto"}:
                raise BuildError(f"{project_path}: package.device.python.installMode must be archive, online, or auto")
        environment_inputs = []
        for field in ("environmentArchive", "environmentDefinition", "requirements"):
            value = python_cfg.get(field)
            if value is not None:
                environment_inputs.append(require_string(value, f"{project_path}: package.device.python.{field}"))
        if not environment_inputs:
            raise BuildError(f"{project_path}: Python App requires at least one environment input")

    host = package.get("host")
    if host is not None:
        if not isinstance(host, dict):
            raise BuildError(f"{project_path}: package.host must be an object")
        validate_keys(host, HOST_FIELDS, f"{project_path}: package.host")
        require_string(host.get("binary"), f"{project_path}: package.host.binary")
        if not isinstance(host.get("panel"), bool):
            raise BuildError(f"{project_path}: package.host.panel must be boolean")

    return device, runtime, entry


def load_project(project_path: Path) -> dict[str, Any]:
    project_dir = project_path.parent
    config = read_json(project_path)
    validate_keys(config, ROOT_FIELDS, str(project_path))
    if config.get("projectVersion") != 1:
        raise BuildError(f"{project_path}: unsupported projectVersion")

    language = require_string(config.get("language"), f"{project_path}: language")
    if language not in SUPPORTED_LANGUAGES:
        raise BuildError(f"{project_path}: unsupported language '{language}'")

    build_cfg = config.get("build", {})
    if not isinstance(build_cfg, dict):
        raise BuildError(f"{project_path}: build must be an object")
    validate_keys(build_cfg, BUILD_FIELDS, f"{project_path}: build")
    source_rel = require_string(build_cfg.get("source", "device"), f"{project_path}: build.source")
    source_dir = safe_relative(project_dir, source_rel, f"{project_path}: build.source")
    if not source_dir.is_dir():
        raise BuildError(f"device source directory not found: {source_dir}")

    package = config.get("package")
    if not isinstance(package, dict):
        raise BuildError(f"{project_path}: package must be an object")
    device, runtime, entry = validate_package(package, project_path)

    if not (project_dir / "icon.png").is_file():
        raise BuildError(f"App icon not found: {project_dir / 'icon.png'}")
    if not (project_dir / "schema_description.json").is_file():
        raise BuildError(f"schema_description.json not found: {project_dir}")

    builder = infer_builder(language)
    target: str | None = None
    script_entry: str | None = None

    if language == "cpp":
        if runtime != "native":
            raise BuildError(f"{project_path}: C++ App package.device.runtime must be 'native'")
        target = require_string(build_cfg.get("target", entry), f"{project_path}: build.target")
        if not (source_dir / "CMakeLists.txt").is_file():
            raise BuildError(f"CMakeLists.txt not found: {source_dir}")
        if "python" in device:
            raise BuildError(f"{project_path}: C++ App must not declare package.device.python")
    elif language == "python":
        if runtime != "python":
            raise BuildError(f"{project_path}: Python App package.device.runtime must be 'python'")
        if "target" in build_cfg:
            raise BuildError(f"{project_path}: Python App must not declare build.target")
        if not entry.endswith(".py"):
            raise BuildError(f"{project_path}: Python package.device.entry must be a .py file")
        if not isinstance(device.get("python"), dict):
            raise BuildError(f"{project_path}: Python App requires package.device.python")
        script_entry = entry
    else:
        if runtime != "native":
            raise BuildError(f"{project_path}: Lua App package.device.runtime must be 'native'")
        if "target" in build_cfg:
            raise BuildError(f"{project_path}: Lua App must not declare build.target")
        if Path(entry).name != "kavio_lua_runner":
            raise BuildError(f"{project_path}: Lua package.device.entry must be 'kavio_lua_runner'")
        arguments = device["arguments"]
        if not arguments:
            raise BuildError(f"{project_path}: Lua package.device.arguments[0] must name the Lua entry script")
        script_entry = require_string(arguments[0], f"{project_path}: package.device.arguments[0]")
        if not script_entry.endswith(".lua"):
            raise BuildError(f"{project_path}: Lua entry script must be a .lua file")
        if "python" in device:
            raise BuildError(f"{project_path}: Lua App must not declare package.device.python")

    rel = project_dir.relative_to(APP_ROOT)
    key = rel.as_posix()
    return {
        "key": key,
        "project_dir": project_dir,
        "project_path": project_path,
        "config": config,
        "language": language,
        "builder": builder,
        "source_dir": source_dir,
        "target": target,
        "script_entry": script_entry,
        "dependencies": infer_dependencies(language),
        "package": package,
        "runtime": runtime,
        "entry": entry,
    }

def discover_projects(*, errors: list[str] | None = None) -> list[dict[str, Any]]:
    """Discover App descriptors; optionally isolate invalid projects for listing."""
    projects: list[dict[str, Any]] = []
    if not PROJECT_SCHEMA.is_file():
        raise BuildError(f"project schema not found: {PROJECT_SCHEMA}")
    if not APP_ROOT.is_dir():
        return projects

    for project_path in sorted(APP_ROOT.rglob(PROJECT_CONFIG_NAME)):
        if any(part in {"build", "output", "dist", "__pycache__"} for part in project_path.parts):
            continue
        try:
            projects.append(load_project(project_path))
        except BuildError as exc:
            if errors is None:
                raise
            errors.append(str(exc) if str(project_path) in str(exc) else f"{project_path}: {exc}")

    return projects


def resolve_project(project_key: str) -> dict[str, Any]:
    projects = discover_projects()
    exact = [item for item in projects if item["key"] == project_key]
    if exact:
        return exact[0]
    raise BuildError(f"unknown project: {project_key}; run './kavio list'")


def cmake_configure(source: Path, build: Path, *, release: bool, extra: tuple[str, ...] = ()) -> None:
    command = [
        "cmake", "-S", str(source), "-B", str(build),
        f"-DCMAKE_BUILD_TYPE={build_type(release)}",
    ]
    command.extend(extra)
    run(command)


def build_sdk(args: argparse.Namespace, project: dict[str, Any] | None = None) -> Path:
    sdk_build = BUILD_ROOT / "sdk" / config_key(args.release)
    sdk_build.mkdir(parents=True, exist_ok=True)

    # The SDK is a complete, stable build regardless of which App is selected.
    run([sys.executable, str(DEPENDENCY_CHECKER), "--all"])

    cmake_configure(
        SDK_SOURCE_DIR,
        sdk_build,
        release=args.release,
        extra=(
            "-DCONFIG_LV_BUILD_EXAMPLES=OFF",
            "-DCONFIG_LV_BUILD_DEMOS=OFF",
            "-DLV_BUILD_TESTS=OFF",
        ),
    )
    run([
        "cmake", "--build", str(sdk_build),
        "--target", "kavio_device_sdk_package",
        "-j", str(job_count(args.jobs)),
    ])
    config = sdk_build / "KavioDeviceSdkConfig.cmake"
    if not config.is_file():
        raise BuildError(f"SDK CMake package was not generated: {config}")
    print(f"SDK build: {sdk_build}", flush=True)
    return sdk_build


def project_output_dir(project: dict[str, Any]) -> Path:
    # Preserve the exact project path under kavio_app; language is not a directory rule.
    return BUILD_ROOT.joinpath("apps", *Path(str(project["key"])).parts)


def prepare_project_outputs(project: dict[str, Any]) -> Path:
    output = project_output_dir(project)
    (output / "device").mkdir(parents=True, exist_ok=True)
    (output / "dist").mkdir(parents=True, exist_ok=True)
    if project["package"].get("host") is not None:
        (output / "host").mkdir(parents=True, exist_ok=True)
    return output


def project_build_dir(project: dict[str, Any], args: argparse.Namespace) -> Path:
    return project_output_dir(project) / config_key(args.release) / "device"


def build_native_device(project: dict[str, Any], args: argparse.Namespace, sdk_build: Path) -> Path:
    app_build = project_build_dir(project, args)
    app_build.mkdir(parents=True, exist_ok=True)
    cmake_configure(
        project["source_dir"], app_build, release=args.release,
        extra=(
            f"-DKAVIO_DEVICE_SDK_DIR={sdk_build}",
            f"-DKAVIO_SDK_ROOT={SDK_SOURCE_DIR.parent}",
            f"-DKAVIO_APP_ROOT={APP_ROOT}",
            f"-DKAVIO_APP_PROJECT_ROOT={project['project_dir']}",
            "-DCMAKE_BUILD_WITH_INSTALL_RPATH=ON",
            "-DCMAKE_INSTALL_RPATH=$ORIGIN/lib",
        ),
    )
    run([
        "cmake", "--build", str(app_build),
        "--target", str(project["target"]),
        "-j", str(job_count(args.jobs)),
    ])
    artifact = app_build / "bin" / str(project["target"])
    if not artifact.is_file():
        raise BuildError(f"device artifact was not generated: {artifact}")
    return artifact


def _copy_python_source(source: Path, target: Path) -> None:
    for item in source.rglob("*"):
        if item.is_symlink():
            raise BuildError(f"Python App source must not contain symlinks: {item}")
    if target.exists():
        shutil.rmtree(target)

    def ignore(_: str, names: list[str]) -> set[str]:
        ignored = {name for name in names if name in {"__pycache__", "build", "dist"}}
        ignored.update(name for name in names if name.endswith((".pyc", ".pyo")))
        return ignored

    shutil.copytree(source, target, ignore=ignore)


def build_python_device(project: dict[str, Any], args: argparse.Namespace, sdk_build: Path) -> Path:
    del sdk_build
    app_build = project_build_dir(project, args)
    files_dir = app_build / "files"
    files_dir.parent.mkdir(parents=True, exist_ok=True)
    _copy_python_source(project["source_dir"], files_dir)
    entry = files_dir / str(project["entry"])
    if not entry.is_file():
        raise BuildError(f"Python App entry was not found after staging: {entry}")
    for py_file in sorted(files_dir.rglob("*.py")):
        try:
            compile(py_file.read_text(encoding="utf-8"), str(py_file), "exec")
        except (OSError, UnicodeError, SyntaxError) as exc:
            raise BuildError(f"Python syntax check failed for {py_file}: {exc}") from exc
    return files_dir


def _copy_lua_source(source: Path, target: Path) -> None:
    for item in source.rglob("*"):
        if item.is_symlink():
            raise BuildError(f"Lua App source must not contain symlinks: {item}")
    if target.exists():
        shutil.rmtree(target)

    def ignore(_: str, names: list[str]) -> set[str]:
        return {name for name in names if name in {"build", "dist"}}

    shutil.copytree(source, target, ignore=ignore)


def build_lua_device(project: dict[str, Any], args: argparse.Namespace, sdk_build: Path) -> Path:
    runner = sdk_build / "lua" / "runtime" / "device" / "kavio_lua_runner"
    core_binding = sdk_build / "lua" / "core" / "kavio" / "native.so"
    if not runner.is_file() or not core_binding.is_file():
        raise BuildError(
            "Lua SDK runtime was not built. Ensure Lua 5.4 source is available under "
            "kavio_sdk/device/3rdParty and rebuild the SDK."
        )
    app_build = project_build_dir(project, args)
    files_dir = app_build / "files"
    files_dir.parent.mkdir(parents=True, exist_ok=True)
    _copy_lua_source(project["source_dir"], files_dir)
    script_entry = str(project["script_entry"])
    entry = files_dir / script_entry
    if not entry.is_file():
        raise BuildError(f"Lua App entry script was not found after staging: {entry}")
    lua_files = sorted(files_dir.rglob("*.lua"))
    if not lua_files:
        raise BuildError(f"Lua App contains no .lua source files: {project['source_dir']}")
    for lua_file in lua_files:
        run([str(runner), "--check", str(lua_file)])
    return files_dir


def build_device(project: dict[str, Any], args: argparse.Namespace, sdk_build: Path) -> Path:
    if project["language"] == "cpp":
        return build_native_device(project, args, sdk_build)
    if project["language"] == "python":
        return build_python_device(project, args, sdk_build)
    return build_lua_device(project, args, sdk_build)


def require_host_artifact(project: dict[str, Any]) -> Path | None:
    host = project["package"].get("host")
    if host is None:
        return None
    binary = Path(require_string(host["binary"], "package.host.binary"))
    if binary.is_absolute() or ".." in binary.parts or not binary.parts or binary == Path("."):
        raise BuildError("package.host.binary must be a safe package-relative path")
    artifact = project_output_dir(project) / "host" / binary
    if not artifact.is_file() or artifact.is_symlink():
        raise BuildError(
            "Host Component is declared but its PC build output is missing. "
            "Copy the Windows Host DLL and its dependencies into:\n"
            f"  {project_output_dir(project) / 'host'}\n"
            f"Required DLL: {artifact}"
        )
    return artifact


def copy_user_assets(source: Path, target: Path, name: str) -> None:
    """Copy user-provided runtime assets without overwriting generated outputs."""
    if not source.is_dir() or source.is_symlink():
        raise BuildError(f"{name} directory must be a real directory: {source}")
    for path in sorted(source.rglob("*")):
        if path.is_symlink():
            raise BuildError(f"{name} must not contain symbolic links: {path}")
        relative = path.relative_to(source)
        destination = target / relative
        if path.is_dir():
            if destination.exists() and not destination.is_dir():
                raise BuildError(f"{name} directory conflicts with generated file: {destination}")
            destination.mkdir(parents=True, exist_ok=True)
        elif path.is_file():
            if destination.exists():
                raise BuildError(f"{name} file conflicts with generated output: {destination}")
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, destination)
        else:
            raise BuildError(f"unsupported {name} file: {path}")


def stage_project_metadata(project: dict[str, Any], stage: Path) -> dict[str, Any]:
    for name in ("icon.png", "schema_description.json"):
        source = project["project_dir"] / name
        if source.is_symlink():
            raise BuildError(f"project metadata must not contain symlinks: {source}")
        shutil.copy2(source, stage / name)

    package_config = {"packageConfigVersion": 1, **project["package"]}
    config_path = stage / "package_input.json"
    config_path.write_text(json.dumps(package_config, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return package_config


def sdk_dependency_artifact(name: str, sdk_build: Path) -> tuple[Path, Path]:
    libraries = {
        "app_runtime": "libkavio_app_runtime.so",
        "ui_core": "libkavio_ui_core.so",
        "ui_lvgl": "libkavio_ui_lvgl.so",
    }
    filename = libraries.get(name)
    if filename:
        return sdk_build / "lib" / filename, Path("lib") / filename
    if name == "python_binding":
        return sdk_build / "python" / "core" / "kavio", Path("kavio")
    if name == "python_ui_binding":
        return sdk_build / "python" / "ui" / "kavio", Path("kavio")
    if name == "lua_runtime":
        return sdk_build / "lua" / "runtime" / "device", Path(".")
    if name == "lua_binding":
        return sdk_build / "lua" / "core" / "kavio", Path("kavio")
    if name == "lua_ui_binding":
        return sdk_build / "lua" / "ui" / "kavio", Path("kavio")
    raise BuildError(f"unsupported SDK runtime dependency: {name}")


def copy_device_dependencies(dependencies: list[str], sdk_build: Path, device_stage: Path) -> None:
    for dependency in dependencies:
        source, relative_target = sdk_dependency_artifact(dependency, sdk_build)
        target = device_stage / relative_target
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target, dirs_exist_ok=True)
        elif source.is_file():
            shutil.copy2(source, target)
        else:
            raise BuildError(f"SDK runtime dependency is missing: {source}")


def package_project(
    project: dict[str, Any], args: argparse.Namespace,
    device_artifact: Path, sdk_build: Path,
) -> Path:
    project_output = prepare_project_outputs(project)
    host_artifact = require_host_artifact(project)
    stage = BUILD_ROOT.joinpath("staging", *Path(str(project["key"])).parts)
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True, exist_ok=True)
    package_config = stage_project_metadata(project, stage)

    device = package_config["device"]
    assert isinstance(device, dict)
    device_entry = Path(require_string(device.get("entry"), "package.device.entry"))
    if device_entry.is_absolute() or ".." in device_entry.parts:
        raise BuildError("package.device.entry must be a safe package-relative path")

    device_stage = stage / "device"
    device_stage.mkdir(parents=True, exist_ok=True)
    if device_artifact.is_dir():
        shutil.copytree(device_artifact, device_stage, dirs_exist_ok=True)
    else:
        target = device_stage / device_entry
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(device_artifact, target)
    copy_device_dependencies(runtime_dependencies(project, device_artifact), sdk_build, device_stage)
    copy_user_assets(project_output / "device", device_stage, "Device assets")
    if not (device_stage / device_entry).is_file():
        raise BuildError(f"staged device.entry is missing: {device_stage / device_entry}")

    if host_artifact is not None:
        copy_user_assets(project_output / "host", stage / "host", "Host assets")

    version = require_string(package_config.get("version"), "package.version")
    output_name = f"{str(project['key']).replace('/', '-')}-{version}.kap"
    custom_output = getattr(args, "output", None)
    output = custom_output.resolve() if custom_output is not None else project_output / "dist" / output_name
    run([
        sys.executable, str(PACKAGE_TOOL),
        "--source-dir", str(stage),
        "--output", str(output),
    ])
    return output


def locate_existing_device(project: dict[str, Any], args: argparse.Namespace) -> Path:
    if project["language"] == "cpp":
        artifact = project_build_dir(project, args) / "bin" / str(project["target"])
        exists = artifact.is_file()
    elif project["language"] == "python":
        artifact = project_build_dir(project, args) / "files"
        exists = artifact.is_dir() and (artifact / str(project["entry"])).is_file()
    else:
        artifact = project_build_dir(project, args) / "files"
        script_entry = str(project["script_entry"])
        exists = artifact.is_dir() and (artifact / script_entry).is_file()
    if not exists:
        raise BuildError(
            f"built Device artifact not found: {artifact}; run './kavio build {project['key']}' first"
        )
    return artifact


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise BuildError("development deploy connection closed unexpectedly")
        data.extend(chunk)
    return bytes(data)


class DeveloperControlClient:
    """Client for the board-local development deploy channel."""

    def __init__(self, socket_path: str = DEVELOPER_SOCKET) -> None:
        self._socket_path = socket_path
        self._socket: socket.socket | None = None
        self._request_id = 0

    def __enter__(self) -> "DeveloperControlClient":
        self._socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self._socket.connect(self._socket_path)
        except OSError as exc:
            self._socket.close()
            self._socket = None
            raise BuildError(
                "cannot connect to the board-local development deploy channel; "
                "enable 'Local Development Deploy' on the device and verify the device service is running: "
                f"{exc}"
            ) from exc
        return self

    def __exit__(self, exc_type: object, exc: object, traceback: object) -> None:
        if self._socket is not None:
            self._socket.close()
            self._socket = None

    def request(
        self,
        command: int,
        metadata: dict[str, object] | None = None,
        payload: bytes = b"",
    ) -> dict[str, object]:
        if self._socket is None:
            raise BuildError("developer control client is not connected")
        if len(payload) > 2 * 1024 * 1024:
            raise BuildError("developer control payload exceeds 2 MiB frame limit")

        metadata_bytes = json.dumps(
            metadata or {}, separators=(",", ":"), ensure_ascii=False
        ).encode("utf-8")
        if len(metadata_bytes) > 1024 * 1024:
            raise BuildError("developer control metadata exceeds 1 MiB frame limit")

        self._request_id = (self._request_id + 1) & 0xFFFFFFFF
        if self._request_id == 0:
            self._request_id = 1
        header = DEVELOPER_FRAME_HEADER.pack(
            DEVELOPER_PROTOCOL_MAGIC,
            DEVELOPER_PROTOCOL_VERSION,
            command,
            self._request_id,
            0,
            len(metadata_bytes),
            len(payload),
        )
        self._socket.sendall(header)
        if metadata_bytes:
            self._socket.sendall(metadata_bytes)
        if payload:
            self._socket.sendall(payload)

        response_header = read_exact(self._socket, DEVELOPER_FRAME_HEADER.size)
        magic, version, response_command, request_id, status_code, metadata_size, payload_size = (
            DEVELOPER_FRAME_HEADER.unpack(response_header)
        )
        if magic != DEVELOPER_PROTOCOL_MAGIC or version != DEVELOPER_PROTOCOL_VERSION:
            raise BuildError("device returned an invalid development deploy frame")
        if response_command != (command | DEVELOPER_RESPONSE_BIT) or request_id != self._request_id:
            raise BuildError("device returned a mismatched development deploy response")
        if metadata_size > 1024 * 1024 or payload_size > 2 * 1024 * 1024:
            raise BuildError("development deploy response exceeds protocol limits")

        response_metadata_bytes = read_exact(self._socket, metadata_size) if metadata_size else b""
        if payload_size:
            read_exact(self._socket, payload_size)
        try:
            response = (
                json.loads(response_metadata_bytes.decode("utf-8"))
                if response_metadata_bytes else {}
            )
        except (UnicodeError, json.JSONDecodeError) as exc:
            raise BuildError(f"device returned invalid development deploy response metadata: {exc}") from exc
        if not isinstance(response, dict):
            raise BuildError("development deploy response metadata must be a JSON object")
        if status_code != 0:
            message = response.get("message")
            detail = message if isinstance(message, str) and message else f"status={status_code}"
            raise BuildError(f"device rejected development deploy command: {detail}")
        return response


def inspect_development_package(package_path: Path) -> tuple[str, str, bytes, bytes, int, str]:
    try:
        package_size = package_path.stat().st_size
    except OSError as exc:
        raise BuildError(f"cannot inspect development package: {package_path}: {exc}") from exc
    package_sha256 = sha256_file(package_path)
    try:
        with zipfile.ZipFile(package_path, "r") as archive:
            manifest_bytes = archive.read("manifest.json")
            try:
                signature_bytes = archive.read("signature.json")
            except KeyError:
                signature_bytes = b""
    except (OSError, zipfile.BadZipFile, KeyError) as exc:
        raise BuildError(f"cannot read development package metadata: {package_path}: {exc}") from exc
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise BuildError(f"development package manifest is invalid: {exc}") from exc
    if not isinstance(manifest, dict):
        raise BuildError("development package manifest must be a JSON object")
    app_id = require_string(manifest.get("appId"), "manifest.appId")
    version = require_string(manifest.get("version"), "manifest.version")
    return app_id, version, manifest_bytes, signature_bytes, package_size, package_sha256


def build_development_package(project: dict[str, Any], args: argparse.Namespace) -> Path:
    sdk_build, artifact = build_project(project, args)
    output_dir = BUILD_ROOT.joinpath(
        "deploy", *Path(str(project["key"])).parts, config_key(args.release)
    )
    output_dir.mkdir(parents=True, exist_ok=True)
    output = output_dir / "app.kap"

    # Board-local development deployment updates only the Device Component.
    # Keep the project definition unchanged so formal packaging still includes
    # the Host Component when the project declares one.
    deployment_project = copy.deepcopy(project)
    deployment_package = deployment_project.get("package")
    if not isinstance(deployment_package, dict):
        raise BuildError("project package definition is invalid")
    deployment_package.pop("host", None)

    package_args = argparse.Namespace(**vars(args))
    package_args.output = output
    package_path = package_project(deployment_project, package_args, artifact, sdk_build)
    print(f"Development package: {package_path}", flush=True)
    return package_path


def deploy_development_package(package_path: Path) -> str:
    app_id, version, manifest_bytes, signature_bytes, package_size, package_sha256 = (
        inspect_development_package(package_path)
    )
    session_id = time.time_ns() & ((1 << 63) - 1)
    if session_id == 0:
        session_id = 1

    begin_metadata: dict[str, object] = {
        "installSessionId": str(session_id),
        "appId": app_id,
        "version": version,
        "bundleHash": package_sha256,
        "packageSize": str(package_size),
        "packageSha256": package_sha256,
        "manifestSize": len(manifest_bytes),
        "signatureSize": len(signature_bytes),
    }

    print(f"Deploying {app_id}...", flush=True)
    with DeveloperControlClient() as client:
        begun = False
        committed = False
        try:
            client.request(
                DEV_CMD_DEPLOY_BEGIN,
                begin_metadata,
                manifest_bytes + signature_bytes,
            )
            begun = True
            with package_path.open("rb") as source:
                offset = 0
                while True:
                    chunk = source.read(DEVELOPER_CHUNK_SIZE)
                    if not chunk:
                        break
                    client.request(
                        DEV_CMD_DEPLOY_CHUNK,
                        {
                            "installSessionId": str(session_id),
                            "offset": str(offset),
                        },
                        chunk,
                    )
                    offset += len(chunk)
            client.request(
                DEV_CMD_DEPLOY_COMMIT,
                {"installSessionId": str(session_id)},
            )
            committed = True
        finally:
            if begun and not committed:
                try:
                    client.request(
                        DEV_CMD_DEPLOY_ABORT,
                        {"installSessionId": str(session_id)},
                    )
                except (BuildError, OSError):
                    pass
    print(f"Deployed: {app_id}", flush=True)
    return app_id


def _response_uint64(response: dict[str, object], field: str) -> int:
    value = response.get(field, 0)
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.isdigit():
        return int(value)
    raise BuildError(f"device returned invalid {field}: {value!r}")


def start_development_app(app_id: str) -> tuple[int, Path]:
    print(f"Starting {app_id}...", flush=True)
    with DeveloperControlClient() as client:
        response = client.request(DEV_CMD_START_APP, {"appId": app_id, "force": False})

    process_id = _response_uint64(response, "processId")
    log_path_value = response.get("logPath")
    if process_id <= 0 or not isinstance(log_path_value, str) or not log_path_value:
        raise BuildError("device start response did not include processId and logPath")

    print(f"Started: {app_id} (pid={process_id})", flush=True)
    print(f"Activated: {app_id}", flush=True)
    return process_id, Path(log_path_value)


def stop_development_app(app_id: str) -> None:
    print(f"Stopping {app_id}...", flush=True)
    with DeveloperControlClient() as client:
        client.request(DEV_CMD_STOP_APP, {"appId": app_id, "force": False})


def get_development_log_path(app_id: str) -> Path:
    with DeveloperControlClient() as client:
        response = client.request(DEV_CMD_GET_APP_LOG_PATH, {"appId": app_id, "force": False})
    log_path_value = response.get("logPath")
    if not isinstance(log_path_value, str) or not log_path_value:
        raise BuildError("device did not return an App log path")
    return Path(log_path_value)


def process_is_running(process_id: int) -> bool:
    if process_id <= 0:
        return False
    try:
        for line in Path(f"/proc/{process_id}/status").read_text(encoding="utf-8").splitlines():
            if line.startswith("State:"):
                fields = line.split()
                return len(fields) >= 2 and fields[1] != "Z"
    except OSError:
        return False
    return True


def follow_app_log(log_path: Path, process_id: int | None = None) -> None:
    if not log_path.is_file():
        raise BuildError(f"App log file not found: {log_path}")

    print("Note: only logs written to console (stdout/stderr) are followed.", flush=True)
    print(f"**Application log folder: {log_path.with_suffix('')}", flush=True)
    print(f"--- following {log_path} ---", flush=True)
    with log_path.open("rb") as source:
        while True:
            chunk = source.read(64 * 1024)
            if chunk:
                sys.stdout.buffer.write(chunk)
                sys.stdout.buffer.flush()
                continue

            # AppService truncates the same per-App log file on every launch.
            # Reset to the beginning when a separately invoked `kavio log` spans a restart.
            try:
                if log_path.stat().st_size < source.tell():
                    source.seek(0)
                    continue
            except OSError:
                pass

            if process_id is not None and not process_is_running(process_id):
                # Drain bytes written immediately before process exit.
                time.sleep(0.05)
                chunk = source.read(64 * 1024)
                if chunk:
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
                return
            time.sleep(0.1)


def cmd_deploy(args: argparse.Namespace) -> int:
    project = resolve_project(args.project)
    package_path = build_development_package(project, args)
    deploy_development_package(package_path)
    return 0


def cmd_run(args: argparse.Namespace) -> int:
    project = resolve_project(args.project)
    package_path = build_development_package(project, args)
    app_id = deploy_development_package(package_path)
    process_id, log_path = start_development_app(app_id)
    try:
        follow_app_log(log_path, process_id)
    except KeyboardInterrupt:
        print("\nStopping App after Ctrl+C...", flush=True)
        stop_development_app(app_id)
        print(f"Stopped: {app_id}", flush=True)
        return 0

    print(f"App exited: {app_id}", flush=True)
    return 0


def cmd_log(args: argparse.Namespace) -> int:
    project = resolve_project(args.project)
    package = project["package"]
    app_id = require_string(package.get("appId"), f"{project['project_path']}: package.appId")
    log_path = get_development_log_path(app_id)
    try:
        follow_app_log(log_path)
    except KeyboardInterrupt:
        print("\nLog follow stopped.", flush=True)
    return 0


def cmd_list(_: argparse.Namespace) -> int:
    errors: list[str] = []
    projects = discover_projects(errors=errors)
    if projects:
        print(f"{'PROJECT':30} {'LANGUAGE':9} {'HOST':4} NAME")
        for item in projects:
            package = item["package"]
            print(
                f"{str(item['key']):30} {str(item['language']):9} "
                f"{'yes' if package.get('host') else 'no':4} {package['name']}"
            )
    else:
        print("No valid Kavio projects found." if errors else "No Kavio projects found.")

    if errors:
        print(f"\nSkipped {len(errors)} invalid project(s):", file=sys.stderr)
        for error in errors:
            print(f"  - {error}", file=sys.stderr)
    return 0 if projects or not errors else 1


def build_project(project: dict[str, Any], args: argparse.Namespace) -> tuple[Path, Path]:
    """Build one project's SDK components and Device artifact."""
    prepare_project_outputs(project)
    sdk_build = build_sdk(args, project)
    artifact = build_device(project, args, sdk_build)
    print(f"Device artifact: {artifact}", flush=True)
    return sdk_build, artifact


def cmd_build(args: argparse.Namespace) -> int:
    if args.project == "sdk":
        build_sdk(args)
        return 0
    build_project(resolve_project(args.project), args)
    return 0


def pack_project(project: dict[str, Any], args: argparse.Namespace) -> Path:
    """Always build the Device artifact before packaging it."""
    sdk_build, artifact = build_project(project, args)
    output = package_project(project, args, artifact, sdk_build)
    print(f"Package: {output}", flush=True)
    return output


def cmd_pack(args: argparse.Namespace) -> int:
    pack_project(resolve_project(args.project), args)
    return 0


def run_all_projects(args: argparse.Namespace, *, package: bool) -> int:
    """Continue across invalid projects and individual build/package failures."""
    discovery_errors: list[str] = []
    projects = discover_projects(errors=discovery_errors)
    action = "pack" if package else "build"
    succeeded: list[tuple[str, Path | None]] = []
    failed: list[tuple[str, str]] = [("configuration", error) for error in discovery_errors]

    for index, project in enumerate(projects, 1):
        key = str(project["key"])
        print(f"\n[{index}/{len(projects)}] {action}: {key}", flush=True)
        try:
            if package:
                package_path = pack_project(project, args)
            else:
                build_project(project, args)
                package_path = None
        except (BuildError, subprocess.CalledProcessError, OSError, KeyError) as exc:
            failed.append((key, str(exc)))
            print(f"FAILED: {key}: {exc}", file=sys.stderr, flush=True)
        else:
            succeeded.append((key, package_path))

    print(f"\n{action}_all summary: {len(succeeded)} succeeded, {len(failed)} failed", flush=True)
    for key, package_path in succeeded:
        if package and package_path is not None:
            print(f"  OK: {key}: {package_path}", flush=True)
        else:
            print(f"  OK: {key}", flush=True)
    for key, reason in failed:
        print(f"  FAILED: {key}: {reason}", file=sys.stderr, flush=True)
    if not projects and not discovery_errors:
        print("No Kavio projects found.", file=sys.stderr, flush=True)
        return 1
    return 1 if failed else 0


def cmd_build_all(args: argparse.Namespace) -> int:
    return run_all_projects(args, package=False)


def cmd_pack_all(args: argparse.Namespace) -> int:
    return run_all_projects(args, package=True)


def remove_path(path: Path) -> None:
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists() or path.is_symlink():
        path.unlink()


def cmd_clean(args: argparse.Namespace) -> int:
    target = args.target
    if target == "all":
        remove_path(BUILD_ROOT)
        print(f"Cleaned: {BUILD_ROOT}")
        return 0
    if target == "sdk":
        remove_path(BUILD_ROOT / "sdk")
        print(f"Cleaned: {BUILD_ROOT / 'sdk'}")
        return 0
    project = resolve_project(target)
    key_parts = Path(str(project["key"])).parts
    remove_path(BUILD_ROOT.joinpath("apps", *key_parts))
    remove_path(BUILD_ROOT.joinpath("staging", *key_parts))
    print(f"Cleaned project build: {project['key']}")
    return 0


def project_slug(project_path: str) -> tuple[str, str]:
    name = Path(project_path).name
    app_slug = re.sub(r"[^a-z0-9-]+", "-", name.lower()).strip("-") or "app"
    target_slug = re.sub(r"[^A-Za-z0-9_]+", "_", name).strip("_").lower() or "app"
    return app_slug, target_slug


def copy_template(source: Path, target: Path) -> None:
    if target.exists():
        raise BuildError(f"project directory already exists: {target}")

    def ignore(_: str, names: list[str]) -> set[str]:
        return {
            name for name in names
            if name in {"dist", "output", "build", "__pycache__"}
            or name.endswith((".pyc", ".pyo", ".kap"))
        }

    shutil.copytree(source, target, ignore=ignore)


TEMPLATE_ORDER = [
    "cpp/kavio_widget",
    "cpp/framebuffer",
    "cpp/advanced_ui",
    "python/kavio_widget",
    "python/framebuffer",
    "lua/kavio_widget",
    "lua/framebuffer",
]

TEMPLATE_DESCRIPTIONS = {
    "cpp/kavio_widget": "Standard C++ application with Kavio Widget UI. Recommended for most apps.",
    "cpp/framebuffer": "C++ application that owns its renderer and writes pixels to FrameBuffer.",
    "cpp/advanced_ui": "Specialized C++ UI that uses LVGL APIs directly instead of Kavio Widgets.",
    "python/kavio_widget": "Standard Python application with Kavio Widget UI. Recommended for most apps.",
    "python/framebuffer": "Python application that renders through the writable FrameBuffer buffer protocol.",
    "lua/kavio_widget": "Standard Lua application with Kavio Widget UI. Recommended for most apps.",
    "lua/framebuffer": "Lua application that renders directly through the FrameBuffer BufferView.",
}


def available_templates() -> list[str]:
    if not TEMPLATE_ROOT.is_dir():
        return []
    discovered = {
        path.parent.relative_to(TEMPLATE_ROOT).as_posix()
        for path in TEMPLATE_ROOT.rglob(TEMPLATE_CONFIG_NAME)
    }
    ordered = [key for key in TEMPLATE_ORDER if key in discovered]
    ordered.extend(sorted(discovered - set(ordered)))
    return ordered


def print_templates() -> None:
    templates = available_templates()
    if not templates:
        print("No Kavio App templates found.")
        return

    print("Available templates (choose one with --template):\n")
    current_language = None
    for key in templates:
        language = key.split("/", 1)[0]
        if language != current_language:
            label = {"cpp": "C++", "python": "Python", "lua": "Lua"}.get(language, language)
            print(f"{label}:")
            current_language = language
        print(f"  {key:24} {TEMPLATE_DESCRIPTIONS.get(key, '')}")

    print(
        "\nRecommended: use <language>/kavio_widget for a standard application. "
        "Choose framebuffer only when the application needs to own pixel rendering; "
        "choose cpp/advanced_ui when direct LVGL access is required."
    )


def print_new_help() -> None:
    print(
        "Create a new Kavio Device App\n"
        "============================\n\n"
        "Syntax:\n"
        "  ./kavio new <project-path> --template <template-name> [options]\n\n"
        "The project path is relative to kavio_app/. It may contain subdirectories.\n"
        "The command copies a template, creates kavio_project.json, and initializes\n"
        "the App ID, display name, and package version.\n\n"
        "Options:\n"
        "  --template <name>   Required template name shown below.\n"
        "  --app-id <id>       App ID. Default: com.example.<project-name>.\n"
        "  --name <name>       Display name. Default: final directory name.\n"
        "  --list              Show template choices and this usage guide.\n\n"
        "Examples:\n"
        "  ./kavio new MyFirstApp --template cpp/kavio_widget\n"
        "  ./kavio new examples/PythonMonitor --template python/kavio_widget \\\n"
        "      --app-id com.example.python-monitor --name \"Python Monitor\"\n"
        "  ./kavio new tools/RawDisplay --template lua/framebuffer\n\n"
        "After creating a project:\n"
        "  ./kavio list                         # verify the project was discovered\n"
        "  ./kavio build MyFirstApp             # build its Device artifact\n"
        "  ./kavio deploy MyFirstApp            # build and install for development\n"
        "  ./kavio run MyFirstApp               # build, install, start and follow logs\n"
        "  ./kavio log MyFirstApp               # follow the installed App log\n"
        "  ./kavio pack MyFirstApp              # build and create the .kap package\n"
        "  ./kavio build_all                    # build all discovered projects\n\n"
    )
    print_templates()


def _replace_template_tokens(root: Path, replacements: dict[str, str]) -> None:
    text_suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".py", ".lua", ".md", ".txt", ".json", ".cmake"}
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        if (
            path.suffix.lower() not in text_suffixes
            and path.name not in {"CMakeLists.txt", TEMPLATE_CONFIG_NAME}
        ):
            continue
        try:
            content = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        for token, value in replacements.items():
            content = content.replace(token, value)
        path.write_text(content, encoding="utf-8")


def cmd_new(args: argparse.Namespace) -> int:
    if args.list_templates or args.project is None:
        print_new_help()
        return 0
    if not args.template:
        raise BuildError(
            "--template is required. Example: './kavio new MyApp --template cpp/kavio_widget'. "
            "Run './kavio new' to see template descriptions and more examples."
        )

    relative = Path(args.project)
    if relative.is_absolute() or ".." in relative.parts or not relative.parts:
        raise BuildError("project must be a safe path relative to kavio_app/")
    if relative.parts[0] in {"Templates", "tools"}:
        raise BuildError(f"project path uses a reserved kavio_app directory: {relative.parts[0]}")

    target_dir = APP_ROOT / relative
    template_dir = TEMPLATE_ROOT / Path(args.template)
    template_config = template_dir / TEMPLATE_CONFIG_NAME
    if not template_config.is_file():
        raise BuildError(f"unknown template: {args.template}; run './kavio new --list'")

    app_slug, target_slug = project_slug(args.project)
    app_id = args.app_id or f"com.example.{app_slug}"
    app_name = args.name or Path(args.project).name
    device_entry = f"kavio_{target_slug}"

    copy_template(template_dir, target_dir)
    try:
        _replace_template_tokens(
            target_dir,
            {
                "{{APP_ID}}": app_id,
                "{{APP_NAME}}": app_name,
                "{{DEVICE_ENTRY}}": device_entry,
                "{{PROJECT_KEY}}": args.project,
            },
        )
        template_copy = target_dir / TEMPLATE_CONFIG_NAME
        project_path = target_dir / PROJECT_CONFIG_NAME
        if not template_copy.is_file():
            raise BuildError(f"template project descriptor is missing: {template_copy}")
        template_copy.rename(project_path)

        project = read_json(project_path)
        package = project.get("package")
        if not isinstance(package, dict):
            raise BuildError(f"generated project package is invalid: {project_path}")
        package["appId"] = app_id
        package["name"] = app_name
        package["description"] = f"{app_name} Kavio application."
        package["version"] = "1.0.0"
        package.pop("host", None)
        build_cfg = project.get("build")
        if isinstance(build_cfg, dict):
            if not build_cfg:
                project.pop("build", None)
        project_path.write_text(json.dumps(project, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

        # Reuse the real project scanner as the final validation step.
        resolved = resolve_project(args.project)
        if resolved["project_path"] != project_path:
            raise BuildError(f"generated project was not discovered correctly: {project_path}")
    except Exception:
        remove_path(target_dir)
        raise

    print(f"Created project: {target_dir}")
    print(f"Template: {args.template}")
    print(f"App ID: {app_id}")
    print(f"Next: ./kavio build {args.project}")
    return 0

def add_common_build_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--release", action="store_true", help="use Release configuration")
    parser.add_argument("--jobs", type=int, help="parallel build job count")


def cmd_doctor(args: argparse.Namespace) -> int:
    checks = ["--all"] if args.all else (
        (["--python"] if args.python else [])
        + (["--lua"] if args.lua else [])
        + (["--ui"] if args.ui else [])
    )
    run([sys.executable, str(DEPENDENCY_CHECKER), *checks])
    return 0


def create_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="kavio",
        description=(
            "Kavio Device App development tool. Create projects from templates, "
            "discover projects, build/deploy Apps, and package installable .kap files.\n"
            "A directory is recognized as a project only when it contains kavio_project.json."
        ),
        epilog=(
            "Typical workflow:\n"
            "  1. ./kavio new MyApp --template cpp/kavio_widget\n"
            "  2. ./kavio list\n"
            "  3. ./kavio build MyApp\n"
            "  4. ./kavio deploy MyApp\n"
            "  5. ./kavio pack MyApp\n\n"
            "Common commands:\n"
            "  ./kavio new --help       Show project creation syntax and template choices.\n"
            "  ./kavio doctor --all     Check all SDK build dependencies.\n"
            "  ./kavio build sdk        Build the complete Device SDK.\n"
            "  ./kavio deploy MyApp     Build and install a development App on this device.\n"
            "  ./kavio run MyApp        Build, install, start, activate and follow logs.\n"
            "  ./kavio log MyApp        Follow the current installed App log.\n"
            "  ./kavio build_all        Build every discovered Device App.\n"
            "  ./kavio pack_all         Build and package every discovered Device App."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    sub = parser.add_subparsers(dest="command")

    p_doctor = sub.add_parser(
        "doctor",
        help="check SDK third-party sources and system build dependencies",
        description="Check the host tools and SDK dependencies required by Device App builds.",
        epilog=(
            "Examples:\n"
            "  ./kavio doctor --all       Check every optional component.\n"
            "  ./kavio doctor --python    Check Python native binding requirements.\n"
            "  ./kavio doctor --lua       Check Lua runtime and binding requirements.\n"
            "  ./kavio doctor --ui        Check LVGL and FreeType requirements."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_doctor.add_argument("--all", action="store_true", help="check all optional SDK components")
    p_doctor.add_argument("--python", action="store_true", help="check Python binding requirements")
    p_doctor.add_argument("--lua", action="store_true", help="check Lua binding requirements")
    p_doctor.add_argument("--ui", action="store_true", help="check LVGL/FreeType requirements")
    p_doctor.set_defaults(func=cmd_doctor)

    p_list = sub.add_parser(
        "list",
        help="list projects discovered from kavio_project.json",
        description="List valid projects under kavio_app/ and show their language, UI and Host settings.",
    )
    p_list.set_defaults(func=cmd_list)

    p_build = sub.add_parser(
        "build",
        help="build the Device SDK or one Device App",
        description="Build the SDK or the Device artifact for one discovered App project.",
        epilog=(
            "Examples:\n"
            "  ./kavio build sdk              Build the complete Device SDK.\n"
            "  ./kavio build MyApp            Build the project shown by ./kavio list.\n"
            "  ./kavio build MyApp --release  Build a Release artifact.\n"
            "  ./kavio build MyApp --jobs 4   Limit parallel build jobs."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_build.add_argument("project", help="project key shown by './kavio list', or 'sdk'")
    add_common_build_options(p_build)
    p_build.set_defaults(func=cmd_build)

    p_deploy = sub.add_parser(
        "deploy",
        help="build and install one Device App through the local development deploy channel",
        description=(
            "Build one App and install it through Platform's independent board-local "
            "development deploy channel. The App remains stopped after deployment."
        ),
        epilog=(
            "The device options 'Local Development Deploy' and, for unsigned packages, "
            "'Allow Unverified Apps' must be enabled.\n\n"
            "Examples:\n"
            "  ./kavio deploy MyApp\n"
            "  ./kavio deploy MyApp --release\n"
            "  ./kavio deploy MyApp --jobs 4"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_deploy.add_argument("project", help="project key shown by './kavio list'")
    add_common_build_options(p_deploy)
    p_deploy.set_defaults(func=cmd_deploy)

    p_run = sub.add_parser(
        "run",
        help="build, install, start and foreground one Device App, then follow its log",
        description=(
            "Build and deploy one App, start it through the device App lifecycle, make it the "
            "active screen App, and follow stdout/stderr until the App exits or Ctrl+C is pressed. "
            "Ctrl+C stops the App before returning to the shell."
        ),
        epilog=(
            "Examples:\n"
            "  ./kavio run MyApp\n"
            "  ./kavio run MyApp --release\n"
            "  ./kavio run MyApp --jobs 4"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_run.add_argument("project", help="project key shown by './kavio list'")
    add_common_build_options(p_run)
    p_run.set_defaults(func=cmd_run)

    p_log = sub.add_parser(
        "log",
        help="follow the current log of an installed Device App",
        description=(
            "Resolve the App ID from the project and follow its current stdout/stderr log. "
            "Ctrl+C only stops log following and does not stop the App."
        ),
        epilog=(
            "Examples:\n"
            "  ./kavio log MyApp"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_log.add_argument("project", help="project key shown by './kavio list'")
    p_log.set_defaults(func=cmd_log)

    p_pack = sub.add_parser(
        "pack",
        help="build and package one Device App",
        description="Build one App and create its installable .kap package.",
        epilog=(
            "Examples:\n"
            "  ./kavio pack MyApp\n"
            "  ./kavio pack MyApp --output ./release/MyApp-1.0.0.kap"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_pack.add_argument("project", help="project key shown by './kavio list'")
    add_common_build_options(p_pack)
    p_pack.add_argument("--output", type=Path, help="override output .kap path")
    p_pack.set_defaults(func=cmd_pack)

    p_build_all = sub.add_parser(
        "build_all",
        help="build all discovered Device Apps and continue after individual failures",
    )
    add_common_build_options(p_build_all)
    p_build_all.set_defaults(func=cmd_build_all)

    p_pack_all = sub.add_parser(
        "pack_all",
        help="build and package all discovered Device Apps; continue after individual failures",
    )
    add_common_build_options(p_pack_all)
    p_pack_all.set_defaults(func=cmd_pack_all)

    templates = available_templates()
    template_guide_lines = [
        f"  {template:24} {TEMPLATE_DESCRIPTIONS.get(template, '')}"
        for template in templates
    ]
    template_guide = "\n".join(template_guide_lines) if template_guide_lines else "  No templates found."
    p_new = sub.add_parser(
        "new",
        help="create a project from a C++, Python or Lua template",
        description=(
            "Create a new App directory under kavio_app/. The selected template is copied "
            "and its kavio_project.json is initialized for the new project."
        ),
        epilog=(
            "Examples:\n"
            "  ./kavio new MyApp --template cpp/kavio_widget\n"
            "  ./kavio new examples/PythonMonitor --template python/kavio_widget \\\n"
            "      --app-id com.example.python-monitor --name \"Python Monitor\"\n"
            "  ./kavio new tools/RawDisplay --template lua/framebuffer\n\n"
            "After creation, run './kavio list', then './kavio deploy <project-path>' "
            "for board-local development or './kavio pack <project-path>' for a package.\n\n"
            "Template choices:\n" + template_guide
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_new.add_argument("project", nargs="?", help="project path relative to kavio_app/, e.g. MyApp or examples/MyApp")
    p_new.add_argument("--template", choices=templates, help="template to copy; see template descriptions below")
    p_new.add_argument("--list", dest="list_templates", action="store_true", help="show template choices and usage examples")
    p_new.add_argument("--app-id", help="App ID; default: com.example.<project-name>")
    p_new.add_argument("--name", help="display name; default: final directory name")
    p_new.set_defaults(func=cmd_new)

    p_clean = sub.add_parser(
        "clean",
        help="clean SDK, one project, or all unified build output",
        description="Remove generated build output. Source projects and templates are not removed.",
        epilog=(
            "Examples:\n"
            "  ./kavio clean MyApp\n"
            "  ./kavio clean sdk\n"
            "  ./kavio clean all"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p_clean.add_argument("target", help="sdk, all, or a project key")
    p_clean.set_defaults(func=cmd_clean)
    return parser


def normalize_args(args: argparse.Namespace) -> None:
    output = getattr(args, "output", None)
    if output is not None and output.suffix.lower() != ".kap":
        raise BuildError("--output must use the .kap extension")


def main() -> int:
    parser = create_parser()
    args = parser.parse_args()
    if args.command is None:
        parser.print_help()
        return 0
    try:
        normalize_args(args)
        return int(args.func(args))
    except (BuildError, subprocess.CalledProcessError, OSError, KeyError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
