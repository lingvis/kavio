# Kavio App Demo

`kavio_app` 提供一组可直接编译、运行和打包的 Kavio App 示例，面向需要开发自定义 Kavio App 的开发者。

每个 Demo 一级目录都是一个独立 App，对应一个 `appId` 和一个 `.kap` 安装包；`tools/` 保存公共打包工具。Demo 重点展示 Kavio SDK 的标准开发方式，不要求 App 开发者了解 Windows 与设备之间的底层传输实现。

## 1. App 开发架构

一个 Kavio App 至少包含 Device Component，也可以按需要增加 Host Component、App Configuration 和 App Panel。

```text
Windows                                      Device
┌─────────────────────────┐                  ┌─────────────────────────┐
│ Host Component          │                  │ Device Component        │
│ Windows x64 DLL         │◄── USB Channel ─►│ Native executable       │
│                         │                  │ or Python entry script  │
│ Optional App Panel      │                  │                         │
└─────────────────────────┘                  │ RX / TX / Input / Config│
                                             └─────────────────────────┘
```

### Device Component

Device Component 是 App 在 Kavio 设备侧运行的主体，当前支持两种形式：

- **Native**：C++ 可执行文件，链接 `kavio_sdk/device` 预先构建的静态库；
- **Python**：Python 入口脚本，随 App 一起打包 Kavio Python SDK 和运行依赖。

Device Component 可以按业务需要使用：

- RX：读取输入视频帧；
- TX：创建并更新输出 Surface；
- Input：发送键盘、鼠标等输入事件；
- USB Channel：与 Windows Host Component 双向交换 App 自定义数据；
- App Configuration：通过 SDK 查询、修改完整 JSON 配置。

C++ Demo 使用 `IClientMessageHandler::OnMessage()` 接收 protobuf 消息。业务请求异步提交，
以 `track_id` 关联响应；收到 TX 创建成功消息后再打开本地共享内存。Python Demo 使用 Python SDK 的
`on_message()`。App 需要自行处理业务超时，不应在消息回调中阻塞等待另一条响应。

### Host Component

Host Component 是可选的 Windows x64 DLL，用于实现必须在 Windows 侧完成的业务，例如：

- 获取 Windows 系统数据；
- 文件选择、图片解码等 Windows 能力；
- 创建自定义 App Panel；
- 通过 USB Channel 与 Device Component 交换业务数据。

Host Component 使用 `kavio_sdk/host/include` 中的 Host API 编译，最终统一输出为：

```text
<app>/output/host/host.dll
```

### USB Channel

Host Component 与 Device Component 之间的 App 自定义通信统一称为 **USB Channel**。

USB Channel 是双向消息通道，业务协议由 App 自己定义。常见做法是把 Host 与 Device 共用的协议声明放在：

```text
<app>/common/
```

Native Device API 中对应 `SendCommChannelData`、`OnMessage(MSG_P2A_DATA_COMM_CHANNEL)` 等接口；Host API 中通过 `send` 和 `KavioHostOnData` 收发数据。Python Demo 对这些接口做了轻量封装。

App 开发只需要围绕 USB Channel 设计自己的消息协议，不需要处理底层 USB 传输细节。

### App Configuration 与 App Panel

App Configuration 的配置项与 App Panel 都是可选能力，且相互独立；但 `schema_description.json` 文件是必需的，无配置时内容为 `{}`：

- **App Configuration**：通过 `package/schema_description.json` 描述标准配置项，由 Kavio UI 生成配置页面；配置读写、校验和持久化由 Device SDK 管理；
- **App Panel**：由 Host Component 创建自定义 Windows 界面，适合文件拖放、预览、交互控制等场景。

## 2. 推荐仓库结构

当前 Demo 的构建脚本使用相对路径查找 Kavio SDK 和打包依赖，推荐保持以下目录关系：

```text
<repository>/
├── kavio_app/                  # 本目录
└── kavio_sdk/
    ├── device/
    └── host/
```

其中：

- Native Device Component 使用 `kavio_sdk/device/build/KavioDeviceSdkConfig.cmake` 引用预构建 SDK；
- Python Device Component 使用 `kavio_sdk/device/py/kavio_sdk`；
- Host Component 使用 `kavio_sdk/host/include`；
- `.kap` 打包工具读取 `kavio_sdk/device` 中统一维护的 App Manifest 和版本定义，不依赖私有 Platform。

## 3. 单个 App 的目录结构

```text
kavio_app/<app_name>/
├── README.md                   # App 功能、构建、打包和验证说明
├── common/                     # 可选，Host/Device 共用业务协议
├── device/                     # Device Component 源码和构建入口
│   ├── CMakeLists.txt          # Native App 使用
│   ├── build.sh
│   └── ...
├── host/                       # 可选，Windows Host Component
│   ├── *.sln
│   ├── *.vcxproj
│   └── HostComponent.cpp
├── package/                    # 安装包元数据和静态资源
│   ├── kavio_package.json
│   ├── icon.png
│   └── schema_description.json # 必需；无配置写 {}
├── output/                     # 构建后的待打包产物
│   ├── device/
│   └── host/
└── dist/                       # 最终 .kap
```

