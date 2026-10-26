# Kavio SDK

Kavio SDK 是 Kavio App 的公开开发接口集合，用于开发运行在 Kavio Device 上的 Device Component，以及运行在 Windows 上的 Host Component。SDK 向 App 提供稳定的软件能力接口，并隔离具体 SoC、板卡和 Kavio Runtime 私有实现。

SDK 按运行位置拆分为两个独立入口：

```text
kavio_sdk/
├── README.md
├── device/                     # Kavio 开发板侧 SDK
│   ├── README.md
│   ├── CMakeLists.txt
│   ├── build.sh
│   ├── include/kavio/          # C++ 公共 API
│   ├── src/                    # SDK 实现
│   ├── proto/                  # Device SDK 协议定义
│   ├── py/kavio_sdk/           # Python Device SDK
│   ├── tools/
│   └── 3rdParty/
└── host/                       # Windows Host Component SDK
    ├── README.md
    └── include/kavio/KavioHost.h
```

## 1. SDK 与 Kavio App

一个 Kavio App 至少包含一个 Device Component，也可以按需增加 Host Component。

```text
Windows                                      Kavio Device
┌─────────────────────────┐                  ┌─────────────────────────┐
│ Host Component          │                  │ Device Component        │
│ Windows x64 DLL         │◄── USB Channel ─►│ C++ executable          │
│                         │                  │ or Python script        │
│ Optional App Panel      │                  │                         │
└─────────────────────────┘                  │ RX / TX / Input / Config│
                                             └─────────────────────────┘
```

- **Device Component**：App 在 Kavio Device 上的主体，可读取 RX 视频、创建 TX 图层、发送键盘鼠标事件、读取运行状态和处理 App 配置。
- **Host Component**：可选的 Windows x64 DLL，用于调用 Windows 能力、提供 App Panel，并通过 USB Channel 与 Device Component 双向交换业务数据。
- **USB Channel**：Host Component 与 Device Component 的 App 专用双向消息通道。业务协议由 App 自己定义。

App 开发者只需要使用公开 SDK API，不需要依赖 Kavio Runtime 的内部通信、进程管理或视频管线实现。

## 2. 选择开发入口

### Device Component：C++

使用：

```text
kavio_sdk/device/
```

Native C++ App 不直接加入 SDK 源码树。先独立构建 `kavio_sdk/device`，再通过生成的 CMake package 链接预编译 SDK：

```cmake
set(KAVIO_DEVICE_SDK_DIR "${REPOSITORY_ROOT}/kavio_sdk/device/build")
find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH)

target_link_libraries(my_device_app PRIVATE Kavio::DeviceSdk)
```

这样多个 App 共用同一份 SDK 构建产物，不会在各自构建目录中重复编译 SDK 源码。

业务代码通常只需要：

```cpp
#include <kavio/KavioClient.h>
```

详细说明见 [`device/README.md`](device/README.md)。

### Device Component：Python

Python SDK 位于：

```text
kavio_sdk/device/py/kavio_sdk/
```

当前 Python SDK 提供 Device 生命周期、RX/TX、USB Channel、运行状态、输入事件和 App 配置相关能力。详细说明见 [`device/README.md`](device/README.md)。

### Host Component：Windows DLL

使用：

```text
kavio_sdk/host/
```

Host SDK 是 header-only C ABI，不需要单独构建 SDK。Windows 工程只需加入：

```text
kavio_sdk/host/include
```

并包含：

```cpp
#include <kavio/KavioHost.h>
```

详细说明见 [`host/README.md`](host/README.md)。

## 3. Device SDK 公开能力

| 能力 | C++ API | 说明 |
| --- | --- | --- |
| 生命周期 | `KavioClient::Start` / `Stop` | 建立和释放 Device Component 与 Kavio Runtime 的连接 |
| 运行状态 | `GetRuntimeData` / `GetRxInfo` | 查询 RX、TX、输入设备等运行状态 |
| RX 视频 | `SetRxParams` / `ReadLatestRxFrame` | 启用 RX 取帧并读取最新视频帧 |
| TX 图层 | `CreateTxSurface` / `TxSurface::WriteFrame` | 创建 BGRA8888 TX Surface 并提交 App 图像 |
| TX 控制 | `SetTxParams` | 控制 TX 是否启用及 App 图层层级 |
| H.264 编码流 | `SubscribeH264Stream` / `ReadH264Packet` | 订阅 Platform 公共编码流，通过只读 SHM 读取数据包 |
| 输入事件 | `SendInputEvent` | 发送键盘、鼠标事件 |
| USB Channel | `SendCommChannelData` | 向同一 App 的 Host Component 发送业务数据 |
| App 配置 | `GetAppConfig` / `UpdateAppConfig` | SDK 自动处理 GET/SET；成功 SET 通过 `OnMessage` 通知业务代码 |
| 运行统计 | `QueryStats` | 查询当前运行统计信息 |

