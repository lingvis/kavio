# Kavio Device SDK 总体架构设计

## 1. 目标

Kavio Device SDK 为运行在 Kavio Device 上的 App 提供统一的生命周期、设备能力、AppView Presentation、Widget 和多语言 Binding。

正式支持：

- C++17；
- Python 3.10+；
- Lua 5.4。

三种语言共享同一套 C++ Runtime 和稳定 C ABI。语言层只负责对象封装、回调转换和脚本运行，不各自实现 IPC、Session、AppView 或设备协议。

核心原则：

1. 每个 KavioApp 都有 AppView Presentation；
2. LVGL 是可选 Widget Backend，不是 AppRuntime 的强制依赖；
3. Direct FrameBuffer 与 Widget 是同级呈现路线；
4. Device 能力由一个 Runtime 管理的 `KavioClient` 统一提供；
5. Python/Lua 只通过稳定 C ABI 使用 Runtime；
6. 每个可构建 App 由单一 `kavio_project.json` 自描述；语言和公开 SDK 模块决定 Builder/Binding 依赖，安装运行语义保留在 `package` 字段；
7. 仓库根目录 `kavio` 是唯一正式工程入口。

---

## 2. 总体分层

```text
C++ API ───────────────────────┐
Python kavio ── Native ─ C ABI ├── libkavio_app_runtime.so
Lua kavio ───── Native ─ C ABI ┘       │
                                        ├── AppRuntime
                                        ├── DeviceRuntime ── KavioClient
                                        ├── EventRuntime
                                        └── PresentationRuntime
                                                │
                               ┌────────────────┴───────────────┐
                               │                                │
                        Direct FrameBuffer                Widget Model
                                                               │
                                                        LVGL Backend
```

`libkavio_app_runtime.so` 不依赖 LVGL。Widget Core 和 LVGL Backend 分别位于独立共享库中。

---

## 3. AppRuntime

`kavio::App` 是高层 C++ App 入口。Runtime 统一管理：

- Device Session；
- AppView Surface；
- Event queue；
- Presentation；
- Device；
- Ready / Tick / Stopping 生命周期；
- Stop 和资源释放。

生命周期：

```text
Created
  ↓
Starting
  ↓
Session Ready
  ↓
Presentation Ready
  ↓
Running
  ↓
Stopping
  ↓
Stopped
```

所有高层业务回调运行在调用 `App::run()` 的 App thread。

IPC Receive / Dispatch Thread 不直接进入：

- App callback；
- Python VM；
- Lua VM；
- LVGL Widget mutation。

---

## 4. DeviceRuntime

一个高层 App 只创建一个 `KavioClient`。

高层 `kavio::Device` 提供 SDK 自有类型，不向普通 App 暴露 Protobuf、JsonCpp 或低层 SHM 实现。

能力包括：

- Runtime Data；
- RX 状态和参数；
- RX Frame；
- TX 参数和 `DeviceTxSurface`；
- Keyboard / Mouse；
- `Keyboard.h` 是 USB HID Keyboard/Keypad KeyCode 的公共唯一命名来源，Proto 仅传输数值；
- USB App Channel；
- App Configuration JSON；
- H.264 Subscribe / Packet；
- Device Error、Config Changed、H.264 Invalidated 等异步事件。

低层 `KavioClient` 和 `AppViewSurface` 仍作为高级 C++ 接口存在，通过 `Kavio::DeviceSdk` 显式使用。

---

## 5. PresentationRuntime

Presentation 是所有 KavioApp 的显示入口。

当前 AppView：

- BGRA8888；
- 共享内存；
- width / height / stride 由 Surface Descriptor 决定；
- 单个持续共享画布；
- `BeginWrite` / `Present` 同步；
- 支持 Dirty Rect；
- `Present` 表示发布 Frame Ready，不表示 VSync 或显示完成。

Presentation 同时负责：

- Surface 生命周期；
- FrameLease；
- Raw Touch；
- Visibility；
- Backend 内部输入观察者。

App 自己的 Raw Touch/Visibility Handler 与 Widget Backend 的输入适配可以同时存在。

---

## 6. FrameBuffer 路线

`FrameLease` 是一次 AppView 写事务：

```text
Acquire
  ↓
Writable
  ├── Present
  └── Discard
```

规则：

