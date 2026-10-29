# Kavio App Configuration（应用配置）规范 v1.0

> 历史记录，代码不再实现本文格式。当前 App 仅使用 [`Kavio_App_Configuration_Spec_v2_0.md`](Kavio_App_Configuration_Spec_v2_0.md)；新版本把初始值写在每个字段的 `value`，不再使用独立 `config.default`。

## 1. 目的与适用范围

本文定义 Kavio App 的 App Configuration（应用配置）机制，包括 Config Definition（配置定义）、Default Config（默认配置）、完整配置 JSON（数据交换格式）、KavioWin 的 App Settings（应用设置）页面、SystemChannel（系统通道）协议以及 Device Component（设备组件）的处理约束。

这里的 Config Definition 只负责描述“有哪些配置项以及如何校验和展示”，不是多套配置实例，因此当前体系不使用 Profile（配置档案）概念。

该机制适用于以下场景：

- 应用包没有 Windows Host Component（主机组件），仍需在 KavioWin 中配置Device Component（设备组件）；
- 应用包同时提供相互独立的 Config Definition 通用配置和 Windows Host Component 高级配置；
- 配置读取和保存不应依赖应用业务 Channel（业务通道）是否已注册；
- Platform（平台端）只负责可信路由，不理解、不保存业务配置。

## 2. 核心原则

1. **Config Definition 是界面和校验声明，不是当前配置值。**
2. **当前配置由Device Component（设备组件） 自行读取、校验、原子保存和应用。**
3. **KavioWin 每次读取或保存完整配置 JSON，不发送差异字段。**
4. **App Configuration 配置链路只使用 SystemChannel，不占用应用业务 Channel。**
5. **Platform 只按真实进程身份对应的 `app_id（应用标识）` 路由。**
6. **Config Definition 和配置采用严格字段校验，未知字段直接拒绝，避免拼写错误静默生效。**
7. **当前协议不定义配置版本合并、服务端迁移、局部更新和敏感字段加密。**

## 3. manifest（应用清单）声明

### 3.1 Config Definition 声明

```json
{
  "config": {
    "definition": "config/definition.json",
    "default": "config/default.json"
  }
}
```

| 字段 | 必需性 | 说明 |
|---|---:|---|
| `config.definition` | 声明 `config` 时必需 | Config Definition 文件在应用包内的安全相对路径 |
| `config.default` | 可选 | KavioWin 首次展示参数页面时使用的默认完整配置 JSON；不会替代Device Component（设备组件） 自行保存的当前配置 |

未声明 `config` 表示该项目不提供 App Settings（应用设置）页面，这是合法状态。

一旦声明 `config`，打包工具必须严格校验：

- `definition` 文件必须存在且为合法 Config Definition；
- 声明了 `default` 时文件必须存在且内容必须满足 Config Definition；
- Config Definition 或默认值错误时停止打包并报告具体字段和原因。

运行期间如果已安装包中的 Config Definition 文件丢失、损坏或解析失败：

- Device Component 和 Host Component 仍可正常运行；
- KavioWin 不展示 App Settings（应用设置）页面；
- KavioWin 必须向用户明确提示 Config Definition 加载失败原因，不能静默当作“未提供 Config Definition”。

### 3.2 与 App Panel 的关系

App Configuration 和 App Panel 是两个相互独立的可选能力：

- KavioWin 根据 Config Definition 自动生成标准 App Settings 页面；
- App Panel 由 Host Component 自行实现，可用于数据展示、控制、文件操作或其他交互；
- 一个项目可以只提供 App Configuration、只提供 App Panel、同时提供二者，或二者都不提供。

App Configuration 配置链路仍由 SystemChannel 完成，App Panel 与Device Component（设备组件） 的业务数据交换使用该项目自行定义的 Host Component / Device Component 通信协议。

## 4. Config Definition 根对象

Config Definition 文件必须是 UTF-8（统一字符编码）的 JSON 对象，最大 **256 KiB**。
KavioWin 使用 UTF-8 解析 Config Definition；加载 Config Definition 时立即将标题、分类、名称、说明、单位、占位文本和枚举显示名称转换为 UTF-16 并保存在配置窗口模型中，
后续控件创建、布局和刷新直接使用已转换文本。完整配置 JSON 保持 UTF-8，字符串配置值加载到控件时转换为 UTF-16，提交配置时再转换为 UTF-8。

