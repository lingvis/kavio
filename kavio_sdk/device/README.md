# Kavio Device SDK

Kavio Device SDK 用于开发运行在 Kavio Device 上的 KavioApp，正式支持 C++17、Python 3.10+ 和 Lua 5.4。三种语言共用同一套 C++ App Runtime、Device Runtime、Presentation Runtime 和稳定 C ABI。

## 1. 架构

```text
C++ API ───────────────┐
Python kavio ── C ABI ─┼── AppRuntime
Lua kavio ───── C ABI ─┘      ├── DeviceRuntime ── KavioClient
                               ├── EventRuntime
                               └── PresentationRuntime ── AppViewSurface
                                          ├── FrameBuffer
                                          └── Widget + LVGL Backend (optional)
```

所有 KavioApp 都具有 AppView Presentation。LVGL 不是核心 Runtime 的必需依赖：

- FrameBuffer App 可以直接写入 SDK 提供的 BGRA8888 AppView 画布；
- Widget App 使用 backend-independent Widget tree，再由 LVGL Backend 渲染；
- C++ 可通过 LVGL Backend Native Extension 实现复杂专用控件。

## 2. 主要目录

```text
kavio_sdk/device/
├── include/kavio/              # C++ / C 公共 API
├── src/                        # Runtime 和低层实现
├── bindings/
│   ├── python/                 # Python Native + 高层 kavio 包
│   └── lua/                    # Lua 5.4 Native + Runner + 高层 kavio 包
├── proto/                      # SDK 内部协议源文件
├── tools/                      # CMake Package 生成工具
└── 3rdParty/                   # SDK 构建依赖
```

普通 App 不应包含 `src/`、`src/internal/` 或生成 Protobuf 头文件。

## 3. 构建 SDK

设备构建依赖和固定版本参见 [`docs/DEPENDENCIES.md`](../../docs/DEPENDENCIES.md)。构建前可执行 `./kavio doctor --all` 检查完整 SDK 环境。


从仓库根目录执行：

```bash
./kavio build sdk
./kavio build sdk --release

### Project-scoped SDK build

`./kavio build <project>` configures only the SDK language/UI components required by that project's `kavio_project.json`. A C++ project does not require Python or Lua development packages, and a framebuffer-only project does not require the LVGL backend.

`./kavio build sdk` builds the complete SDK, including all enabled language bindings and UI backends, so the corresponding development dependencies must be available.
```

输出位于：

```text
build/sdk/debug/
build/sdk/release/
```

设备端构建脚本不构建 Windows Host DLL，也不提供 Host/MSBuild 或交叉编译参数。

## 4. C++ 高层 API

普通 App 链接：

```cmake
find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH)

target_link_libraries(my_app PRIVATE Kavio::App)
```

核心入口：

```cpp
#include <kavio/App.h>
#include <kavio/Device.h>
#include <kavio/presentation/Presentation.h>
```

`kavio::App::run()` 统一管理：

- Device Session；
- AppView 创建和映射；
- EventRuntime；
- Presentation；
- Device；
- 停止和资源释放。

高层 `kavio::Device` 只暴露 SDK 自有类型，不暴露 Protobuf、JsonCpp 或低层 `KavioClient` 类型。

### 4.1 Device

高层能力：

- Runtime Data / RX Info；
- RX 参数和最新帧；
- TX 参数和 `DeviceTxSurface`；
- Keyboard / Mouse；
  - 键盘公共键码统一定义在 `include/kavio/Keyboard.h` 的 `kavio::KeyCode`；
  - IPC `input.proto` 只传输 `int32 key_code`，不重复维护 HID KeyCode 枚举；
- USB App Channel；
- App Configuration JSON；
- H.264 订阅和 Packet；
- Device 异步事件。

异步 Handler 由 EventRuntime 转发到 App run thread。

### 4.2 Presentation / FrameBuffer

```cpp
auto& presentation = context.presentation();
kavio::FrameLease frame;
if (presentation.AcquireFrame(frame).ok()) {
    auto* pixels = frame.Data();
    const auto& info = frame.Info();
    // Draw BGRA8888 using info.width / height / stride.
    frame.Present();
}
```

FrameLease 规则：

- 获取、写入、Present/Discard 和销毁必须在同一线程；
- `Present()` 是异步 Frame Ready，不等于 VSync 完成；
- `Discard()` 释放写事务但不发布画面；
- 必须使用 Surface 返回的 stride；
- 当前 AppView 是持续共享画布，不对外承诺双缓冲/三缓冲。

## 5. Widget / LVGL

Widget App 链接：

```cmake
target_link_libraries(my_app PRIVATE
    Kavio::App
    Kavio::UI::Core
    Kavio::UI::LVGL)
```

公共 Widget：

```text
Page
Column
Row
Label
Button
Image
Slider
Switch
Style
State
```

`Kavio::UI::LVGL` 消费 AppRuntime 已创建的 Presentation，不建立第二个 Session 或 AppView。LVGL Partial Flush 直接写入 Presentation FrameLease，并保留 Dirty Rect。

C++ 专用复杂界面可通过：

```cpp
backend.Display();
backend.Screen();
backend.MakeDefault();
```

访问同一 Backend 中的 LVGL Runtime。

## 6. 稳定 C ABI

C++ 公共头文件使用 `#include <kavio/...>`；C ABI 使用 `#include <kavio_c/...>`。

公共 C Header：

```text
include/kavio_c/
├── common.h
├── app.h
├── presentation.h
├── framebuffer.h
├── events.h
├── device.h
└── ui.h
```