- 获取、写入、Present/Discard/Destroy 必须在同一线程；
- 使用 SDK 返回的 stride；
- `Discard` 只释放写事务，不发布画面；
- FrameLease 不代表独立双缓冲；
- 当前不承诺 DMA-BUF、GPU Texture Import 或 VSync Fence。

自定义 Renderer 应优先直接写 SDK 提供的 FrameBuffer，避免额外完整画布复制。

Python 使用可写 Buffer Protocol；Lua 使用受 Lease 生命周期约束的 BufferView。

---

## 7. Widget 与 LVGL Backend

公共 Widget Model：

- Page；
- Column；
- Row；
- Label；
- Button；
- Image；
- Slider；
- Switch；
- Style；
- State。

Widget Model 本身不依赖 LVGL。

`libkavio_ui_lvgl.so` 将 Widget tree 映射到同一 Presentation，并保留局部刷新：

```text
LVGL Partial Render
  ↓
64-line draw buffer
  ↓
FrameLease
  ↓
Dirty Rect Present
```

C++ 可以通过 `LvglBackend::Display()`、`Screen()`、`MakeDefault()` 使用同一份 LVGL Runtime 创建复杂专用控件。

Python/Lua 当前公共 SDK 不暴露 raw `lv_obj_t*`；复杂脚本界面使用 FrameBuffer、自定义 Native Extension 或第三方 Renderer。

---

## 8. 稳定 C ABI

公共 C ABI：

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

约束：

- opaque handle；
- 固定宽度整数；
- 可扩展结构体使用 `struct_size` / `abi_version`；
- UTF-8 字符串；
- 不跨 ABI 传递 STL、C++ exception、Protobuf、JsonCpp、LVGL 对象；
- `KAVIO_ABI_VERSION` 独立于 SDK Version。

C++ 高层枚举使用编译期断言与 C ABI 数值保持一致。Python/Lua 从 Native Binding 获取同一数值来源。

---

## 9. Python Binding

目录：

```text
bindings/python/
├── native/
│   ├── kavio_native.cpp
│   └── kavio_ui_native.cpp
└── package/kavio/
```

产物：

```text
kavio/_native.so
kavio/_ui_native.so    # Widget App 按需部署
```

规则：

- Core Binding 只依赖 AppRuntime C ABI；
- UI Binding 只依赖 Widget/LVGL C ABI；
- `App.run()` 等待期间释放 GIL；
- 进入 Python callback 时获取 GIL；
- callback exception 请求 Runtime 安全停止，并从 `App.run()` 重新抛出；
- FrameLease 使用可写 Buffer Protocol；
- RX Frame / H.264 Packet 使用只读 Buffer Protocol；
- Python App 日志使用 `kavio.app_logger`。

不维护独立 Python IPC、SHM 或 Protobuf 协议实现。

---

## 10. Lua Binding

目录：

```text
bindings/lua/
├── native/
└── package/kavio/
```

产物：

```text
kavio_lua_runner
libkavio_lua.so
kavio/native.so
kavio/ui_native.so      # Widget App 按需部署
```

Lua App 的语言在 `kavio_project.json` 中声明；设备 Package 使用 native `kavio_lua_runner` 作为入口，脚本路径保存于 `package.device.arguments`。

Lua callback 只在 App thread 进入 VM。FrameBuffer 使用 BufferView；RX/H.264 使用只读 BufferView。

---

## 11. 构建产物

核心：

```text
libkavio_app_runtime.so
libkavio_ui_core.so
libkavio_ui_lvgl.so     # optional
```

低层高级 C++：

```text
Kavio::DeviceSdk
```

高层 C++：

```text
Kavio::App
Kavio::UI::Core
Kavio::UI::LVGL
```

Python：

```text
kavio/_native.so
kavio/_ui_native.so
```

Lua：

```text
kavio_lua_runner
libkavio_lua.so
kavio/native.so
kavio/ui_native.so
```

---

## 12. App 工程与模板

### 12.1 可构建工程

每个真实 App 工程目录必须包含：

```text
kavio_project.json
```

根工具只递归扫描：

```text
kavio_app/**/kavio_project.json
```

工程 Key 为该目录相对 `kavio_app/` 的路径。目录名称不参与工程识别。

`kavio_project.json` 维护开发者真正需要控制的语义：