```json
{
  "definition_version": "1.0",
  "title": "示例配置",
  "description": "用于说明整个配置页面。",
  "fields": []
}
```

| 字段 | 类型 | 必需 | 约束 | 说明 |
|---|---|---:|---|---|
| `definition_version` | string（字符串） | 是 | 当前只能为 `1.0` | Config Definition 规范版本 |
| `title` | string | 是 | 非空，最多 128 字节 | 配置页面标题 |
| `description` | string | 否 | 最多 2048 字节 | 配置页面整体说明 |
| `fields` | array（数组） | 是 | 1～128 项 | 配置项定义，按数组顺序展示 |

根对象不允许出现其他字段。

## 5. 配置项通用字段

每个 `fields` 元素必须是对象。以下三个用户可见字段为强制字段：

- `name`：配置名称；
- `category`：配置类别；
- `description`：配置说明。

完整字段如下：

| 字段 | 类型 | 必需 | 适用类型 | 说明 |
|---|---|---:|---|---|
| `key` | string | 是 | 全部 | 配置 JSON 中的唯一键 |
| `name` | string | 是 | 全部 | 配置名称，显示在控件左侧 |
| `category` | string | 是 | 全部 | 配置类别，相邻同类别字段分组展示 |
| `description` | string | 是 | 全部 | 配置说明，显示在控件下方 |
| `type` | string | 是 | 全部 | `boolean`、`integer`、`number`、`string`、`enum` |
| `default` | 与字段类型一致 | 是 | 全部 | 默认值；所有字段都必须提供，以保证完整配置可确定生成 |
| `required` | boolean（布尔值） | 否 | 全部 | 默认 `false`；字符串不能为空，其他类型必须有有效值 |
| `read_only` | boolean | 否 | 全部 | 默认 `false`；界面可展示但不可编辑 |
| `unit` | string | 否 | 全部 | 单位说明，最多 64 字节 |
| `placeholder` | string | 否 | string、integer、number | 输入提示，最多 256 字节 |
| `minimum` | number（数值） | 否 | integer、number | 最小值，包含边界 |
| `maximum` | number | 否 | integer、number | 最大值，包含边界 |
| `min_length` | integer | 否 | string | UTF-8 字节长度下限，0～65535 |
| `max_length` | integer | 否 | string | UTF-8 字节长度上限，0～65535，默认 4096 |
| `multiline` | boolean | 否 | string | 默认 `false`；使用多行编辑框 |
| `options` | array | 是（仅 enum） | enum | 1～256 项枚举选项数组 |

### 5.1 `key` 约束

`key` 必须匹配：

```text
[a-z][a-z0-9_.-]{0,63}
```

即：

- 长度 1～64；
- 首字符必须是小写英文字母；
- 后续只允许小写英文字母、数字、下划线、短横线和点；
- 同一个 Config Definition 内不得重复。

### 5.2 文本长度约束

| 字段 | 最大 UTF-8 字节数 |
|---|---:|
| `name` | 128 |
| `category` | 128 |
| `description` | 2048 |
| `unit` | 64 |
| `placeholder` | 256 |

### 5.3 未知字段

Config Definition 根对象、字段对象和枚举选项对象中的未知字段均视为错误。该规则用于阻止类似 `maximun`、`readOnly` 等拼写错误被静默忽略。

## 6. 支持的字段类型

### 6.1 `boolean（布尔）`

Config Definition 示例：

```json
{
  "key": "report_cpu_usage",
  "name": "上报CPU利用率",
  "category": "上报字段",
  "description": "控制Device Component（设备组件） 是否上报 CPU 总体利用率。",
  "type": "boolean",
  "required": true,
  "default": true
}
```

Windows 控件：复选框。

配置值：JSON `true` 或 `false`，不接受字符串形式的 `"true"`。

### 6.2 `integer（整数）`