Host 与 Device 的业务消息应通过 USB Channel 传输。C++ Device API 支持发送 **1 字节到 1 MiB** 的单条完整数据报。

## 4. Host SDK 公开能力

Host Component 必须实现以下入口：

```text
KavioHostCreate
KavioHostStart
KavioHostStop
KavioHostDestroy
KavioHostOnData
```

按需实现：

```text
KavioHostPrepareStop
KavioHostOnDeviceState
KavioHostOpenPanel
```

Kavio Runtime 通过 `KavioHostApi` 向 DLL 提供：

- USB Channel 数据发送；
- Device 在线状态查询；
- 当前 App Channel ID；
- Host Component 重启请求；
- 统一日志；
- App 私有数据目录。

详细 ABI、生命周期和最小实现见 [`host/README.md`](host/README.md)。

## 5. Public / Internal 边界

### Device C++ 公共头文件

```text
device/include/kavio/KavioClient.h
device/include/kavio/KavioSdkVersion.h
device/include/kavio/Status.h
device/include/kavio/Types.h
```

业务代码不要直接包含 `device/src/internal/` 下的文件。这些文件属于 SDK 私有实现，不构成 App API。

### Host 公共头文件

```text
host/include/kavio/KavioHost.h
```

Host Component 不需要链接 Kavio Windows 程序或引用其内部头文件。

### Python 公共包

```text
device/py/kavio_sdk/
```

Python protobuf 文件由 `device/proto/*.proto` 生成，不应手工修改生成文件。

## 6. SDK 版本

Device SDK 版本唯一来源：

```text
kavio_sdk/device/include/kavio/KavioSdkVersion.h
```

版本格式：

```text
major.minor.patch
```

当前源码版本：

```text
3.0.0
```

Kavio Runtime 在 Device Component 建立连接时检查 SDK 版本是否满足设备端最低接入版本。App 应使用与目标设备软件版本匹配的 SDK 构建。

Host Component 使用稳定 C ABI，通过 `struct_size` 判断结构体字段是否可用，不使用 Device SDK 版本号作为 Host ABI 条件。

## 7. 第三方依赖

Device C++ SDK 使用随构建提供的 protobuf `3.21.12` 与 JsonCpp `1.9.5`：

```text
kavio_sdk/device/3rdParty/protobuf-3.21.12/
kavio_sdk/device/3rdParty/jsoncpp-1.9.5/
```

这些目录是 SDK 构建依赖，不属于 App 公共 API；实际依赖源码需在构建前准备。

Kavio SDK 不把 OpenCV 作为公共依赖。App 如果需要 OpenCV、FFmpeg 或其他库，应在自己的 App 工程中声明和管理。

## 8. 与 Demo 工程配合使用

推荐仓库结构：

```text
<repository>/
├── README.md
├── kavio_sdk/
└── kavio_app/
```

`kavio_app` 提供可直接参考的完整 App：

| Demo | Device | Host | 主要用途 |
| --- | --- | --- | --- |
| `demo_rx_tx` | C++ | 无 | RX、TX Surface、输入事件和 Device 生命周期 |
| `demo_communication` | C++ | DLL | USB Channel、Windows 数据采集和 App Configuration |
| `demo_panel_image` | C++ | DLL | App Panel、图片传输和 TX 图层联动 |
| `demo_metrics_panel` | Python | DLL | Python Device Component、USB Channel 和 App Panel |
| `demo_virtual_hdmi` | C++ | DLL | 虚拟显示捕获、图像流传输及 App Panel |

第一次开发建议从 `kavio_app/README.md` 开始，再根据目标选择对应 Demo 和 SDK 文档。

## 9. Kavio App Package

Kavio App 安装包统一使用：

```text
.kap
```

`.kap` 表示 **Kavio App Package**。App 的 Device Component、可选 Host Component、图标、配置定义和安装清单统一由 App 工程的打包工具生成。
