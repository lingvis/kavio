# Kavio 应用包完整打包说明

> 适用工具：`kavio_app/kavio_app.py（统一项目工具）`、`kavio_app/tools/build_package.py（低级打包器）`  
> 输出格式：`.kap（Kavio App Package）`  
> 当前清单版本：`manifestVersion = 1`（取自 `KAVIO_MANIFEST_VERSION`）

本文档说明如何将一个包含设备端程序、Host Component（主机组件）、配置文件、模型、资源和依赖库的完整目录，构建为 Kavio 可安装应用包。

当前 `kavio_app/tools/build_package.py（低级打包器）` 只生成**未签名开发包**。正式发布包还需要经过 Kavio 官方签名流程。

---

## 0. 与统一业务项目的关系

业务项目采用“静态打包定义”和“运行产物”分离结构：

```text
kavio_app/<project_name>/
├── package/                 # 进入版本库
│   ├── kavio_package.json
│   ├── schema_description.json # 必需；无配置写 {}
│   ├── icon.png           # 必需；64×64 PNG
│   └── assets/            # 可选其他静态资源
├── output/                  # 不进入版本库
│   ├── device/
│   └── host/                # 可选
└── dist/                    # 最终安装包
```

设备端和 Host Component（主机组件）分别构建到 `output/`。推荐命令：

```bash
cd kavio_app
python3 kavio_app.py status demo_communication
python3 kavio_app.py package demo_communication
```

`kavio_app.py` 会创建临时暂存目录，将 `package/` 中的清单和静态资源与 `output/` 中清单声明的运行目录合并，再调用 `tools/build_package.py`。

`tools/build_package.py` 是低级打包器。只有自动化流水线已经自行准备好一个完整的“待打包目录”时才直接调用。

---

## 1. 打包流程总览

```text
在 device/ 中编译 ARM Device Component
→ 在 host/ 中编译 Windows Host Component（可选）
→ 双端产物分别进入项目 output/
→ package/ 维护清单、配置和图标
→ kavio_app.py status 检查静态文件和双端产物
→ kavio_app.py package 临时合并 package/ + output/
→ build_package.py 生成未签名 .kap
→ 开发模式安装测试，或进入官方签名流程
```

最终安装包可以包含：

- 设备端主程序或 Python 源码；
- 设备端动态库、模型和算法资源；
- 配置文件；
- Host Component DLL 及其依赖；
- 图标和展示资源；
- Python 环境定义、依赖列表或离线环境归档。

---

## 2. 前置条件

### 2.1 构建机

需要安装：

- Python 3.9 或更高版本；
- 已编译完成的Device Component（设备组件）；
- 如果包含 Host Component，已编译完成的 Host Component DLL 及运行依赖。

普通业务项目检查：

```bash
python3 kavio_app/kavio_app.py --help
```

低级打包器检查：

```bash
python3 kavio_app/tools/build_package.py --help
```

两个 Python 脚本都只使用标准库。

### 2.2 设备端 Python App

如果打包 Python App，设备镜像应预先部署：

- Miniforge（精简 Conda 发行版）；
- `tar（归档工具）`；
- 可访问的 Conda 和 Pip 软件源，或包内提供离线环境归档。

是否允许在线安装 Python 环境，由 Platform 的 `AppServiceConfigPb（应用服务配置）` 决定。

---

## 3. 目录规范

### 3.1 仓库中的项目目录

```text
kavio_app/<project_name>/
├── package/
│   ├── kavio_package.json
│   ├── config/
│   └── assets/
├── output/
│   ├── device/
│   │   ├── app_entry
│   │   ├── lib/
│   │   ├── models/
│   │   └── resources/
│   └── host/
│       ├── host.dll
│       ├── helper.dll
│       └── resources/
└── dist/
```

规则：

1. `package/` 根目录必须包含 `kavio_package.json`。
2. 配置、图标等静态路径相对于 `package/`。
3. `device.directory` 和 `host.directory` 对应的运行目录相对于 `output/`。
4. 主入口文件必须真实存在于 `output/<directory>/`。
5. `package/` 和被使用的 `output/` 目录均不允许符号链接。
6. `output/` 和 `dist/` 不应提交版本库。

### 3.2 临时待打包目录

`kavio_app.py package` 内部会形成如下临时目录：

