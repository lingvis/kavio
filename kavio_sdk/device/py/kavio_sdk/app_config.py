"""SDK-owned App configuration: schema validation, atomic persistence and snapshots."""
import json
import math
import os
import threading

MAX_CONFIG_BYTES = 1024 * 1024


def _load_json(path):
    with open(path, "rb") as stream:
        raw = stream.read(MAX_CONFIG_BYTES + 1)
    if len(raw) > MAX_CONFIG_BYTES:
        raise ValueError("configuration exceeds 1 MiB")
    return json.loads(raw.decode("utf-8"))


def _save(path, value):
    raw = (json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode("utf-8")
    if len(raw) > MAX_CONFIG_BYTES:
        raise ValueError("configuration exceeds 1 MiB")
    temporary = path + ".tmp"
    try:
        with open(temporary, "wb") as stream:
            stream.write(raw)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def _valid(value, field):
    kind = field["type"]
    if kind == "boolean":
        return type(value) is bool
    if kind == "integer":
        if type(value) is not int:
            return False
    elif kind == "number":
        if type(value) not in (float, int) or not math.isfinite(value):
            return False
    elif kind == "string":
        if not isinstance(value, str):
            return False
        size = len(value.encode("utf-8"))
        return (not field.get("required", False) or bool(value)) and \
            field.get("min_length", 0) <= size <= field.get("max_length", 65535)
    elif kind == "enum":
        if not isinstance(value, str):
            return False
        if any(option.get("value") == value for option in field["options"]):
            return True
        custom = field.get("custom")
        if custom is None:
            return False
        size = len(value.encode("utf-8"))
        return custom.get("min_length", 0) <= size <= custom.get("max_length", 4096)
    else:
        return False
    return field.get("minimum", float("-inf")) <= value <= field.get("maximum", float("inf"))


class AppConfig:
    """A managed App has one validated schema and one saved configuration."""
    def __init__(self):
        self._lock = threading.RLock()
        self.schema = {}
        self.defaults = {}
        self.current = {}
        self.path = None
        self.loaded = False

    def initialize(self):
        with self._lock:
            source = os.getenv("KAVIO_APP_SCHEMA_DESCRIPTION_PATH")
            data_dir = os.getenv("KAVIO_APP_DATA_DIR")
            if not source or not data_dir:
                self.loaded = True
                return
            if not self.loaded:
                try:
                    if os.path.getsize(source) > 256 * 1024:
                        raise ValueError("schema_description.json exceeds 256 KiB")
                    self.schema = _load_json(source)
                except (OSError, ValueError) as exc:
                    raise ValueError("invalid schema_description.json; use {} for no configuration") from exc
                if not isinstance(self.schema, dict):
                    raise ValueError("schema_description.json must be a JSON object; use {} for no configuration")
                self.defaults = {}
                if self.schema:
                    fields = self.schema.get("fields")
                    if self.schema.get("definition_version") != "2.0" or not isinstance(self.schema.get("title"), str) or not self.schema["title"] or not isinstance(fields, list) or not 1 <= len(fields) <= 128:
                        raise ValueError("invalid schema_description.json; use {} for no configuration")
                    for field in fields:
                        if not isinstance(field, dict) or not isinstance(field.get("key"), str) or \
                                any(not isinstance(field.get(k), str) for k in ("name", "category", "description")) or \
                                "value" not in field or field["key"] in self.defaults or \
                                not _valid(field["value"], field):
                            raise ValueError("invalid schema_description.json; use {} for no configuration")
                        self.defaults[field["key"]] = field["value"]
                self.path = os.path.join(data_dir, "config.json")
                self.loaded = True
            if not self.schema:
                return
            if not os.path.exists(self.path):
                _save(self.path, self.defaults)
                self.current = dict(self.defaults)
                return
            current = _load_json(self.path)
            self._validate(current)
            self.current = current

    def _validate(self, value):
        if not isinstance(value, dict) or set(value) != set(self.defaults):
            raise ValueError("config.json fields do not match schema_description.json")
        for field in self.schema["fields"]:
            if not _valid(value[field["key"]], field):
                raise ValueError("invalid configuration field: " + field["key"])

    def update(self, value):
        with self._lock:
            if not self.schema:
                raise ValueError("App has no configuration")
            self._validate(value)
            _save(self.path + ".backup", self.current)
            _save(self.path, value)
            self.current = dict(value)

    def reset_to_defaults(self):
        with self._lock:
            if not self.schema:
                raise ValueError("App has no configuration")
            _save(self.path, self.defaults)
            self.current = dict(self.defaults)

    def get(self):
        with self._lock:
            return dict(self.current)

    def description_bytes(self):
        with self._lock:
            return json.dumps(self.schema, ensure_ascii=False).encode("utf-8")

    def config_bytes(self):
        with self._lock:
            return json.dumps(self.current, ensure_ascii=False).encode("utf-8")
