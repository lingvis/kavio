# Kavio SDK

Kavio SDK 是 Kavio App 的公开开发接口集合，分为设备端 Device SDK 和 Windows Host SDK。App 只依赖公共 SDK，不依赖设备 Runtime、硬件平台或 Windows Host 的私有实现。

```text
kavio_sdk/
├── common/logging/          # 公共日志模块
├── device/                  # Device App SDK：C++ / Python / Lua 5.4
└── host/                    # Windows x64 Host Component C ABI
```

## Device SDK

Device SDK 使用统一 C++ Runtime 管理 App 生命周期、设备 Session、AppView Presentation 和事件分发，并通过稳定 C ABI 向 Python、Lua 开放相同能力。

三种正式开发语言：

- C++17
- Python 3.10+
- Lua 5.4

核心对象模型：

```text
App
├── Presentation
│   ├── FrameBuffer / FrameLease
│   └── optional Widget + LVGL Backend
└── Device
    ├── RX / TX
    ├── USB App Channel
    ├── Keyboard / Mouse
    ├── H.264
    └── App Configuration
```

AppView 是所有 KavioApp 的显示入口，但 LVGL 不是必选依赖。自定义 Renderer 可以直接绘制 SDK 提供的 BGRA8888 FrameBuffer；普通 UI 可以使用统一 Widget API 和可选 LVGL Backend。

详细说明：[`device/README.md`](device/README.md)

跨语言能力矩阵：[`../docs/modules/sdk/Kavio_Device_SDK_API_Matrix.md`](../docs/modules/sdk/Kavio_Device_SDK_API_Matrix.md)

## C++ 入口

普通 C++ App 使用：

```cmake
find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH)

target_link_libraries(my_app PRIVATE Kavio::App)
```

Widget App 额外链接：

```cmake
target_link_libraries(my_app PRIVATE Kavio::UI::Core Kavio::UI::LVGL)
```

`Kavio::DeviceSdk` 提供低层/高级 C++ 接口。普通 App 不需要直接使用 `KavioClient`、Protobuf 或 JsonCpp。

## Python 入口

高层 Python 包为：

```python
import kavio
from kavio import App
from kavio import ui
```

Python Native Binding 只调用稳定 C ABI，不创建独立 IPC Session。FrameBuffer 通过 Buffer Protocol 直接访问 AppView 映射；RX Frame 和 H.264 Packet 提供只读 Buffer Protocol。

Python 设备能力统一通过高层 `kavio` 包和稳定 C ABI 提供，不维护独立的 Python IPC/SHM 协议实现。

## Lua 入口

Lua 5.4 使用：

```lua
local kavio = require("kavio")
```

Lua App 由 SDK 自带 `kavio_lua_runner` 启动。Runner、Lua VM 和 Binding 属于 Device SDK，不要求设备 App 宿主增加独立 Lua Runtime 类型。

## Host SDK

Windows Host Component 使用：

```text
kavio_sdk/host/include/kavio/KavioHost.h
```

Host SDK 使用稳定 C ABI。Host DLL 在 Windows PC 上独立构建，设备端 `kavio` 工具不负责 Host 编译。

详细说明：[`host/README.md`](host/README.md)

## 构建

仓库根目录 `kavio` 是正式工程工具：

```bash
./kavio list
./kavio build sdk --release
./kavio build <project-key> --release
./kavio pack <project-key> --release
```

每个真实 App 工程由 `kavio_project.json` 自描述；Templates 使用 `kavio_project.json.template`，只通过 `./kavio new` 实例化。语言决定 Device Builder 和基础语言 Binding；`sdk.modules` 当前唯一可选公共模块为 `ui`；`package` 保存最终安装/运行元数据。正式 Schema：

```text
kavio_app/tools/kavio_project.schema.json
```

完整规则：[`../docs/modules/sdk/Kavio_SDK_APP_BUILD_GUIDE.md`](../docs/modules/sdk/Kavio_SDK_APP_BUILD_GUIDE.md)

## 版本与 ABI

Device SDK 版本唯一来源：

```text
device/include/kavio/KavioSdkVersion.h
```

跨语言 C ABI 单独使用 `KAVIO_ABI_VERSION` 管理兼容性。SDK 版本和 C ABI 版本不是同一概念。
