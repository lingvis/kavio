#!/usr/bin/env python3
"""Build an unsigned Kavio application package from a prepared directory."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
from functools import lru_cache
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import zipfile

TOOLS_DIR = Path(__file__).resolve().parent
SDK_ROOT = TOOLS_DIR.parents[1] / "kavio_sdk" / "device"
APP_MANIFEST_BINDING = SDK_ROOT / "py" / "kavio_sdk" / "proto_py" / "app_manifest_pb2.py"

try:
    from google.protobuf.json_format import MessageToJson
except ImportError as exc:
    raise RuntimeError(
        "Kavio packaging requires the Python protobuf runtime; install protobuf>=3.20.3"
    ) from exc


def load_app_manifest_binding():
    if not APP_MANIFEST_BINDING.is_file():
        raise RuntimeError(
            f"Kavio packaging protobuf binding is missing: {APP_MANIFEST_BINDING}; "
            "build kavio_sdk/device first"
        )
    spec = importlib.util.spec_from_file_location(
        "kavio_app_manifest_pb2", APP_MANIFEST_BINDING
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(
            f"cannot load Kavio packaging protobuf binding: {APP_MANIFEST_BINDING}"
        )
    module = importlib.util.module_from_spec(spec)
    try:
        spec.loader.exec_module(module)
    except ModuleNotFoundError as exc:
        if exc.name and exc.name.startswith("google.protobuf"):
            raise RuntimeError(
                "Kavio packaging protobuf runtime is missing or incompatible; "
                "install protobuf>=3.20.3"
            ) from exc
        raise RuntimeError(
            f"failed to load Kavio packaging protobuf binding "
            f"{APP_MANIFEST_BINDING}: {exc}"
        ) from exc
    except Exception as exc:
        raise RuntimeError(
            f"failed to load Kavio packaging protobuf binding "
            f"{APP_MANIFEST_BINDING}: {exc}"
        ) from exc
    return module


app_manifest_pb2 = None # loaded only for packaging, not for status/help

PACKAGE_CONFIG_NAME = "kavio_package.json"
PACKAGE_CONFIG_VERSION = 1
GENERATED_FILE_NAMES = {"manifest.json", "signature.json"}
APP_ICON_WIDTH = 64
APP_ICON_HEIGHT = 64
PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
CONFIG_MAX_FIELDS = 128
# Required in the package source directory and copied unchanged into the .kap.
SCHEMA_DESCRIPTION_PATH = "schema_description.json"
CONFIG_MAX_JSON_BYTES = 1024 * 1024
FIELD_KEY_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")
ROOT = TOOLS_DIR.parents[1]
KAVIO_VERSION_HEADER = ROOT / "kavio_sdk" / "device" / "include" / "kavio" / "KavioSdkVersion.h"


class PackageError(ValueError):
    """Raised when the package source directory is invalid."""


@lru_cache(maxsize=None)
def read_version_define(path: Path, name: str) -> str:
    try:
        content = path.read_text(encoding="utf-8", errors="ignore")
    except OSError as exc:
        raise PackageError(f"cannot read version header: {path}: {exc}") from exc
    pattern = re.compile(rf"^\s*#define\s+{re.escape(name)}\s+([^/\r\n]+)", re.MULTILINE)
    match = pattern.search(content)
    if not match:
        raise PackageError(f"{name} is not defined in {path}")
    return match.group(1).strip()


def current_manifest_version() -> int:
    raw = read_version_define(KAVIO_VERSION_HEADER, "KAVIO_MANIFEST_VERSION")
    match = re.fullmatch(r"(\d+)[uUlL]*", raw)
    if not match:
        raise PackageError(f"KAVIO_MANIFEST_VERSION must be an integer macro, got: {raw}")
    return int(match.group(1))


def current_config_definition_version() -> str:
    raw = read_version_define(KAVIO_VERSION_HEADER, "KAVIO_CONFIG_DEFINITION_VERSION")
    match = re.fullmatch(r'"([^"]*)"', raw)
    if not match:
        raise PackageError(f"KAVIO_CONFIG_DEFINITION_VERSION must be a string macro, got: {raw}")
    return match.group(1)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def append_file_entry(manifest: app_manifest_pb2.AppManifestPb,
                      path: Path, package_path: str) -> None:
    entry = manifest.files.add()
    entry.path = package_path
    entry.size = path.stat().st_size
    entry.sha256 = sha256(path)


def require_object(value: object, field: str) -> dict[str, object]:
    if not isinstance(value, dict):
        raise PackageError(f"{field} must be a JSON object")
    return value


def require_string(value: object, field: str, *, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or (not allow_empty and not value):
        suffix = "a string" if allow_empty else "a non-empty string"
        raise PackageError(f"{field} must be {suffix}")
    return value


def optional_string(value: object, field: str) -> str:
    if value is None:
        return ""
    return require_string(value, field, allow_empty=True)


def require_bool(value: object, field: str) -> bool:
    if not isinstance(value, bool):
        raise PackageError(f"{field} must be a boolean")
    return value


def require_uint(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise PackageError(f"{field} must be a non-negative integer")
    return value


def require_string_list(value: object, field: str) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(item, str) for item in value):
        raise PackageError(f"{field} must be an array of strings")
    return list(value)


def validate_app_id(value: str) -> None:
    if len(value) > 128:
        raise PackageError("appId must not exceed 128 characters")
    previous_dot = False
    for index, character in enumerate(value):
        valid = character.isascii() and (
            character.islower() or character.isdigit() or character in (".", "-")
        )
        if not valid:
            raise PackageError(
                "appId may contain only lowercase ASCII letters, digits, '.' and '-'"
            )
        if index in (0, len(value) - 1) and not character.isalnum():
            raise PackageError("appId must start and end with a letter or digit")
        if character == "." and previous_dot:
            raise PackageError("appId must not contain consecutive dots")
        previous_dot = character == "."


def normalize_relative_path(value: object, field: str, *, allow_empty: bool = False) -> str:
    raw = require_string(value, field, allow_empty=allow_empty)
    if not raw:
        return ""
    if "\\" in raw or ":" in raw:
        raise PackageError(f"{field} must use a safe package-relative path with '/' separators")
    path = PurePosixPath(raw)
    if path.is_absolute() or any(part in ("", ".", "..") for part in path.parts):
        raise PackageError(f"{field} must be a safe package-relative path")
    return path.as_posix()


def join_package_path(directory: str, relative_path: str, field: str) -> str:
    if not directory:
        return normalize_relative_path(relative_path, field)
    return normalize_relative_path(f"{directory}/{relative_path}", field)


def source_path(source_dir: Path, package_path: str, field: str) -> Path:
    path = source_dir.joinpath(*PurePosixPath(package_path).parts)
    try:
        resolved = path.resolve(strict=True)
    except FileNotFoundError as exc:
        raise PackageError(f"{field} does not exist: {package_path}") from exc
    try:
        resolved.relative_to(source_dir)
    except ValueError as exc:
        raise PackageError(f"{field} escapes the source directory: {package_path}") from exc
    return resolved


def validate_regular_file(path: Path, field: str) -> None:
    if path.is_symlink():
        raise PackageError(f"{field} must not be a symbolic link: {path}")
    try:
        mode = path.stat().st_mode
    except OSError as exc:
        raise PackageError(f"cannot inspect {field}: {path}: {exc}") from exc
    if not stat.S_ISREG(mode):
        raise PackageError(f"{field} must be a regular file: {path}")


def validate_app_icon(path: Path, field: str = "icon") -> None:
    """Validate the required 64x64 PNG App icon without an image-library dependency."""
    validate_regular_file(path, field)
    if path.suffix.lower() != ".png":
        raise PackageError(f"{field} must be a PNG file")
    try:
        with path.open("rb") as source:
            header = source.read(24)
    except OSError as exc:
        raise PackageError(f"cannot read {field}: {path}: {exc}") from exc
    if (len(header) < 24 or header[:8] != PNG_SIGNATURE or
            header[12:16] != b"IHDR"):
        raise PackageError(f"{field} is not a valid PNG file")
    width = int.from_bytes(header[16:20], "big")
    height = int.from_bytes(header[20:24], "big")
    if width != APP_ICON_WIDTH or height != APP_ICON_HEIGHT:
        raise PackageError(
            f"{field} must be exactly {APP_ICON_WIDTH}x{APP_ICON_HEIGHT} pixels; "
            f"got {width}x{height}"
        )


def collect_package_files(source_dir: Path, config_path: Path,
                          output_path: Path) -> list[tuple[Path, str]]:
    files: list[tuple[Path, str]] = []
    for root, directory_names, file_names in os.walk(source_dir, followlinks=False):
        root_path = Path(root)
        for directory_name in directory_names:
            directory = root_path / directory_name
            if directory.is_symlink():
                raise PackageError(f"symbolic-link directories are not allowed: {directory}")
        for file_name in file_names:
            path = root_path / file_name
            resolved = path.resolve(strict=True)
            if path.is_symlink():
                raise PackageError(f"symbolic-link files are not allowed: {path}")
            if resolved == config_path or resolved == output_path:
                continue
            relative = path.relative_to(source_dir).as_posix()
            if relative in GENERATED_FILE_NAMES:
                raise PackageError(
                    f"{relative} is generated by the packaging tool and must not exist "
                    "in the source directory"
                )
            validate_regular_file(path, "package file")
            files.append((path, normalize_relative_path(relative, "package file path")))
    files.sort(key=lambda item: item[1])
    if not files:
        raise PackageError("the source directory contains no distributable files")
    return files


def read_json_file(path: Path, field: str) -> object:
    validate_regular_file(path, field)
    try:
        return json.loads(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise PackageError(f"{field} is not valid UTF-8 JSON: {exc}") from exc


def validate_field_value(value: object, definition: dict[str, object], field: str) -> None:
    value_type = definition["type"]
    if value_type == "boolean":
        if not isinstance(value, bool):
            raise PackageError(f"{field} must be boolean")
        return
    if value_type == "integer":
        if not isinstance(value, int) or isinstance(value, bool):
            raise PackageError(f"{field} must be integer")
    elif value_type == "number":
        if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(value):
            raise PackageError(f"{field} must be a finite number")
    elif value_type == "string":
        if not isinstance(value, str):
            raise PackageError(f"{field} must be string")
        minimum = definition.get("min_length", 0)
        maximum = definition.get("max_length", 65535)
        byte_length = len(value.encode("utf-8"))
        if byte_length < minimum or byte_length > maximum:
            raise PackageError(f"{field} UTF-8 string length is outside the declared range")
        if definition.get("required", False) and not value:
            raise PackageError(f"{field} must not be empty")
        return
    elif value_type == "enum":
        if not isinstance(value, str):
            raise PackageError(f"{field} must be an enum string")
        allowed = {item["value"] for item in definition["options"]}
        if value not in allowed:
            custom = definition.get("custom")
            if custom is None:
                raise PackageError(f"{field} is not one of the declared enum values")
            byte_length = len(value.encode("utf-8"))
            if ((definition.get("required", False) and not value) or
                    byte_length < custom.get("min_length", 0) or
                    byte_length > custom.get("max_length", 4096)):
                raise PackageError(f"{field} custom string length is outside the declared range")
        return
    else:
        raise PackageError(f"{field} has unsupported type")

    minimum = definition.get("minimum")
    maximum = definition.get("maximum")
    if minimum is not None and value < minimum:
        raise PackageError(f"{field} is below minimum")
    if maximum is not None and value > maximum:
        raise PackageError(f"{field} is above maximum")


def validate_schema_description(source_dir: Path) -> bool:
    definition_path = SCHEMA_DESCRIPTION_PATH
    definition_source = source_path(source_dir, definition_path, "schema_description.json")
    if not definition_source.is_file() or definition_source.is_symlink():
        raise PackageError("schema_description.json must exist; use {} for no configuration")
    if definition_source.stat().st_size > 256 * 1024:
        raise PackageError("schema_description.json exceeds 256 KiB; use {} for no configuration")
    try:
        definition_json = read_json_file(
            source_path(source_dir, definition_path, "schema_description.json"),
            "schema_description.json")
        config_definition = require_object(definition_json, "schema_description.json")
    except PackageError as exc:
        raise PackageError(f"{exc}; use {{}} in schema_description.json for no configuration") from exc
    if not config_definition:
        return False
    try:
        validate_schema_description_content(config_definition)
    except PackageError as exc:
        raise PackageError(f"{exc}; use {{}} in schema_description.json for no configuration") from exc
    return True


def validate_schema_description_content(config_definition: dict[str, object]) -> None:
    root_allowed = {"definition_version", "title", "description", "fields"}
    root_unknown = set(config_definition) - root_allowed
    if root_unknown:
        raise PackageError(
            f"schema_description.json root contains unsupported fields: {', '.join(sorted(root_unknown))}"
        )
    definition_version = current_config_definition_version()
    if config_definition.get("definition_version") != definition_version:
        raise PackageError(
            f"schema_description.json.definition_version must be '{definition_version}'"
        )
    if not isinstance(config_definition.get("title"), str) or not config_definition["title"]:
        raise PackageError("schema_description.json.title must be a non-empty string")
    if "description" in config_definition and not isinstance(config_definition["description"], str):
        raise PackageError("schema_description.json.description must be a string")
    fields = config_definition.get("fields")
    if not isinstance(fields, list) or not fields or len(fields) > CONFIG_MAX_FIELDS:
        raise PackageError(f"schema_description.json.fields must contain 1 to {CONFIG_MAX_FIELDS} items")

    field_allowed = {
        "key", "name", "category", "description", "type", "required",
        "read_only", "minimum", "maximum", "min_length", "max_length", "multiline",
        "unit", "placeholder", "options", "show_value", "custom",
    }
    field_allowed.add("value")
    definitions: dict[str, dict[str, object]] = {}
    for index, raw_field in enumerate(fields):
        field_name = f"schema_description.json.fields[{index}]"
        definition = require_object(raw_field, field_name)
        unknown = set(definition) - field_allowed
        if unknown:
            raise PackageError(
                f"{field_name} contains unsupported fields: {', '.join(sorted(unknown))}"
            )
        key = require_string(definition.get("key"), f"{field_name}.key")
        if not FIELD_KEY_RE.fullmatch(key):
            raise PackageError(f"{field_name}.key has an invalid format: {key}")
        if key in definitions:
            raise PackageError(f"config field key is duplicated: {key}")
        require_string(definition.get("name"), f"{field_name}.name")
        require_string(definition.get("category"), f"{field_name}.category")
        require_string(definition.get("description"), f"{field_name}.description")
        value_type = require_string(definition.get("type"), f"{field_name}.type")
        if value_type not in {"boolean", "integer", "number", "string", "enum"}:
            raise PackageError(f"{field_name}.type is unsupported: {value_type}")
        definition["type"] = value_type
        for bool_key in ("required", "read_only", "multiline", "show_value"):
            if bool_key in definition:
                require_bool(definition[bool_key], f"{field_name}.{bool_key}")
        if definition.get("multiline", False) and value_type != "string":
            raise PackageError(f"{field_name}.multiline is valid only for string fields")

        for number_key in ("minimum", "maximum"):
            if number_key in definition:
                number = definition[number_key]
                if not isinstance(number, (int, float)) or isinstance(number, bool) or not math.isfinite(number):
                    raise PackageError(f"{field_name}.{number_key} must be a finite number")
                if value_type not in {"integer", "number"}:
                    raise PackageError(f"{field_name}.{number_key} is valid only for numeric fields")
                if value_type == "integer" and not float(number).is_integer():
                    raise PackageError(f"{field_name}.{number_key} must be integral")
        if ("minimum" in definition and "maximum" in definition and
                definition["minimum"] > definition["maximum"]):
            raise PackageError(f"{field_name}.minimum must not exceed maximum")

        for length_key, fallback in (("min_length", 0), ("max_length", 65535)):
            if length_key in definition:
                length = definition[length_key]
                if not isinstance(length, int) or isinstance(length, bool) or not 0 <= length <= 65535:
                    raise PackageError(f"{field_name}.{length_key} must be an integer in [0, 65535]")
                if value_type != "string":
                    raise PackageError(f"{field_name}.{length_key} is valid only for string fields")
            else:
                definition[length_key] = fallback
        if definition["min_length"] > definition["max_length"]:
            raise PackageError(f"{field_name}.min_length must not exceed max_length")

        if value_type != "enum" and ("show_value" in definition or "custom" in definition):
            raise PackageError(f"{field_name}.show_value and custom are valid only for enum fields")

        if value_type == "enum":
            if "custom" in definition:
                custom_name = f"{field_name}.custom"
                custom = require_object(definition["custom"], custom_name)
                unknown_custom = set(custom) - {"label", "placeholder", "min_length", "max_length"}
                if unknown_custom:
                    raise PackageError(f"{custom_name} contains unsupported fields: " +
                                       ", ".join(sorted(unknown_custom)))
                label = custom.get("label", "自定义")
                placeholder = custom.get("placeholder", "请输入自定义值")
                require_string(label, f"{custom_name}.label")
                require_string(placeholder, f"{custom_name}.placeholder", allow_empty=True)
                if len(label.encode("utf-8")) > 128 or len(placeholder.encode("utf-8")) > 256:
                    raise PackageError(f"{custom_name} text is too long")
                for name, fallback in (("min_length", 0), ("max_length", 4096)):
                    length = custom.get(name, fallback)
                    if (not isinstance(length, int) or isinstance(length, bool) or
                            not 0 <= length <= 65535):
                        raise PackageError(f"{custom_name}.{name} must be an integer in [0, 65535]")
                    custom[name] = length
                if custom["min_length"] > custom["max_length"]:
                    raise PackageError(f"{custom_name}.min_length must not exceed max_length")
                custom["label"] = label
                custom["placeholder"] = placeholder
            options = definition.get("options")
            if not isinstance(options, list) or not options or len(options) > 256:
                raise PackageError(f"{field_name}.options must contain 1 to 256 items")
            seen: set[str] = set()
            normalized_options: list[dict[str, str]] = []
            for option_index, raw_option in enumerate(options):
                option_name = f"{field_name}.options[{option_index}]"
                option = require_object(raw_option, option_name)
                if set(option) != {"value", "label"}:
                    raise PackageError(f"{option_name} must contain only value and label")
                value = require_string(option.get("value"), f"{option_name}.value")
                label = require_string(option.get("label"), f"{option_name}.label")
                if len(value.encode("utf-8")) > 128 or len(label.encode("utf-8")) > 128:
                    raise PackageError(f"{option_name} value or label is too long")
                if "custom" in definition and label == definition["custom"]["label"]:
                    raise PackageError(f"{option_name}.label conflicts with custom.label")
                if value in seen:
                    raise PackageError(f"{option_name}.value is duplicated: {value}")
                seen.add(value)
                normalized_options.append({"value": value, "label": label})
            definition["options"] = normalized_options
        elif "options" in definition:
            raise PackageError(f"{field_name}.options is valid only for enum fields")

        if "value" not in definition:
            raise PackageError(f"{field_name}.value is required")
        validate_field_value(definition["value"], definition, f"{field_name}.value")
        definitions[key] = definition




def parse_manifest(config: dict[str, object], source_dir: Path,
                   package_paths: set[str]) -> app_manifest_pb2.AppManifestPb:
    config_allowed = {
        "packageConfigVersion", "appId", "version", "name", "description",
        "icon", "device", "host",
    }
    config_unknown = set(config) - config_allowed
    if config_unknown:
        raise PackageError(
            f"package config contains unsupported fields: {', '.join(sorted(config_unknown))}"
        )

    config_version = require_uint(config.get("packageConfigVersion"), "packageConfigVersion")
    if config_version != PACKAGE_CONFIG_VERSION:
        raise PackageError(
            f"unsupported packageConfigVersion={config_version}; expected {PACKAGE_CONFIG_VERSION}"
        )

    manifest = app_manifest_pb2.AppManifestPb()
    manifest.manifest_version = current_manifest_version()
    manifest.app_id = require_string(config.get("appId"), "appId")
    validate_app_id(manifest.app_id)
    manifest.version = require_string(config.get("version"), "version")
    manifest.name = require_string(config.get("name"), "name")
    manifest.description = optional_string(config.get("description"), "description")
    # Every Kavio App must provide a 64x64 PNG icon.  The package builder
    # rejects the package before manifest generation when the icon is missing or invalid.
    manifest.icon = normalize_relative_path(config.get("icon"), "icon")
    validate_app_icon(source_path(source_dir, manifest.icon, "icon"))

    device = require_object(config.get("device"), "device")
    device_allowed = {"directory", "runtime", "entry", "arguments", "autoStart", "python"}
    device_unknown = set(device) - device_allowed
    if device_unknown:
        raise PackageError(
            f"device contains unsupported fields: {', '.join(sorted(device_unknown))}"
        )
    device_directory = normalize_relative_path(device.get("directory"), "device.directory")
    device_source = source_path(source_dir, device_directory, "device.directory")
    if not device_source.is_dir() or device_source.is_symlink():
        raise PackageError("device.directory must identify a real directory")

    device_manifest = manifest.device
    device_manifest.runtime = require_string(device.get("runtime", "native"), "device.runtime")
    if device_manifest.runtime not in ("native", "python"):
        raise PackageError("device.runtime must be 'native' or 'python'")
    device_entry_relative = normalize_relative_path(device.get("entry"), "device.entry")
    device_manifest.entry = join_package_path(device_directory, device_entry_relative, "device.entry")
    device_manifest.arguments.extend(require_string_list(device.get("arguments", []), "device.arguments"))
    device_manifest.auto_start = require_bool(device.get("autoStart", False), "device.autoStart")

    if device_manifest.runtime == "python":
        python_config = require_object(device.get("python"), "device.python")
        python_allowed = {
            "pythonVersion", "installMode", "environmentArchive",
            "environmentDefinition", "requirements",
        }
        python_unknown = set(python_config) - python_allowed
        if python_unknown:
            raise PackageError(
                "device.python contains unsupported fields: "
                + ", ".join(sorted(python_unknown))
            )
        python_manifest = device_manifest.python
        python_manifest.python_version = optional_string(
            python_config.get("pythonVersion"), "device.python.pythonVersion")
        python_manifest.install_mode = require_string(
            python_config.get("installMode", "auto"), "device.python.installMode")
        for config_key, proto_field in (
            ("environmentArchive", "environment_archive"),
            ("environmentDefinition", "environment_definition"),
            ("requirements", "requirements"),
        ):
            value = normalize_relative_path(
                python_config.get(config_key, ""),
                f"device.python.{config_key}",
                allow_empty=True,
            )
            package_value = (
                join_package_path(device_directory, value, f"device.python.{config_key}")
                if value else ""
            )
            setattr(python_manifest, proto_field, package_value)
        if not any((
            python_manifest.environment_archive,
            python_manifest.environment_definition,
            python_manifest.requirements,
        )):
            raise PackageError("Python applications require at least one environment input")
    elif "python" in device:
        raise PackageError("device.python is valid only when device.runtime is 'python'")

    host_value = config.get("host")
    if host_value is not None:
        host = require_object(host_value, "host")
        host_allowed = {"directory", "binary", "panel"}
        host_unknown = set(host) - host_allowed
        if host_unknown:
            raise PackageError(
                f"host contains unsupported fields: {', '.join(sorted(host_unknown))}"
            )
        host_directory = normalize_relative_path(host.get("directory"), "host.directory")
        host_source = source_path(source_dir, host_directory, "host.directory")
        if not host_source.is_dir() or host_source.is_symlink():
            raise PackageError("host.directory must identify a real directory")
        host_binary_relative = normalize_relative_path(host.get("binary"), "host.binary")
        manifest.host.binary = join_package_path(
            host_directory, host_binary_relative, "host.binary")
        manifest.host.panel = require_bool(host.get("panel", False), "host.panel")

    # Schema description is mandatory, including applications without configurable values.
    validate_schema_description(source_dir)

    referenced_paths: list[tuple[str, str]] = [(device_manifest.entry, "device.entry")]
    if manifest.icon:
        referenced_paths.append((manifest.icon, "icon"))
    if manifest.HasField("host"):
        referenced_paths.append((manifest.host.binary, "host.binary"))
    referenced_paths.append((SCHEMA_DESCRIPTION_PATH, "schema_description.json"))
    if device_manifest.runtime == "python":
        for value, field in (
            (device_manifest.python.environment_archive, "device.python.environmentArchive"),
            (device_manifest.python.environment_definition, "device.python.environmentDefinition"),
            (device_manifest.python.requirements, "device.python.requirements"),
        ):
            if value:
                referenced_paths.append((value, field))

    for package_path, field in referenced_paths:
        if package_path not in package_paths:
            raise PackageError(f"{field} is not a packaged file: {package_path}")

    return manifest


def serialize_manifest_json(manifest: app_manifest_pb2.AppManifestPb) -> bytes:
    # manifest.json is the canonical protobuf JSON representation of AppManifestPb.
    # Field names, scalar encoding and message layout therefore come only from
    # app_manifest.proto, not from a second hand-maintained JSON structure definition.
    return MessageToJson(
        manifest,
        preserving_proto_field_name=False,
        indent=2,
        sort_keys=False,
    ).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create an unsigned Kavio application package from a directory containing "
            f"{PACKAGE_CONFIG_NAME}."
        )
    )
    parser.add_argument("--source-dir", required=True, type=Path,
                        help=f"prepared package directory containing {PACKAGE_CONFIG_NAME}")
    parser.add_argument("--output", required=True, type=Path,
                        help="output .kap (Kavio App Package) file")
    args = parser.parse_args()

    try:
        source_dir = args.source_dir.resolve(strict=True)
        if not source_dir.is_dir() or source_dir.is_symlink():
            raise PackageError(f"source directory is invalid: {source_dir}")
        config_path = (source_dir / PACKAGE_CONFIG_NAME).resolve(strict=True)
        try:
            config_path.relative_to(source_dir)
        except ValueError as exc:
            raise PackageError(f"{PACKAGE_CONFIG_NAME} escapes the source directory") from exc
        validate_regular_file(config_path, PACKAGE_CONFIG_NAME)

        output_path = args.output.resolve(strict=False)
        if output_path.suffix.lower() != ".kap":
            raise PackageError("Kavio App Package output must use the .kap extension")
        temporary_output = output_path.with_name(output_path.name + ".tmp")

        # Always remove the target archive before building. A failed package
        # operation must not leave a pre-existing .kap that can be mistaken for this build.
        if temporary_output.exists():
            temporary_output.unlink()
        if output_path.exists():
            if not output_path.is_file() or output_path.is_symlink():
                raise PackageError(f"output path is not a regular file: {output_path}")
            output_path.unlink()
        try:
            config_data = json.loads(config_path.read_text(encoding="utf-8-sig"))
        except (OSError, UnicodeError, json.JSONDecodeError) as exc:
            raise PackageError(f"read {PACKAGE_CONFIG_NAME} failed: {exc}") from exc
        config = require_object(config_data, PACKAGE_CONFIG_NAME)

        global app_manifest_pb2
        app_manifest_pb2 = load_app_manifest_binding()

        package_files = collect_package_files(source_dir, config_path, output_path)
        package_paths = {package_path for _, package_path in package_files}
        manifest = parse_manifest(config, source_dir, package_paths)
        for path, package_path in package_files:
            append_file_entry(manifest, path, package_path)

        output_path.parent.mkdir(parents=True, exist_ok=True)
        try:
            with zipfile.ZipFile(temporary_output, "w", compression=zipfile.ZIP_DEFLATED,
                                 allowZip64=True) as package:
                package.writestr(
                    "manifest.json",
                    serialize_manifest_json(manifest),
                )
                for source_path_value, package_path in package_files:
                    package.write(source_path_value, package_path)
            temporary_output.replace(output_path)
        except Exception:
            if temporary_output.exists():
                temporary_output.unlink()
            raise

        print(f"Created unsigned development package: {output_path}")
        print(f"app_id: {manifest.app_id}")
        print(f"files: {len(package_files)}")
        if manifest.HasField("host"):
            print("Enable both Host development mode and Device development mode before installation.")
        else:
            print("Enable Device development mode before installing this unsigned device-only package.")
        return 0
    except (PackageError, OSError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