```text
<temporary>/package/
├── kavio_package.json
├── device/
├── host/                    # 可选
├── schema_description.json   # 必需
└── assets/                  # 可选（含图标）
```

这个临时目录才是 `tools/build_package.py --source-dir` 的实际输入。开发者通常不需要手工创建或保留它。

### 3.3 低级打包器的基本规则

直接调用 `build_package.py` 时，待打包目录必须满足：

1. 清单和全部运行文件已经位于同一根目录下；
2. 路径必须使用 `/`，不能使用绝对路径、`..`、反斜杠或盘符；
3. 不允许包含符号链接；
4. 根目录不能预先存在 `manifest.json` 或 `signature.json`；
5. 输出包应放在待打包目录之外；
6. 除 `kavio_package.json` 和生成文件外，普通文件都会被递归打包。

因此不要把源码仓库直接作为低级打包器输入。

---

## 4. kavio_package.json 完整字段说明

### 4.1 完整示例

```json
{
  "packageConfigVersion": 1,
  "appId": "com.kavio.demo.app-communication",
  "version": "1.0.0",
  "name": "Demo App Communication",
  "description": "Windows Host Component and device App communication sample",
  "icon": "assets/icon.png",
  "device": {
    "directory": "device",
    "runtime": "native",
    "entry": "demo_communication",
    "arguments": [],
    "autoStart": false
  },
  "host": {
    "directory": "host",
    "binary": "host.dll",
    "panel": false
  }
}
```

**注意：** 与 `kavio_package.json` 同级必须有 `schema_description.json`，文件为 `{}` 表示无配置，其余内容必须符合 Config Definition v2.0。SDK 运行时从字段 `value` 生成默认值。详见 [`Kavio_App_Configuration_Spec.md`](Kavio_App_Configuration_Spec.md)。

### 4.2 顶层字段

| 字段 | 必填 | 类型 | 默认值 | 说明 |
|---|---:|---|---|---|
| `packageConfigVersion` | 是 | 非负整数 | 无 | 打包配置格式版本，当前必须为 `1` |
| `appId` | 是 | 字符串 | 无 | 应用永久唯一标识；KavioWin 与 Platform 使用同一算法由该值独立生成稳定 ChannelID |
| `version` | 是 | 字符串 | 无 | 应用包版本，工具不强制语义化版本格式，建议使用 `主版本.次版本.修订版本` |
| `name` | 是 | 字符串 | 无 | 应用管理页面显示名称 |
| `description` | 否 | 字符串 | 空字符串 | 应用说明 |
| `icon` | 否 | 相对路径 | 空 | 应用 PNG（便携式网络图形）图标文件名/路径，相对于待打包目录根目录；必须为 64×64 像素 |
| `device` | 是 | 对象 | 无 | Device Component（设备组件） 定义 |
| `host` | 否 | 对象 | 无 | Windows Host Component（主机组件）定义；省略后为纯Device Component（设备组件） 包 |

当前打包工具要求 `device（设备端定义）` 必须存在，因此不支持只包含Host Component DLL 的 Host-only（纯上位机）包。

`icon（应用图标）` 为可选字段：

- 省略 `icon` 或设置为空字符串时，应用包不携带自定义图标，KavioWin 使用内置默认应用图标；
- 填写 `icon` 后，目标文件必须存在于待打包目录中，文件格式必须为 PNG，分辨率必须严格为 **64×64**；
- 图标文件不存在、不是 PNG 或分辨率不是 64×64 时，打包直接失败；KavioWin 在安装外部应用包时也会再次校验；
- KavioWin 默认图标资源 ID 为 `IDR_PNG_APP_DEFAULT`，资源路径为 `res\AppDefault.png`。

### 4.3 appId 规则

`appId（应用唯一标识）` 必须满足：

- 最长 128 个字符；
- 只能使用小写 ASCII（美国信息交换标准代码）字母、数字、`.` 和 `-`；
- 首字符和末字符必须是字母或数字；
- 不允许连续两个 `.`；
- 安装、升级和通信期间必须保持稳定；
- 安装时会计算稳定 ChannelID；若与已安装的其他 `appId` 或内置业务 Channel 保留 ID 冲突，安装直接失败；
- 不要把版本号写入 `appId`。

合法示例：

```text
com.kavio.sample
com.company.camera-service
app1.device-task
```