`package/` 只保存安装包描述和静态资源，不保存编译产物。构建结果统一暂存到 `output/`，打包工具再把两部分合并成 `.kap`。

## 4. 构建 Device Component

### Native Device Component

先在与 App 相同的目标架构/工具链环境中构建一次 Device SDK：

```bash
cd kavio_sdk/device
./build.sh -r
```

Native App 通过 `kavio_sdk/device/build/KavioDeviceSdkConfig.cmake` 引用已经构建好的 Device SDK 和静态库。

进入对应 Demo 的 `device/`：

```bash
cd kavio_app/demo_communication/device
./build.sh -r
```

在 x86 Linux 上交叉编译时传入工具链：

```bash
./build.sh -r --toolchain /path/to/aarch64-toolchain.cmake
```

构建产物会自动暂存到：

```text
<app>/output/device/
```

### Python Device Component

Python App 不生成 Native 可执行文件。对应 `build.sh` 负责：

- 检查 Python 源码；
- 暂存 App Python 文件；
- 暂存 Kavio Python SDK；
- 合并运行依赖；
- 清理构建机产生的 Python 缓存文件。

示例：

```bash
cd kavio_app/demo_metrics_panel/device
./build.sh
```

## 5. 构建 Host Component

包含 `host/` 的 Demo 使用 Visual Studio 工程构建 Host Component。

1. 使用 Visual Studio 2015 或更高版本打开对应 `.sln`；
2. 选择 `x64`；
3. 选择 `Debug` 或 `Release`；
4. 编译 Host 工程。

当前 Demo 工程使用 `v140` 工具集。若本机 Visual Studio 未安装该工具集，可以安装对应 C++ 工具集，或者由 Visual Studio 重定向到已安装的工具集。

Post-Build Event 会把 DLL 自动暂存为：

```text
<app>/output/host/host.dll
```

## 6. 检查和打包 App

`kavio_app.py` 用于发现 App、检查构建产物并生成 `.kap`，不负责执行 Device 或 Host 编译。

```bash
cd kavio_app
python3 kavio_app.py list
python3 kavio_app.py status demo_communication
python3 kavio_app.py package demo_communication
```

默认输出：

```text
<app>/dist/<app_name>-<version>.kap
```

打包前需要满足：

- `package/kavio_package.json` 有效；
- `icon` 指向实际存在的 64×64 PNG；
- 清单中声明的 Device/Host 产物已经存在于 `output/`；
- 同级 `schema_description.json` 必须存在且符合标准 Schema，或为 `{}`（无配置）；
- Python 环境安装 `protobuf>=3.20.3`。SDK 构建负责生成 Python Protobuf 绑定文件。

打包前先运行 `kavio_sdk/device/build.sh` 生成
`kavio_sdk/device/py/kavio_sdk/proto_py/app_manifest_pb2.py`。
无配置的 App 也必须在 `package/schema_description.json` 写入 `{}`，不能省略或写空文件。

完整打包规则见 [`docs/modules/app/Kavio_Application_Packaging_Guide.md`](../docs/modules/app/Kavio_Application_Packaging_Guide.md)；
Schema 格式与数据持久化规则见 [`Kavio_App_Configuration_Spec.md`](../docs/modules/app/Kavio_App_Configuration_Spec.md)。

## 7. 新建自己的 App

建议从功能最接近的 Demo 复制一个新目录，然后至少修改：

1. App 目录名；
2. `package/kavio_package.json` 中的 `appId`、`name`、`version`、`description` 和 `icon`；
3. Device Component 的入口、构建目标和业务代码；
4. 如果需要 Windows 能力，修改 Host Component；
5. Host/Device 共用的 USB Channel 协议；
6. 可选的 App Configuration 和 App Panel。

不要把 Kavio SDK 复制到 App 源码目录中。App 只引用仓库中的公共 SDK，打包阶段再根据运行时类型准备最终产物。

## 8. Demo 列表

| Demo | Device | Host | 主要用途 |
| --- | --- | --- | --- |
| `demo_rx_tx` | Native | 无 | RX、TX Surface、Input 和 Device SDK 生命周期 |
| `demo_communication` | Native | DLL | USB Channel 请求/响应、Windows 指标采集、App Configuration |
| `demo_panel_image` | Native | DLL | App Panel、PNG/GIF 解码、大块数据分块传输、TX 位置同步 |
| `demo_metrics_panel` | Python | DLL | Python Device Component、USB Channel、Windows 指标和 App Panel |
| `demo_virtual_hdmi` | Native | DLL | Virtual Display、DXGI 捕获、I/P tile 流式传输、App Panel |

第一次接入建议先阅读 `demo_rx_tx`；需要 Host/Device 双端通信时参考 `demo_communication`；需要自定义 Windows UI 时参考 `demo_panel_image` 或 `demo_metrics_panel`；需要连续图像采集和低延迟 Host/Device 推流时参考 `demo_virtual_hdmi`。