ABI 使用 opaque handle、固定宽度字段和可扩展结构体，不暴露 C++ 类型、STL、Protobuf、JsonCpp 或 LVGL 对象。

当前：

```text
KAVIO_ABI_VERSION = 1
```

C++ 编译期断言校验 PixelFormat、Runtime/Input/H.264/Input Event 枚举与 C ABI 数值一致。

## 7. Python

高层包：

```python
from kavio import App, PixelFormat
from kavio import ui
```

Python 使用：

```text
_native.so       Core Binding
_ui_native.so    Widget Binding，按需部署
```

Native Binding 只调用稳定 C ABI。

### FrameBuffer

`FrameLease.buffer` 是可写 Buffer Protocol View，直接指向 AppView FrameBuffer。只要仍有活动 View，Present/Discard 会被拒绝。

### RX / H.264

`ImageFrame` 和 `H264Packet` 使用只读 Buffer Protocol，访问 C++ 已拥有的数据，不再额外复制成 Python `bytes`。

### 线程

Python callback 只在 App run thread 执行。`App.run()` 等待期间释放 GIL；进入 Python callback 时重新获取 GIL。Callback 异常会请求安全停止，并由 `App.run()` 重新抛出。

## 8. Lua 5.4

高层入口：

```lua
local kavio = require("kavio")
```

Lua 使用：

```text
kavio_lua_runner
libkavio_lua.so
kavio/native.so
kavio/ui_native.so    # Widget App 按需部署
```

Lua VM 与 Binding 由 SDK 提供。App Package 的设备入口仍是 native `kavio_lua_runner`，脚本路径作为 Runner 参数。

FrameBuffer 使用受 Lease 生命周期约束的 BufferView；RX/H.264 使用只读 BufferView。Lua callback 同样只在 App run thread 进入 VM。

## 9. 低层 / 高级 C++ API

需要直接控制设备协议或共享内存的高级 C++ 工程可以显式使用：

```text
Kavio::DeviceSdk
kavio/KavioClient.h
kavio/AppViewSurface.h
```

这些接口不属于普通高层 KavioApp 的必需依赖。普通 App 应优先使用 `Kavio::App`、`kavio::Device`、`kavio::Presentation` 和 Widget API。

## 10. App Templates 与 KavioDemo

业务 App 的正式起点位于：

```text
kavio_app/Templates/
```

模板不是可编译工程：它们包含 `kavio_project.json.template`。通过：

```bash
./kavio new --list
./kavio new MyApp --template cpp/kavio_widget --app-id com.example.my-app
```

创建真正包含 `kavio_project.json` 的工程。

模板矩阵：

| 路线 | C++ | Python | Lua |
| --- | ---: | ---: | ---: |
| Direct FrameBuffer | ✓ | ✓ | ✓ |
| Kavio Widget | ✓ | ✓ | ✓ |
| Advanced raw-LVGL UI | ✓ | — | — |

综合 SDK 能力/回归 App 位于：

```text
kavio_app/KavioDemo/{cpp,python,lua}
```

`./kavio list` 只显示真正的工程，不显示 Templates。

详细选择说明见 `kavio_app/Templates/README.md`。

## 11. Runtime 依赖

普通项目不需要手工列出内部 Runtime/Binding 依赖。`kavio_project.json` 只声明语言和公开 SDK 模块：

```json
{
  "language": "python",
  "sdk": {
    "modules": ["ui"]
  }
}
```

构建工具根据语言自动加入 App Runtime 和语言 Binding；声明 `ui` 时再自动加入 UI Core、LVGL Backend 和相应脚本语言 UI Binding。FrameBuffer-only App 不声明 `ui`。

## 12. 动态库布局

标准设备包布局：

```text
device/
├── <entry>
├── kavio/                 # Python/Lua Binding，按语言存在
└── lib/
    ├── libkavio_app_runtime.so
    ├── libkavio_ui_core.so       # optional
    ├── libkavio_ui_lvgl.so       # optional
    └── libkavio_lua.so           # Lua only
```

RPATH 约定：

- Native C++ App：`$ORIGIN/lib`；
- `libkavio_ui_lvgl.so`：`$ORIGIN`；
- Python `_native.so` / `_ui_native.so`：`$ORIGIN/../lib`；
- Lua Runner：`$ORIGIN/lib`；
- Lua `native.so` / `ui_native.so`：`$ORIGIN/../lib`。

## 13. 构建和打包

App 构建规则见：

- [`../../docs/modules/sdk/Kavio_SDK_APP_BUILD_GUIDE.md`](../../docs/modules/sdk/Kavio_SDK_APP_BUILD_GUIDE.md)
- [`../../docs/modules/sdk/Kavio_Device_SDK_API_Matrix.md`](../../docs/modules/sdk/Kavio_Device_SDK_API_Matrix.md)

根命令：

```bash
./kavio list
./kavio build <project-key> --release
./kavio pack <project-key> --release
```

`build` 只构建/暂存设备端。声明 Host Component 的 App 仅在 `pack` 阶段检查 PC 已生成的 Host DLL。


## Protobuf compiler

The Device SDK builds Protobuf from the bundled `protobuf-3.21.12` source and exports the matching compiler through the generated CMake package:

```cmake
Kavio::Protoc
KAVIO_DEVICE_SDK_PROTOC_EXECUTABLE
```

Apps that generate additional `.proto` sources should use the SDK-exported compiler instead of resolving an arbitrary `protoc` from `PATH`.