非法示例：

```text
Com.Kavio.Sample
.com.kavio.sample
com..kavio.sample
com_kavio_sample
com.kavio.sample.
```

### 4.4 device 字段

| 字段 | 必填 | 类型 | 默认值 | 路径基准 | 说明 |
|---|---:|---|---|---|---|
| `directory` | 是 | 相对路径 | 无 | 待打包目录根目录 | 设备端全部文件所在目录 |
| `runtime` | 否 | 字符串 | `native` | — | `native（原生程序）` 或 `python（Python 脚本）` |
| `entry` | 是 | 相对路径 | 无 | `device.directory` | 设备端主入口程序或 Python 主脚本 |
| `arguments` | 否 | 字符串数组 | `[]` | — | AppService 启动 App 时追加的参数列表 |
| `autoStart` | 否 | 布尔值 | `false` | — | Platform 启动后是否由 AppService 自动启动一次 |
| `python` | Python App 必填 | 对象 | 无 | — | Python 环境定义 |

#### arguments 参数规则

每一个数组元素会作为一个独立命令行参数传递，不经过 Shell（命令解释器）解析。

正确示例：

```json
"arguments": [
  "--config",
  "config/device.json",
  "--log-level",
  "info"
]
```

不要写成：

```json
"arguments": [
  "--config config/device.json --log-level info"
]
```

除非设备程序本身就是把整段内容当作一个参数处理。

#### autoStart 行为

`autoStart=true` 表示：

- Platform 启动并完成 AppService 初始化后，自动尝试启动一次；
- 不依赖 USB 是否连接；
- 不依赖Host Component（主机组件）是否启动；
- App 异常退出后不会自动保活或重复拉起。

### 4.5 host 字段

| 字段 | 必填 | 类型 | 默认值 | 路径基准 | 说明 |
|---|---:|---|---|---|---|
| `directory` | 是 | 相对路径 | 无 | 待打包目录根目录 | Host Component（主机组件）及其全部依赖文件所在目录 |
| `binary` | 是 | 相对路径 | 无 | `host.directory` | Host Component（主机组件）主 DLL |
| `panel` | 否 | 布尔值 | `false` | — | 是否提供 App Panel；为 `true` 时应用必须实现面板导出接口 |

`host.directory` 中可以包含应用依赖 DLL、配置、模板和资源。所有普通文件都会保留相对路径打入包内。

Host Component 不在应用包清单中保存独立 Host Component API（主机组件接口）版本，也不单独做 SDK 版本校验。应用与宿主之间的 C ABI 通过 `struct_size` 做结构边界保护。

如果应用不需要Host Component DLL，可以完全省略 `host`：

```json
{
  "device": {
    "directory": "device",
    "runtime": "native",
    "entry": "device_app",
    "arguments": [],
    "autoStart": false
  }
}
```

### 4.6 schema_description.json 固定文件

每个 App 都必须包含与 `kavio_package.json` 同级的 `schema_description.json`。
无配置统一使用 **`{}`**；文件缺失、空文件、非对象、普通配置值 JSON 或无效 Config Definition
必须拒绝，并提示“无配置请使用 `{}`”。不要在 `kavio_package.json` 声明 Schema 信息，
不要生成 `config/default.json`。SDK 启动时读取字段 `value` 生成默认配置。

**enum 关联值与自定义输入示例：**

```json
{
  "key": "server_address",
  "name": "服务地址",
  "category": "网络",
  "description": "预设地址或自定义地址",
  "type": "enum",
  "value": "192.168.0.1",
  "show_value": true,
  "required": true,
  "options": [
    {"label": "server1", "value": "192.168.0.1"},
    {"label": "server2", "value": "192.168.0.2"}
  ],
  "custom": {
    "label": "自定义",
    "placeholder": "请输入地址",
    "min_length": 1,
    "max_length": 256
  }
}
```

下拉框显示 `server1/server2/自定义`，预设项下方显示只读实际值；“自定义”下方开放输入。GET/SET 仍仅传输 `server_address` 对应的字符串。`show_value` 默认关闭，`custom` 不声明则不可自定义。完整校验规则参见当前配置规范。

App Panel 不属于 App Configuration。是否提供 App Panel 由 `host.panel` 单独声明。

### 4.7 路径基准汇总