```json
{
  "key": "report_interval_ms",
  "name": "上报周期",
  "category": "上报设置",
  "description": "两次周期上报之间的时间间隔。",
  "type": "integer",
  "unit": "ms（毫秒）",
  "minimum": 500,
  "maximum": 60000,
  "required": true,
  "default": 3000
}
```

Windows 控件：单行数值编辑框。

约束：

- 必须是 JSON number（数值），且无小数部分；
- 必须处于 JavaScript/JSON 安全整数范围 `[-9007199254740991, 9007199254740991]`；
- `minimum`、`maximum` 也必须是安全整数；
- 可选边界均为闭区间。

### 6.3 `number（浮点数）`

```json
{
  "key": "gain",
  "name": "增益",
  "category": "图像",
  "description": "设置图像处理增益。",
  "type": "number",
  "minimum": 0.0,
  "maximum": 4.0,
  "default": 1.0
}
```

Windows 控件：单行数值编辑框。

约束：必须是有限数值，不接受 `NaN（非数）` 或无穷大。

### 6.4 `string（字符串）`

```json
{
  "key": "device_name",
  "name": "设备名称",
  "category": "基本信息",
  "description": "设置设备显示名称。",
  "type": "string",
  "required": true,
  "min_length": 1,
  "max_length": 128,
  "placeholder": "请输入设备名称",
  "default": "Kavio Device"
}
```

Windows 控件：单行或多行编辑框。

长度按 UTF-8 字节数计算。`required=true` 时空字符串非法。

### 6.5 `enum（枚举）`

```json
{
  "key": "log_level",
  "name": "日志级别",
  "category": "诊断",
  "description": "设置 App 输出日志的最低级别。",
  "type": "enum",
  "required": true,
  "default": "info",
  "options": [
    { "value": "debug", "label": "调试" },
    { "value": "info", "label": "信息" },
    { "value": "warning", "label": "警告" }
  ]
}
```

Windows 控件：只读下拉框。

约束：

- `options` 必须包含 1～256 项；
- 每项只允许 `value` 和 `label`；
- `value`、`label` 必须是非空字符串，且分别不超过 128 个 UTF-8 字节；
- 同一字段内 `value` 不得重复；
- 当前配置值必须等于某个 `value`，保存时不会写入 `label`。

## 7. 字段组合约束

- `minimum`、`maximum` 仅允许用于 `integer` 或 `number`；
- `min_length`、`max_length`、`multiline` 仅允许用于 `string`；
- `options` 仅允许用于 `enum`，且 `enum` 必须声明 `options`；
- `minimum` 不得大于 `maximum`；
- `min_length` 不得大于 `max_length`；
- 每个字段都必须声明类型正确且满足约束的 `default`；
- `default` 必须满足该字段全部类型、范围、长度或枚举约束；
- 不允许依赖 boolean（布尔值）的 `false`、number（数值）的 `0` 或 enum（枚举）的第一项等隐式零值；
- 不支持对象、数组、密码框、文件选择器、颜色、日期和嵌套路径控件；需要这些能力时由 Host Component 提供独立 App Panel。

## 8. 完整配置 JSON

### 8.1 基本格式

```json
{
  "report_interval_ms": 3000,
  "report_cpu_usage": true,
  "report_memory_usage": true,
  "report_disk_usage": true
}
```

### 8.2 约束

- 必须是 JSON 对象；
- 最大 **1 MiB**；
- 键必须全部存在于 Config Definition；未知键直接拒绝；
- KavioWin 保存时输出 Config Definition 中全部字段，不发送局部差异；
- App 对 GET（读取）成功响应时也必须返回当前完整有效配置；
- 非空配置 JSON 必须包含 Config Definition 中的全部字段，缺少任一字段或包含未知字段均拒绝；
- 仅 KavioWin 尚无任何缓存配置时，允许以空字符串作为内部“未初始化”状态，并由全部字段的 `default` 生成首份完整配置；`{}` 不属于有效完整配置；
- 非空输入配置缺少字段时直接拒绝；`default` 仅用于 KavioWin 内部“未初始化”状态生成首份完整配置，Config Definition 不允许省略 `default`；
- App 仍必须独立执行同等级别的业务校验，不能信任 KavioWin 已完成校验。

