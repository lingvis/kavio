# Kavio App Configuration 规范

本文说明现行 App Schema Description、Device SDK 配置管理与上位机 GET/SET 协议。
对外唯一生效的配置规范以本文件为准；字段格式版本在正文标注，不以文件名区分版本。

## 1. 应用包要求

每个 App 的 `package/` 目录必须包含 `schema_description.json`，与 `kavio_package.json` 同级。
打包工具将其原样放入 `.kap`，并加入 `AppManifestPb.files` 完整性校验列表。
`app_manifest.proto` 不包含 `SchemaConfigPb`、`schema_config` 或兼容旧字段的解析逻辑。

```text
package/
├── kavio_package.json
├── schema_description.json   # 必需；无配置时写 {}
└── icon.png
```

- **无配置的唯一有效声明为 `{}`**。
- 文件缺失、空文件、`null`、`[]`、普通配置值 JSON、非法 JSON 或不符合规范的 Schema 都必须报错。
- 不在 `kavio_package.json` 中增加 `schemaConfig`，也不再生成 `config/default.json`。
- 配置项发布初始值直接取自 Schema 中的 `fields[].value`。

## 2. Schema 字段格式

非空 Schema 使用 **Config Definition 2.0**：`definition_version` 为 `"2.0"`，
必须具有非空 `title` 和 `fields` 数组（1～128 项）。字段至少包含
`key`、`name`、`category`、`description`、`type`、`value`。

支持 `boolean`、`integer`、`number`、`string` 和 `enum` 字段类型。
扩展属性包括 `minimum`、`maximum`、`min_length`、`max_length`、`required`、
`read_only`、`unit`、`placeholder`、`multiline`、`show_value`、`options`、`custom`。
打包工具校验字段类型、取值、范围及枚举选项；字符串长度按 UTF-8 编码后的字节数计算。

```json
{
  "definition_version": "2.0",
  "title": "显示设置",
  "fields": [
    {
      "key": "show_cpu",
      "name": "CPU",
      "category": "显示",
      "description": "是否显示 CPU 数据",
      "type": "boolean",
      "value": true
    }
  ]
}
```

完整字段规则以当前打包工具及 Device SDK 的校验实现为准。构建输入的
`schema_description.json` 上限为 256 KiB；配置 JSON 的 IPC 传输上限为 1 MiB。

## 3. Device SDK 配置管理

Device SDK 在创建 IPC Session 前加载 Schema 和用户配置。首次运行时根据 `fields[].value`
生成默认 JSON，并将用户配置保存到 `KAVIO_APP_DATA_DIR/config.json`。
已有配置不合法时启动失败，由 App 决定是否显式恢复默认值。

C++ 接口：

```cpp
Json::Value config = client.GetAppConfig();
config["show_cpu"] = false;
auto status = client.UpdateAppConfig(config);
// 如需恢复默认值：client.ResetAppConfigToDefaults();
```

Python 接口：

```python
config = client.get_app_config()
config["show_cpu"] = False
client.update_app_config(config)
# 如需恢复默认值：client.reset_app_config_to_defaults()
```

上述示例仅适用于具有 `show_cpu` 字段的 App；无配置的 App 不应提交配置更新。
本地更新接口不主动触发 `OnMessage()` 或向 Windows 推送配置。

收到上位机 SET 后，SDK **先校验、持久化并更新内存快照**，成功后才通过普通
`OnMessage()` / `on_message()` 向业务代码交付 `MSG_P2A_REQ_SET_APP_CONFIG`。
App 在消息回调中调用 `GetAppConfig()` / `get_app_config()` 获取最新值并应用业务逻辑。
GET 和失败的 SET 由 SDK 内部响应，不交付业务层；App 不需要手工读写 JSON 文件或构造配置响应。

## 4. 上位机配置同步

KavioWin 完成设备授权并进入 `ChannelReady` 后，刷新 App 列表，并对运行中的
Device Component 主动发起 `QueryAppConfig(app_id)`；后续 App 进入 `Running` 时也主动查询。

```text
KavioWin AppManager
  → USB SystemChannel GET/SET（340/341）
  → Platform 按 app_id 定向路由
  → Device IPC GET/SET（75/76、77/78）
  → Device SDK 查询 / 校验 / 持久化
  → 原路径返回 Schema 和当前完整配置
```

- GET 返回 Schema 和当前配置 JSON；SET 提交完整 JSON，成功响应返回实际生效的配置。
- 失败结果使用通用 `RspPackagePb.status_code` 和 `status_message`，不另建业务状态字段。
- `schema_description.json` 为 `{}` 表示 App 无配置项；**不代表 Schema 文件可以省略**。
- Device SDK 不主动发送配置快照；App 本地修改后，上位机通过下一次 GET 获取最新内容。
- Platform 只转发请求与响应，不解析 App 的业务 JSON。

## 5. 第三方依赖

C++ Device SDK 使用构建目录内的 JsonCpp 1.9.5 静态库；由 SDK 的 CMake package
向业务 App 传播头文件和链接依赖，不要求 App 开发者另行安装系统 JsonCpp。
Python SDK 使用标准库 `json`。