| 字段 | 相对路径基准 |
|---|---|
| `icon` | 待打包目录根目录 |
| `device.directory` | 待打包目录根目录 |
| `device.entry` | `device.directory` |
| `device.python.environmentArchive` | `device.directory` |
| `device.python.environmentDefinition` | `device.directory` |
| `device.python.requirements` | `device.directory` |
| `host.directory` | 待打包目录根目录 |
| `host.binary` | `host.directory` |

配置文件中统一使用 `/` 作为分隔符，即使在 Windows 上打包，也不能写 `\`。

---

## 5. 原生双端应用打包示例

### 5.1 目录

```text
kavio_app/camera_service/
├── package/
│   ├── kavio_package.json
│   ├── schema_description.json
│   └── assets/icon.png
├── output/
│   ├── device/
│   │   ├── camera_app
│   │   ├── lib/libcamera_algorithm.so
│   │   └── models/detector.bin
│   └── host/
│       ├── host.dll
│       └── camera_helper.dll
└── dist/
```

### 5.2 配置

```json
{
  "packageConfigVersion": 1,
  "appId": "com.company.camera",
  "version": "1.0.0",
  "name": "Camera Service",
  "description": "Camera processing App with Windows configuration app",
  "icon": "assets/icon.png",
  "device": {
    "directory": "device",
    "runtime": "native",
    "entry": "camera_app",
    "arguments": [
      "--model",
      "models/detector.bin"
    ],
    "autoStart": true
  },
  "host": {
    "directory": "host",
    "binary": "host.dll",
    "panel": true
  }
}
```

### 5.3 构建

```bash
cd kavio_app
python3 kavio_app.py status camera_service
python3 kavio_app.py package camera_service
```

默认输出到 `camera_service/dist/`。

---

## 6. 纯设备原生 App 打包示例

适用于没有 Windows Host Component（主机组件），只需要通过应用管理页面安装、启动和停止Device Component（设备组件） 的场景。

### 6.1 目录

```text
kavio_app/device_task/
├── package/
│   ├── kavio_package.json
│   └── assets/icon.png
├── output/device/
│   ├── task_app
│   ├── config/task.json
│   └── resources/
└── dist/
```

### 6.2 配置

```json
{
  "packageConfigVersion": 1,
  "appId": "com.company.device-task",
  "version": "1.0.0",
  "name": "Device Task",
  "description": "Device-only background task",
  "icon": "assets/icon.png",
  "device": {
    "directory": "device",
    "runtime": "native",
    "entry": "task_app",
    "arguments": [
      "--config",
      "config/task.json"
    ],
    "autoStart": true
  }
}
```

未签名的纯设备包只要求设备端开发模式开启。

---

## 7. Python App 打包

### 7.1 Python 字段

当 `device.runtime` 为 `python` 时，必须提供 `device.python`：

| 字段 | 必填 | 默认值 | 说明 |
|---|---:|---|---|
| `pythonVersion` | 否 | 空，在线创建时按 Python 3 处理 | Python 版本，例如 `3.11` |
| `installMode` | 否 | `auto` | `auto（自动）`、`archive（离线归档）` 或 `online（在线安装）` |
| `environmentArchive` | 条件必填 | 空 | 离线环境归档，相对于 `device.directory` |
| `environmentDefinition` | 条件必填 | 空 | `environment.yml（Conda 环境定义）`，相对于 `device.directory` |
| `requirements` | 条件必填 | 空 | `requirements.txt（Pip 依赖列表）`，相对于 `device.directory` |

三个环境输入至少存在一个：

```text
environmentArchive
environmentDefinition
requirements
```

### 7.2 installMode 选择规则

#### auto

```text
存在 environmentArchive
→ 优先恢复离线环境

不存在 environmentArchive
→ 使用 environmentDefinition 或 requirements 在线创建
```

#### archive

强制使用 `environmentArchive`。没有归档文件时安装失败。

#### online

强制在线创建环境。必须提供 `environmentDefinition` 或 `requirements`，并且设备端必须允许在线安装。

### 7.3 在线创建：environment.yml

目录示例：

```text
python_app_package/
├── kavio_package.json
└── device/
    └── python/
        ├── main.py
        ├── environment.yml
        ├── requirements.txt
        ├── config/
        └── resources/
