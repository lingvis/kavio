# Kavio App Configuration — Schema Description v3 (current)

## 1. Package contract

Every App package directory MUST contain `schema_description.json`, at the same level as
`kavio_package.json`. The file is included unmodified in the `.kap` archive and in the
`AppManifestPb.files` integrity list. `app_manifest.proto` has no SchemaConfigPb and no
schema_config field. The previous manifest schema flag, field 9 and schema path are removed;
there is no compatibility parsing.

`{}` is the **only** way to declare an App without configurable values. Empty files,
`null`, `[]`, ordinary property/value JSON, malformed JSON and nonstandard Schema content
are errors. Packaging and SDK error messages must explicitly instruct: **Use `{}` in
`schema_description.json` for an App without configuration.**

Example:

```text
package/
├── kavio_package.json
├── schema_description.json  # Required even when contents are {}
└── icon.png
```

## 2. Schema format

Nonempty schema uses Config Definition v2.0: `definition_version: "2.0"`, required
`title` and nonempty `fields` array (up to 128 fields). Each field requires `key`,
`name`, `category`, `description`, `type`, `value`. Other supported attributes are
`minimum`, `maximum`, `min_length`, `max_length`, `required`, `read_only`, `unit`,
`placeholder`, `multiline`, `show_value`, `options`, `custom`. The packager validates
field types, numeric ranges, enum options and initial values.

`min_length` / `max_length` use the UTF-8 encoded byte length. Package validation
and the Device SDK apply the same rule.

```json
{
  "definition_version": "2.0",
  "title": "显示设置",
  "fields": [
    {"key":"show_cpu","name":"CPU","category":"显示",
     "description":"是否绘制 CPU 数据","type":"boolean","value":true}
  ]
}
```

Do not add `schemaConfig` to `kavio_package.json`. Do not generate or ship
`config/default.json`: Device SDK derives the initial config from `fields[].value`.

## 3. Device lifecycle

SDK initializes the Schema and stored config before establishing the IPC Session.
A missing user configuration is generated from field defaults. Invalid stored
settings cause an error; the App decides whether to restore defaults.

C++:

```cpp
Json::Value current = client.GetAppConfig();
current["show_cpu"] = false;
client.UpdateAppConfig(current);
// After a Host SET, inside IClientMessageHandler::OnMessage:
// if (packet.header().msg_type() == kavio::ipc::proto::MSG_P2A_REQ_SET_APP_CONFIG)
//     ApplyConfig(client.GetAppConfig());
```

Python:

```python
current = client.get_app_config()
current["show_cpu"] = False
client.update_app_config(current)
# After a Host SET, inside on_message:
# if package.header.msg_type == proto.MSG_P2A_REQ_SET_APP_CONFIG:
#     apply_config(client.get_app_config())
```

Host SET is validated and saved **before** its successful packet reaches the
normal `OnMessage()` handler. GET and failed SET are handled internally. Local
App API updates do not trigger `OnMessage()`.

## 4. Host synchronization

Windows follows `DeviceConnected` → `StartAuthorization()` → `ChannelReady` →
`RefreshDeviceApps()` → `ListApps(true)` → `HandleListResponse()`.
It then sends `QueryAppConfig(app_id)` for each running Device Component, and
again when an App starts later. USB `GetAppConfigRequestPb` (340) returns `GetAppConfigResultPb` (340),
including Schema and current JSON. USB `SetAppConfigRequestPb` (341) returns
`SetAppConfigResultPb` (341), containing the committed JSON. Both use the
shared `RspPackagePb.status_code` / `status_message` for errors; there is no
GET/SET operation field or duplicated App-specific status.
`{}` Schema indicates no configuration. There is no unsolicited device
configuration push; Windows pulls through the dedicated USB GET request.

SET sends the complete JSON only. Device SDK validates and persists it before
queuing a successful SET for `OnMessage()` and replying with the effective config JSON. Locally changed configuration requires a fresh GET to update Windows
cache. Platform only routes request/reply bytes, not App-specific JSON.

## 5. Dependency

Device SDK builds vendored JsonCpp 1.9.5 as a static library. The SDK CMake package
propagates its include path and built static library, so C++ App users do not install
system JsonCpp. Python implementation uses the standard `json` library.