## 9. SystemChannel 协议

### 9.1 KavioWin 与 Platform

`usb_protocol.proto` 定义：

```proto
message AppConfigRequestPb {
  enum Operation {
    OperationUnknown = 0;
    OperationGet = 1;
    OperationSet = 2;
  }
  string app_id = 1;
  Operation operation = 2;
  bytes config_json = 3;
}

message AppConfigResultPb {
  string app_id = 1;
  AppConfigRequestPb.Operation operation = 2;
  bytes config_json = 3;
  int32 app_error_code = 4;
  string app_error_message = 5;
}

message RspPackagePb {
  HeaderPb header = 1;
  int32 status_code = 2;
  string status_message = 3;
  oneof body {
    AppConfigResultPb app_config_result = 340;
  }
}
```

消息字段号：

- `ReqPackagePb.app_config = 340`；
- `RspPackagePb.app_config_result = 340`。

### 9.2 Platform 与 App IPC（进程间通信）

`ipc.proto` 定义：

```proto
enum AppConfigOperationPb {
  APP_CONFIG_OPERATION_UNKNOWN = 0;
  APP_CONFIG_OPERATION_GET = 1;
  APP_CONFIG_OPERATION_SET = 2;
}

message NtfAppConfigRequestPb {
  uint64 request_id = 1;
  AppConfigOperationPb operation = 2;
  bytes config_json = 3;
}

message NtfAppConfigResponsePb {
  uint64 request_id = 1;
  AppConfigOperationPb operation = 2;
  bytes config_json = 3;
  int32 error_code = 4;
  string error_message = 5;
}
```

消息类型：

- `MSG_NTF_APP_CONFIG_REQUEST = 75`：Platform → App；
- `MSG_NTF_APP_CONFIG_RESPONSE = 76`：App → Platform。

## 10. 路由与身份校验

完整链路：

```text
TabAppManagerPageDlg（应用管理页）
    ↓
AppManager（应用管理器）
    ↓
SystemChannelClient（系统通道客户端）
    ↓ SystemChannel
Platform / AppDaemon
    ↓ 按 app_id 查找真实受管 App
IPCServer
    ↓
KavioClient 回调
    ↓
Device Component（设备组件） 读取或保存配置
    ↑ 原路返回
```

`SystemChannelClient` 对上层分别提供设备会话状态、应用协议消息和内置服务协议消息回调。应用协议消息以完整 `RspPackagePb` 交给 `AppManager`，由 `AppManager` 根据 `body_case` 调用对应业务处理函数。

Platform 必须执行：

1. 校验 USB 请求的 `track_id（跟踪号）`、`app_id`、操作类型和消息大小；
2. 通过 AppService 维护的真实 PID（进程编号）到 `app_id` 映射选择 IPC 会话；
3. 保存待处理请求的 `track_id`、`app_id`、操作和创建时间；
4. App 响应时根据内核提供的对端进程身份重新解析 `app_id`；
5. 校验响应请求号、应用身份和操作类型完全一致；
6. 不解析、不修改、不保存 `config_json`；
7. 15 秒未响应时返回超时错误；
8. App IPC 会话提前关闭时立即返回失败；
9. USB Host 离线时清理待处理请求。

因此，App 不能通过伪造 protobuf 字段为其他应用返回配置。

## 11. GET（读取）流程

1. 用户在 `TabAppManagerPageDlg` 选择应用并点击“读取配置”；
2. KavioWin 发送 `OperationGet`，`config_json` 必须为空；
3. Platform 定向发送 `NtfAppConfigRequestPb`；
4. App 从自己的数据目录读取当前配置；
5. App 读取并独立校验配置；仅首次不存在持久化配置时按 App 自身默认值初始化，然后返回完整配置 JSON；
6. Platform 校验响应身份与请求对应关系后通过 SystemChannel 返回；
7. KavioWin 再按 Config Definition 校验，并更新可视化控件和本地缓存。

App 离线、未安装、未注册 Config Definition 回调、超时或返回非法配置时，界面显示错误，不以安装包默认值冒充设备当前值。