```

配置：

```json
{
  "packageConfigVersion": 1,
  "appId": "com.company.python-task",
  "version": "1.0.0",
  "name": "Python Task",
  "description": "Python device application",
  "device": {
    "directory": "device/python",
    "runtime": "python",
    "entry": "main.py",
    "arguments": [
      "--config",
      "config/task.json"
    ],
    "autoStart": false,
    "python": {
      "pythonVersion": "3.11",
      "installMode": "online",
      "environmentDefinition": "environment.yml",
      "requirements": "requirements.txt"
    }
  }
}
```

设备安装时：

1. 使用 Miniforge 的 Conda 根据 `environment.yml` 创建独立环境；
2. 如果同时提供 `requirements.txt`，在该环境中继续执行 Pip 安装；
3. 使用环境中的 Python 对入口脚本执行语法编译检查；
4. 任一步失败，整个应用安装失败。

### 7.4 在线创建：仅 requirements.txt

配置：

```json
{
  "packageConfigVersion": 1,
  "appId": "com.company.python-simple",
  "version": "1.0.0",
  "name": "Python Simple Task",
  "device": {
    "directory": "device/python",
    "runtime": "python",
    "entry": "main.py",
    "arguments": [],
    "autoStart": false,
    "python": {
      "pythonVersion": "3.11",
      "installMode": "online",
      "requirements": "requirements.txt"
    }
  }
}
```

设备会先创建指定 Python 版本的空环境，再安装 `requirements.txt` 中的依赖。

### 7.5 离线环境归档

配置：

```json
{
  "packageConfigVersion": 1,
  "appId": "com.company.python-offline",
  "version": "1.0.0",
  "name": "Python Offline Task",
  "device": {
    "directory": "device/python",
    "runtime": "python",
    "entry": "main.py",
    "arguments": [],
    "autoStart": false,
    "python": {
      "pythonVersion": "3.11",
      "installMode": "archive",
      "environmentArchive": "environment.tar.zst"
    }
  }
}
```

归档必须能被设备上的 `tar -xf（归档解压命令）` 解压，并在环境根目录提供：

```text
bin/python
```

如果存在：

```text
bin/conda-unpack
```

AppService 会在恢复后执行它，以修正环境路径。

归档中不能包含越界路径。安装时会检查环境中的符号链接安全性。

### 7.6 Python 依赖文件限制

为防止应用包自行指定未批准的软件源，设备端会拒绝依赖文件中的以下内容：

```text
://
git+
--index-url
--extra-index-url
--find-links
```

`requirements.txt` 还不能包含：

- 以 `-` 开头的指令；
- `包名 @ URL` 形式；
- 行末续行反斜杠。

Conda Channel（Conda 软件源）和 Pip Index（Pip 软件源）统一由 Platform 的系统配置提供，不能由单个应用包覆盖。

---

## 8. 执行打包

业务项目推荐使用：

```bash
cd kavio_app
python3 kavio_app.py status <project_name>
python3 kavio_app.py package <project_name>
```

以下命令只适用于已经手工准备好完整暂存目录的低级场景。

### 8.1 Linux 或 macOS

```bash
python3 kavio_app/tools/build_package.py \
  --source-dir /absolute/path/to/package_source \
  --output /absolute/path/to/output/app.kap
```

### 8.2 Windows PowerShell

```powershell
python .\kavio_app/tools/build_package.py `
  --source-dir D:\KavioPackages\package_source `
  --output D:\KavioPackages\output\app.kap
```

配置文件内部的路径仍必须使用 `/`：

```json
"directory": "device"
```

不能写：

```json
"directory": "device\\app"
```

### 8.3 成功输出

```text
Created unsigned development package: /path/to/app.kap
app_id: com.company.app
files: 12
Enable both Host development mode and Device development mode before installation.
```

纯设备包会提示只开启设备开发模式。

### 8.4 退出码

| 退出码 | 含义 |
|---:|---|
| `0` | 打包成功 |
| `2` | 参数、配置、路径或文件校验失败 |
| 其他 | Python 或文件系统异常 |

---

## 9. 最终安装包内容

输出文件本质上是 ZIP（压缩归档）格式，扩展名为：

```text
.kap
```

未签名包示例：

```text
app.kap
├── manifest.json
├── device/
│   └── ...
├── host/
│   └── ...
├── config/
│   └── ...
└── assets/
    └── ...
