# Kavio Device SDK API 能力矩阵

本文定义 Kavio Device SDK 在 C++、Python 和 Lua 三种正式语言入口中的公共能力基线。三种语言使用同一套 C++ Runtime 和稳定 C ABI；语言差异只体现在语法和对象封装，不改变设备行为、线程模型或资源生命周期。

## 1. 统一对象模型

| 能力 | C++ | Python | Lua | 语义 |
| --- | --- | --- | --- | --- |
| App | `kavio::App` | `kavio.App` | `kavio.App` | 阻塞运行并统一管理 Session、Presentation、Device 和事件循环 |
| AppContext | `kavio::AppContext` | `kavio.AppContext` | context table | 持有 Presentation 和 Device |
| Presentation | `kavio::Presentation` | `kavio.Presentation` | native Presentation userdata | AppView 全屏呈现 |
| FrameLease | `kavio::FrameLease` | `kavio.FrameLease` | native FrameLease userdata | AppView FrameBuffer 写入事务 |
| Device | `kavio::Device` | `kavio.Device` | native Device userdata | RX/TX、USB、Input、H.264、Config |
| Widget | `kavio::ui::*` | `kavio.ui.*` | `kavio.ui.*` | retained Widget tree |
| LVGL Backend | `kavio::ui::LvglBackend` | `kavio.ui.LvglBackend` | `kavio.ui.LvglBackend` | 可选 Widget 渲染 Backend |

## 2. App 生命周期

| 能力 | C++ | Python | Lua |
| --- | --- | --- | --- |
| 创建 App | `App(options)` | `App(...)` | `App(options)` |
| Ready | `on_ready()` | `on_ready()` | `on_ready()` |
| Tick | `on_tick()` | `on_tick()` | `on_tick()` |
| Stopping | `on_stopping()` | `on_stopping()` | `on_stopping()` |
| 请求停止 | `request_stop()` | `request_stop()` | `request_stop()` |
| 运行 | `run()` | `run()` | `run()` |
| 最后状态码 | `last_status().code` | `last_status` | `last_status()` |

所有生命周期回调均在执行 `run()` 的 App 线程调用。IPC Receive/Dispatch 线程不直接进入 App 业务回调、Python VM 或 Lua VM。

## 3. Presentation / FrameBuffer

| 能力 | C++ | Python | Lua |
| --- | --- | --- | --- |
| Ready / Visible | 支持 | 支持 | 支持 |
| Surface generation | 支持 | 支持 | 支持 |
| Frame metadata | 支持 | 支持 | 支持 |
| Acquire Frame | 支持 | 支持 | 支持 |
| 直接 BGRA8888 写入 | `Data()` | writable Buffer Protocol | writable BufferView |
| Dirty Rect Present | 支持 | 支持 | 支持 |
| Discard | 支持 | 支持 | 支持 |
| Clear | 支持 | 支持 | 支持 |
| Raw Touch | 支持 | 支持 | 支持 |
| Visibility | 支持 | 支持 | 支持 |

约束：FrameLease 是写入事务，不代表独立双缓冲。`Present()` 发布 Frame Ready，但不表示 VSync 或屏幕消费完成。所有绘制必须使用 SDK 返回的 width、height 和 stride。

Python 中仍存在活动 `memoryview` 时禁止 Present/Discard；Lua 中仍存在活动可写 BufferView 时同样禁止提交，以避免缓冲区被复用后仍保留可写引用。

## 4. Device API

| 能力 | C++ | Python | Lua |
| --- | --- | --- | --- |
| Runtime Data | 支持 | 支持 | 支持 |
| RX 状态 | 支持 | 支持 | 支持 |
| RX 参数 | 支持 | 支持 | 支持 |
| 最新 RX Frame | 支持 | 只读 Buffer Protocol | 只读 BufferView |
| TX 参数 | 支持 | 支持 | 支持 |
| 创建 TX Surface | 支持 | 支持 | 支持 |
| TX Surface Write/Clear/Destroy | 支持 | 支持 | 支持 |
| Keyboard | 支持 | 支持 | 支持 |
| Mouse | 支持 | 支持 | 支持 |
| USB Host 状态 | 支持 | 支持 | 支持 |
| App USB Channel | 支持 | 支持 | 支持 |
| App Config JSON | 支持 | 支持 | 支持 |
| Reset Config | 支持 | 支持 | 支持 |
| H.264 Subscribe/Unsubscribe | 支持 | 支持 | 支持 |
| H.264 Packet | 支持 | 只读 Buffer Protocol | 只读 BufferView |
| Device Error 回调 | 支持 | 支持 | 支持 |
| Config Changed 回调 | 支持 | 支持 | 支持 |