## 12. SET（保存）流程

1. 用户修改可视化控件并点击“保存配置”；
2. KavioWin 校验所有字段并序列化完整配置 JSON；
3. KavioWin 发送 `OperationSet`；
4. App 再次执行完整业务校验；
5. App 使用临时文件、`fsync（强制落盘）` 和原子重命名保存；
6. App 将配置应用到运行态；
7. App 返回保存后实际生效的完整配置 JSON；
8. KavioWin 仅在系统协议和 App 业务结果均成功时更新 App Settings（应用设置）界面缓存。

禁止行为：

- App 收到 SET 后先返回成功、再异步保存；
- 只返回差异字段；
- Platform 替 App 保存业务配置；
- KavioWin 在 App 返回失败时更新缓存；
- 通过业务 Channel 发送 App Configuration请求。

## 13. App SDK 接口

App 注册：

```cpp
callbacks.on_app_config_request =
    [&](const kavio::KavioClient::AppConfigRequest& request) {
        kavio::KavioClient::AppConfigResponse response;
        response.set_request_id(request.request_id());
        response.set_operation(request.operation());

        // GET：读取、校验并返回完整配置。
        // SET：校验、原子保存、应用并返回完整配置。

        client.SendAppConfigResponse(response);
    };
```

约束：

- 响应必须回显相同 `request_id` 和 `operation`；
- 成功时 `error_code=0`，并返回完整配置；
- 失败时 `error_code` 必须非零，并提供可诊断的 `error_message`；
- App 未注册回调时，SDK 自动返回 `kUnsupported（不支持）`；
- `SendAppConfigResponse` 不依赖业务 Channel 就绪状态，但要求 IPC session（会话）有效。

## 14. Windows App Settings（应用设置）页面行为

`CAppConfigWindow` 根据字段类型创建：

| Config Definition 类型 | MFC（微软基础类库）控件 |
|---|---|
| `boolean` | `CButton` 复选框 |
| `integer` | `CEdit` 单行编辑框 |
| `number` | `CEdit` 单行编辑框 |
| `string` | `CEdit` 单行或多行编辑框 |
| `enum` | `CComboBox` 只读下拉框 |

窗口行为：

- 按 `fields` 顺序展示；
- 相邻字段按 `category` 分组；
- 展示 `name`、`unit` 和 `description`；
- 内容超过窗口高度时使用垂直滚动；
- `read_only=true` 时禁用编辑；
- Config Definition 或配置不合法时不创建半有效界面，回退到只读诊断文本；
- 清单声明了有效 Config Definition 且Device Component（设备组件） 已安装时，启用“读取配置”和“保存配置”；
- Host Component 声明并实现 App Panel 时，App Panel 入口独立显示，不依赖 Config Definition；
- Config Definition、Manifest 和配置 JSON 中的字符串使用 UTF-8；Config Definition 的界面描述文本在加载阶段转换为 UTF-16 并由配置窗口直接使用，不在控件创建和布局阶段重复转换。

## 15. 错误处理

状态分两层：

1. SystemChannel 外层状态：`RspPackagePb.status_code` 直接传输 `kavio::StatusCode`，用于请求格式、路由、连接、超时等状态；`status_message` 保存状态描述；
2. App 业务状态：`AppConfigResultPb.app_error_code` 和 `app_error_message` 表示目标 App 对配置内容、持久化和运行态应用的处理结果。

SystemChannel 不定义独立 USB 错误码。错误发生位置直接返回带领域前缀的 `StatusCode`，协议转发层不根据错误字符串重新分类。`StatusCode` 是作用域枚举，使用 `StatusCode::Common_`、`StatusCode::Session_`、`StatusCode::Media_`、`StatusCode::SHM_`、`StatusCode::IPC_` 和 `StatusCode::USB_` 分类；成功状态为 `StatusCode::OK`。日志通过 `StatusCodeToString()` 输出状态码符号名。

Platform、SDK 和 KavioWin 统一使用 `Status` 保存 `StatusCode` 与状态消息。`Status` 定义在独立的 `Status.h` 中，不依赖 `Types.h` 中的 C++17 类型，可直接用于 Windows 协议响应处理。