- `language`：C++ / Python / Lua；
- `sdk.modules`：当前唯一可选公共模块 `ui`；
- `package`：最终安装/运行元数据；
- `build`：只有无法可靠推导的构建输入，例如 KavioDemo 的 PC Host artifact 路径。

Builder、基础 Runtime/语言 Binding、UI 内部依赖由 `language + sdk.modules` 自动推导。

正式 Schema：

```text
kavio_app/tools/kavio_project.schema.json
```

### 12.2 源模板

新项目模板位于：

```text
kavio_app/Templates/
```

模板使用：

```text
kavio_project.json.template
```

因此不会被工程扫描器识别。`./kavio new` 负责复制模板、替换 App ID/name/Device Entry 等 token，并将模板描述文件转换成真正的 `kavio_project.json`。

当前正式模板：

```text
cpp/framebuffer
cpp/kavio_widget
cpp/advanced_ui
python/framebuffer
python/kavio_widget
lua/framebuffer
lua/kavio_widget
```

`advanced_ui` 只存在于 C++，因为当前 Python/Lua 公共 Binding 不暴露 raw LVGL C API。

## 13. 构建入口

仓库只提供一个正式入口：

```bash
./kavio list
./kavio new <project> --template <language/route>
./kavio build sdk [--release]
./kavio build <project-key> [--release]
./kavio pack <project-key> [--release]
./kavio clean <sdk|all|project-key>
```

`build` 只构建/暂存设备端。

Windows Host DLL 在 PC 上独立构建；声明 Host Component 的 App 在 `pack` 阶段检查 `build/apps/<工程相对路径>/host/` 下的指定 DLL。

---

## 14. Package 与 RPATH

标准设备包：

```text
device/
├── <entry>
├── kavio/                  # Python/Lua Binding，按语言存在
└── lib/
    ├── libkavio_app_runtime.so
    ├── libkavio_ui_core.so       # optional
    ├── libkavio_ui_lvgl.so       # optional
    └── libkavio_lua.so           # Lua only
```

RPATH：

- Native App：`$ORIGIN/lib`；
- UI shared library：`$ORIGIN`；
- Python Native Module：`$ORIGIN/../lib`；
- Lua Runner：`$ORIGIN/lib`；
- Lua Native Module：`$ORIGIN/../lib`。

Package 工具直接生成 `manifest.json`，字段遵循 `app_manifest.proto` 的 protobuf JSON 命名和标量表示规则，不在源码树维护生成的 Python protobuf 文件。

---

## 15. Templates 与 KavioDemo

正式开发入口分为两类：

### Templates

```text
kavio_app/Templates/
```

用于 `./kavio new` 创建业务工程，不作为可编译 App 被扫描。

- `framebuffer`：应用自有 Renderer，直接使用 FrameLease/Buffer；
- `kavio_widget`：大多数业务 App 的默认高层 UI 路线；
- `advanced_ui`：仅 C++，复用 SDK LvglBackend 后直接使用 raw LVGL specialized controls。

### KavioDemo

```text
kavio_app/{KavioDemoCpp,KavioDemoPython,KavioDemoLua}
```

三个真实可编译工程，用于综合能力展示、Host 联调与 SDK 回归验证，不作为新业务 App 的复制起点。

## 16. 线程与资源规则

1. App callback 只在 App thread；
2. IPC thread 不执行业务逻辑；
3. LVGL 只由 App/UI thread 操作；
4. FrameLease 不跨线程提交；
5. Python 有活动 writable memoryview 时不能提交 Frame；
6. Lua 有活动 writable BufferView 时不能提交 Frame；
7. Surface 销毁后旧 Lease 失效；
8. Device Runtime 只拥有一个 `KavioClient`；
9. Widget Backend 不拥有 Session 或 AppView；
10. Host 构建与 Device 构建分离。

---

## 17. 公开边界

普通第三方 App 应优先使用：

```text
Kavio::App
kavio::Device
kavio::Presentation
kavio::ui
Python kavio
Lua kavio
```

只有需要直接控制低层 IPC/SHM 的高级 C++ 工程才显式使用：

```text
Kavio::DeviceSdk
kavio/KavioClient.h
kavio/AppViewSurface.h
```

SDK 的 `src/`、`src/internal/`、生成 Protobuf 代码和 Runtime 私有桥接头文件都不属于第三方公开 API。