高层 Device API 使用 SDK 自有类型。Protobuf、JsonCpp 和低层 `KavioClient` 不属于普通 App 的高层接口。需要直接访问低层协议的 C++ 工程可以显式使用 `Kavio::DeviceSdk`。

## 5. 统一枚举

以下枚举值由稳定 C ABI 定义，并由 C++ 编译期断言校验：

- PixelFormat
- RuntimeVideoStatus
- InputMode
- InputChannel
- InputConnectionStatus
- H264PacketFlag
- KeyOption
- MouseOption

Python 和 Lua 的上述公开常量来自 Native Binding 中的 C ABI 常量，不维护另一份独立数值表。

Keyboard KeyCode 例外：其唯一命名来源是 `include/kavio/Keyboard.h` 中的 `kavio::KeyCode`。C ABI 继续以 `int32_t key_code` 传递 USB HID Usage ID；Python/Lua Binding 在构建时直接遍历 `Keyboard.h` 的 KeyCode 描述表导出 `KeyCode`，Proto 只传输整数，不再维护 `KC_*` 枚举。

## 6. Widget API

当前跨语言公共 Widget 集：

- Page
- Column
- Row
- Label
- Button
- Image
- Slider
- Switch
- Style
- State
- LvglBackend

Widget 是 retained object tree。普通 Widget App 使用公共对象模型；C++ 如需 LVGL Canvas、特殊 Draw Event 或尚未抽象的复杂专用控件，可使用 `LvglBackend::Display()` / `Screen()` / `MakeDefault()` 作为 Native LVGL 扩展点。

Python 和 Lua 不暴露 `lv_obj_t*` 或原始 LVGL C API。脚本语言的复杂专用界面使用 FrameBuffer Buffer Protocol / BufferView 接入原生扩展或第三方 Renderer；如果未来要求 Python/Lua 直接操作 LVGL C API，应先增加专门的 LVGL Binding，而不是绕过当前稳定 C ABI。

## 7. 错误与线程语义

- C++：返回 `kavio::Status` 或在 App `last_status()` 中保留运行结果。
- C ABI：返回 `KavioStatusCode`，当前线程的诊断文本通过 `kavio_last_error_message()` 获取。
- Python：失败映射为 `kavio.Error`；Python callback 异常触发安全停止，并从 `App.run()` 重新抛出。
- Lua：失败转换为 Lua error；callback error 触发安全停止，并从 App run 调用返回错误。
- Presentation、Device 和 Widget 业务回调统一运行在 App run thread。
- FrameLease 必须在获取线程完成 Present/Discard/Destroy。

## 8. Runtime 与依赖边界

| 依赖 | 用途 |
| --- | --- |
| `app_runtime` | App、Presentation、Device、C ABI |
| `ui_core` | Widget model |
| `ui_lvgl` | LVGL Widget Backend |
| `python_binding` | Python Core Binding |
| `python_ui_binding` | Python Widget Binding |
| `lua_runtime` | Lua 5.4 Runtime + Runner |
| `lua_binding` | Lua Core Binding |
| `lua_ui_binding` | Lua Widget Binding |

FrameBuffer-only App 不需要 UI 依赖。Widget App 必须同时声明对应的 UI Core、LVGL Backend 和语言 UI Binding。

## 9. ABI

稳定跨语言 ABI：

```text
KAVIO_ABI_VERSION = 1
```

C ABI 使用 opaque handle、固定宽度整数和带 `struct_size` / `abi_version` 的可扩展结构体，不暴露 STL、C++ 对象布局、Protobuf、JsonCpp 或 LVGL 类型。