```

当前脚本不会写入：

```text
kavio_package.json
signature.json
```

正式签名工具会基于生成后的清单和文件摘要生成 `signature.json（签名文件）`。

---

## 10. 生成的 manifest.json

### 10.1 生成规则

打包工具会：

1. 读取 `kavio_package.json` 与同级必需的 `schema_description.json`，校验固定格式并将描述文件纳入清单及摘要；
2. 直接构造 `AppManifestPb（应用安装清单 Proto 对象）`，不再维护第二套 Manifest JSON 字典或手工字段映射；
3. 将两端入口路径转换为包根目录下的完整相对路径；
4. 枚举全部待分发文件，并直接写入 `AppManifestPb.files`；
5. 使用 protobuf 的 `MessageToJson（Proto JSON 序列化）` 生成 `manifest.json`。

`kavio_sdk/device/proto/app_manifest.proto` 定义 Manifest。打包工具使用 SDK
构建产生的 Python Protobuf 绑定；`schema_description.json` 固定打入包内，Manifest
不含 SchemaConfig 字段，不生成默认配置文件。

### 10.2 生成示例

```json
{
  "manifestVersion": 1,
  "appId": "com.kavio.demo.app-communication",
  "version": "1.0.0",
  "name": "Demo App Communication",
  "description": "Demo",
  "device": {
    "runtime": "native",
    "entry": "device/demo_communication",
    "arguments": [],
    "autoStart": false
  },
  "host": {
    "binary": "host/host.dll",
    "panel": false
  },
  "files": [
    {
      "path": "device/demo_communication",
      "size": "123456",
      "sha256": "..."
    },
    {
      "path": "host/host.dll",
      "size": "234567",
      "sha256": "..."
    }
  ]
}
```

不要手工修改包内的 `manifest.json`。修改清单或任意文件后，文件摘要将不再一致，正式签名也会失效。

注意：`AppManifestFilePb.size` 的 Proto 类型是 `uint64`。按照 protobuf JSON 标准，64 位整数在 JSON 中序列化为字符串，因此生成结果为 `"size": "123456"`。Host 和 Device 均通过 protobuf JSON 解析器读取该字段，不应再手工改写为另一套 JSON 表达。

---

## 11. 签名与开发模式

### 11.1 当前脚本只生成未签名包

`kavio_app/tools/build_package.py` 用于开发和测试，不生成官方数字签名。

未签名包在应用管理页面会显示类似：

```text
签名未验证
```

### 11.2 开发模式要求

| 包类型 | 未签名安装和运行条件 |
|---|---|
| 纯Device Component（设备组件） | 开启设备开发模式 |
| 同时包含Device Component（设备组件） 和 Host Component DLL | 同时开启设备开发模式和上位机开发模式 |
| 只包含Host Component DLL | 当前打包工具不支持 |

安装未签名包时，KavioWin 应显示风险提示，用户必须明确确认。

关闭某一侧开发模式后：

- 对应侧已安装的未签名代码保留；
- 正在运行的未签名代码会被停止；
- 未签名代码不能再次启动；
- 重新开启对应开发模式后才恢复运行资格。

### 11.3 正式发布

正式发布应使用 Kavio 官方签名：

- 摘要算法：SHA-256；
- 签名算法：Ed25519（爱德华曲线数字签名算法）；
- 签名文件：`signature.json`。

如果包声明使用 Kavio 官方 `key_id（密钥标识）`，但签名验证失败，系统应按损坏或篡改包处理，开发模式也不能绕过。

---

## 12. 安装和启动后的路径语义

### 12.1 原生 App

Device Component（设备组件）Service 部署后会把原生主入口设置为可执行权限：

```text
0755
```

启动时由 AppService 指定的普通用户运行，默认用户由设备系统配置决定。

### 12.2 Python App

Python App 使用独立虚拟环境中的：

```text
<env>/bin/python
```

执行 `app.entry（应用入口）`。

### 12.3 AppService 注入的环境变量

通过正式流程启动时，AppService 会注入：

```text
KAVIO_APP_ID
KAVIO_APP_INSTANCE_ID
KAVIO_APP_DATA_DIR
```

应用需要保存运行期数据或业务配置时，应优先写入 `KAVIO_APP_DATA_DIR（应用私有数据目录）`。

### 12.4 不建议手工启动

需要与 KavioWin 通信的 App 不应通过 Shell（命令行环境）直接运行。

AppService 启动时会完成：

```text
真实 PID → appId → runtimeInstanceId
```

绑定。IPCServer（进程间通信服务端）通过内核 `SO_PEERCRED（套接字对端凭据）` 验证真实 PID。手工启动即使设置相同环境变量，也没有 AppService 的 PID 注册，因此无法绑定自己的受管 IPC Endpoint，也不能通过 SDK 使用该 App 的业务 Channel。

正式启动应在 KavioWin 应用管理页面点击：

```text
App启动
```

Host Component DLL 则独立点击：

```text
Host启动
```

两者生命周期相互独立。

---

## 13. 包内容检查

### 13.1 查看文件列表

Linux：

```bash
unzip -l app.kap
```

Windows PowerShell：

```powershell
tar -tf .\app.kap
```

### 13.2 查看清单

```bash
unzip -p app.kap manifest.json
```

或：

```powershell
tar -xOf .\app.kap manifest.json
```

### 13.3 建议检查项

确认：

- `manifestVersion` 与 `KAVIO_MANIFEST_VERSION` 一致（当前为 `1`）；
- `appId` 正确；
- `device.entry` 指向包内真实文件；
- `host.binary` 指向正确 DLL；
- 没有把源码、日志、密钥或临时文件打入包中；
- `files` 列表覆盖全部分发文件；
- Python 环境输入路径正确；
- 包中没有 `kavio_package.json`；
- 未签名开发包中没有 `signature.json`。

---

## 14. 推荐发布流程

### 14.1 开发测试包

```text
准备暂存目录
→ 生成未签名 .kap
→ 开启对应开发模式
→ 安装并验证
→ 根据日志修复问题
```

### 14.2 正式包

```text
从干净构建产物准备暂存目录
→ 生成未签名基础包
→ 执行内部安全检查
→ 使用 Kavio 官方签名工具签名
→ 验证签名和文件摘要
→ 归档包及构建记录
→ 发布
```

### 14.3 升级包

升级时：

- 保持 `appId` 不变；
- 增加 `version`；
- 重新构建全部文件摘要；
- 重新执行官方签名；
- 不要手工复制旧包中的 `manifest.json` 或 `signature.json`。

---

## 15. 常见错误与处理

| 错误信息或现象 | 原因 | 处理方法 |
|---|---|---|
| `kavio_package.json` 不存在 | 输入目录错误或缺少配置文件 | 确认 `--source-dir` 指向暂存目录根目录 |
| `unsupported packageConfigVersion` | 配置版本不是 1 | 将 `packageConfigVersion` 改为 `1` |
| `appId may contain only...` | ID 含大写、下划线或其他非法字符 | 使用小写字母、数字、`.`、`-` |
| `device.directory must identify a real directory` | 设备目录不存在或是符号链接 | 修正目录路径并使用真实目录 |
| `device.entry is not a packaged file` | 入口路径错误或入口文件未复制到暂存目录 | 按 `device.directory` 的相对路径修正 `entry` |
| `host.binary is not a packaged file` | 主 DLL 路径错误 | 按 `host.directory` 的相对路径修正 `binary` |
| `Config Definition validation failed` | 已声明的 Config Definition 缺失或内容非法 | 根据错误中的字段路径修正 Config Definition 或字段级 value |
| `Python applications require at least one environment input` | Python App 没有环境归档、YAML 或 requirements | 至少提供一种环境输入 |
| `symbolic-link files/directories are not allowed` | 输入目录包含符号链接 | 将目标文件真实复制到暂存目录 |
| `manifest.json is generated...` | 根目录预先存在清单 | 删除根目录旧清单，让工具重新生成 |
| `signature.json is generated...` | 根目录预先存在签名文件 | 删除旧签名，生成包后重新签名 |
| 安装提示“签名未验证” | 使用的是开发打包脚本生成的未签名包 | 开启对应开发模式，或进入官方签名流程 |
| Python 在线环境安装失败 | 设备未允许在线安装、源不可用或依赖不兼容 | 检查 `allow_online_python_install`、Conda Channel、Pip Index 和依赖版本 |
| Python 依赖文件被拒绝 | 文件包含 URL、Git 或自定义软件源指令 | 删除包内源配置，使用 Platform 统一软件源 |
| App 可以运行但不能通信 | App 被手工启动、对端 Endpoint 未上线，或 `appId`/ChannelID 一致性校验失败 | 通过 AppService 正式启动，确认双端组件状态并检查 `appId` |
| Host 应用无法加载 | DLL 架构错误或依赖 DLL 缺失 | 使用与 KavioWin 相同架构，并把依赖放入 `host.directory` |

---

## 16. 当前打包工具的边界

当前 `kavio_app/tools/build_package.py` 有以下明确边界：

1. 只生成未签名包，不负责官方签名。
2. 必须包含Device Component（设备组件），不支持纯上位机包。
3. 没有忽略文件配置；暂存目录中的普通文件默认全部打包。
4. 不验证 `version` 是否符合语义化版本规范。
5. 不验证图标格式和尺寸，建议使用透明背景 PNG（便携式网络图形），尺寸不低于 128×128。
6. 声明 Config Definition 时严格校验字段级 value、enum 预设选项及可选 custom 约束。
7. 不负责交叉编译设备程序或构建Host Component DLL。
8. 不自动分析 DLL 或 SO（共享对象）依赖，依赖文件由开发者自行准备。
9. 不自动生成 Python 离线环境归档。
10. 不保证不同时间构建的 ZIP 字节完全一致，因此正式发布应归档最终包的 SHA-256。

---

## 17. 打包前检查清单

### 17.1 身份和版本

- [ ] `appId` 唯一、稳定且格式合法。
- [ ] `version` 已更新。
- [ ] `name` 和 `description` 准确。
- [ ] 最低 Platform/KavioWin 版本要求合理。

### 17.2 设备端

- [ ] `device.directory` 正确。
- [ ] `device.entry` 存在。
- [ ] 设备程序架构正确。
- [ ] 所需 SO、模型、配置和资源已经复制。
- [ ] `arguments` 每个元素是独立参数。
- [ ] `autoStart` 符合预期。

### 17.3 上位机

- [ ] 是否确实需要 Host 应用。
- [ ] `host.binary` 存在。
- [ ] DLL 架构与 KavioWin 一致。
- [ ] 依赖 DLL 已复制到 Host 目录。
- [ ] Host Component 使用公开 `KavioHost.h`，并按 `struct_size` 规则访问宿主接口。

### 17.4 Python

- [ ] `runtime` 为 `python`。
- [ ] 至少提供一种环境输入。
- [ ] `installMode` 与提供的文件匹配。
- [ ] 在线依赖不包含自定义源和 URL。
- [ ] 离线环境归档包含 `bin/python`。
- [ ] 入口脚本可通过 Python 语法检查。

### 17.5 安全和内容

- [ ] 暂存目录不含私钥、密码、日志或调试转储。
- [ ] 没有符号链接。
- [ ] 根目录没有旧的 `manifest.json` 或 `signature.json`。
- [ ] 输出路径位于暂存目录外。
- [ ] 包内文件列表已经人工检查。

### 17.6 发布

- [ ] 开发包已在正确的开发模式下安装测试。
- [ ] App 启动、停止、通信和日志正常。
- [ ] Python 环境安装路径已验证。
- [ ] 正式包已完成官方签名。
- [ ] 最终包 SHA-256 已归档。

---

## 18. 最小命令速查

### 双端业务项目

```bash
cd kavio_app
python3 kavio_app.py status demo_communication
python3 kavio_app.py package demo_communication
```

### 纯设备业务项目

```bash
cd kavio_app
python3 kavio_app.py status demo_rx_tx
python3 kavio_app.py package demo_rx_tx
```

### 低级打包器

仅对已经完整组装的暂存目录使用：

```bash
python3 kavio_app/tools/build_package.py \
  --source-dir ./package_source \
  --output ./output/app.kap
```

### 查看包内容

```bash
unzip -l ./output/app.kap
unzip -p ./output/app.kap manifest.json
```

### 安装未签名双端包

```text
KavioWin 应用管理
→ 开启上位机开发模式
→ 开启设备开发模式
→ 安装
→ 确认风险
```

### 正式启动

```text
选择应用
→ App启动
→ 如有Host Component DLL，再点击 Host启动
```