KavioWin 的界面错误采用“问题，请检查项目”的短句，详细路径和 JSON 解析信息写入日志。常见提示如下：

| 提示 | 检查项 |
|---|---|
| `Config Definition文件不存在` | `config.definition` 路径 |
| `Config Definition路径无效` | `manifest.files` 声明 |
| `Config Definition格式错误` | JSON 语法 |
| `Config Definition版本错误` | `definition_version` |
| `配置字段未知` | 配置键名称 |
| `配置字段缺失` | 完整配置字段 |
| `默认配置不存在` | `config.default` 路径 |

`AppConfigResultPb` 同时返回 `app_error_code` 和 `app_error_message`。KavioWin 只有在以下条件全部满足时才视为成功：

- 收到对应 `track_id` 的响应；
- SystemChannel `status_code == StatusCode::OK`；
- `app_id`、操作类型与待处理请求一致；
- `app_error_code == StatusCode::OK`；
- 返回的完整配置通过 Config Definition 校验。

## 16. 安全与资源上限

| 项目 | 上限/规则 |
|---|---|
| Config Definition 文件 | 256 KiB |
| 字段数量 | 1～128 |
| 配置 JSON | 1 MiB |
| 请求超时 | 15 秒 |
| 身份来源 | IPC 对端真实 PID，不信任 App 自报身份 |
| 路由键 | 受管 App 的 `app_id` |
| 配置持久化 | App 私有数据目录，原子替换 |
| 未知字段 | 拒绝 |

Config Definition 不应包含密码、令牌、私钥等敏感值。当前通用窗口没有秘密字段遮罩与安全存储能力；敏感配置必须使用专门安全方案，不应仅依赖 `read_only`。

## 17. 完整示例

```json
{
  "definition_version": "1.0",
  "title": "系统资源监控配置",
  "description": "配置Device Component（设备组件） 的资源利用率上报周期和上报字段。",
  "fields": [
    {
      "key": "report_interval_ms",
      "name": "上报周期",
      "category": "上报设置",
      "description": "两次资源利用率上报之间的时间间隔。",
      "type": "integer",
      "unit": "ms（毫秒）",
      "minimum": 500,
      "maximum": 60000,
      "required": true,
      "default": 3000
    },
    {
      "key": "report_cpu_usage",
      "name": "上报CPU利用率",
      "category": "上报字段",
      "description": "上报板端CPU总体利用率。",
      "type": "boolean",
      "required": true,
      "default": true
    },
    {
      "key": "report_memory_usage",
      "name": "上报内存利用率",
      "category": "上报字段",
      "description": "上报板端内存利用率。",
      "type": "boolean",
      "required": true,
      "default": true
    },
    {
      "key": "report_disk_usage",
      "name": "上报磁盘利用率",
      "category": "上报字段",
      "description": "上报板端根文件系统空间利用率。",
      "type": "boolean",
      "required": true,
      "default": true
    }
  ]
}
```

对应完整配置：

```json
{
  "report_interval_ms": 3000,
  "report_cpu_usage": true,
  "report_memory_usage": true,
  "report_disk_usage": true
}
```

## 18. 本版本代码位置

- SystemChannel 协议：`kavio_platform/proto/usb_protocol.proto`；
- App IPC 协议：`kavio_sdk/device/proto/ipc.proto`；
- Platform 路由：`kavio_platform/app/AppUSB.cpp`、`IPCServer.h`；
- App SDK：`kavio_sdk/device/include/kavio/KavioClient.h`、`kavio_sdk/device/src/KavioClient.cpp`；
- Windows 动态配置窗口：`kavio_win/KavioWin/AppConfigWindow.h/.cpp`；
- 应用管理页：`kavio_win/KavioWin/TabAppManagerPageDlg.h/.cpp`；
- 管理与协议封装：`AppManager.h/.cpp`、`SystemChannelClient.h/.cpp`；
- 示例 Config Definition：`kavio_app/demo_communication/package/config/definition.json`；
- 示例 App：`kavio_app/demo_communication/device/main.cpp`。
