# Kavio Device App 工程、构建与打包指南

本文描述 Kavio Device App 当前正式的工程识别、项目文件、创建、构建和打包规则。

## 1. 唯一正式入口

仓库根目录只有一个面向 App 开发者的工程工具：

```bash
./kavio
```

常用命令：

```bash
./kavio list
./kavio new MyApp --template cpp/kavio_widget
./kavio build sdk --release

### Project-scoped SDK build

`./kavio build <project>` configures only the SDK language/UI components required by that project's `kavio_project.json`. A C++ project does not require Python or Lua development packages, and a framebuffer-only project does not require the LVGL backend.

`./kavio build sdk` builds the complete SDK, including all enabled language bindings and UI backends, so the corresponding development dependencies must be available.
./kavio build MyApp --release
./kavio pack MyApp --release
./kavio clean MyApp
```

命令语义：

| 命令 | 作用 |
| --- | --- |
| `list` | 扫描并列出所有 Kavio 工程 |
| `new` | 从正式 Demo 模板创建新工程 |
| `build` | 构建 Device SDK 或指定 Device App |
| `pack` | 使用已经构建的 Device 产物生成 `.kap` |
| `clean` | 清理 SDK、单个工程或全部构建产物 |

设备端工具不负责 Windows Host DLL 编译，也不调用 MSBuild。

---

## 2. 工程与模板如何被识别

### 2.1 真实工程

Kavio 可构建工程的唯一识别标志是：

```text
kavio_project.json
```

构建工具递归扫描：

```text
kavio_app/**/kavio_project.json
```

扫描到该文件的目录才是可构建工程。工程 Key 是目录相对于 `kavio_app/` 的路径。

仓库内 KavioDemo 示例：

```text
kavio_app/
├── KavioDemoCpp/kavio_project.json
├── KavioDemoPython/kavio_project.json
└── KavioDemoLua/kavio_project.json
```

对应：

```text
KavioDemoCpp
KavioDemoPython
KavioDemoLua
```

```bash
./kavio list
```

只列真实工程。

### 2.2 源模板

模板位于：

```text
kavio_app/Templates/
```

模板使用：

```text
kavio_project.json.template
```

所以不会被工程扫描器命中，也不能直接 `build/pack`。

```bash
./kavio new --list
```

使用独立模板扫描器列出模板。`./kavio new` 实例化模板并创建真正的 `kavio_project.json`。

---

## 3. 推荐工程结构

```text
MyApp/
├── kavio_project.json
├── icon.png
├── schema_description.json
├── README.md
└── device/
    └── ...
```

固定文件：

| 文件 | 用途 |
| --- | --- |
| `kavio_project.json` | 工程、构建和 Package 元数据的唯一源文件 |
| `icon.png` | App 图标 |
| `schema_description.json` | App Configuration 定义；无配置时为 `{}` |
| `device/` | Device Component 源码 |

工程元数据、图标和配置定义都位于工程根目录。

---

## 4. `kavio_project.json`

一个普通 C++ Widget App 可以非常简单：

```json
{
  "projectVersion": 1,
  "language": "cpp",
  "sdk": {
    "modules": ["ui"]
  },
  "package": {
    "appId": "com.example.my-app",
    "version": "1.0.0",
    "name": "My App",
    "description": "My Kavio App.",
    "device": {
      "runtime": "native",
      "entry": "kavio_my_app",
      "arguments": []
    }
  }
}
```

正式 Schema：

```text
kavio_app/tools/kavio_project.schema.json
```

### 4.1 为什么项目文件里没有 `$schema`

`$schema` 常被编辑器用于把一个 JSON 文件关联到 JSON Schema，从而提供：

- 字段自动补全；
- 类型提示；
- 未知字段检查；
- 即时格式校验。

如果每个工程都写：

```json
"$schema": "../../../tools/kavio_project.schema.json"
```

那么工程移动一层目录就可能使相对路径失效。

因此当前设计不要求项目文件携带 `$schema`。Kavio 工具固定使用：

```text
kavio_app/tools/kavio_project.schema.json
```

作为机器可读规范，并在扫描/构建时执行等价的强校验。

Schema 文件本身仍会出现：

```json
"$schema": "https://json-schema.org/draft/2020-12/schema"
```

这不是工程路径，而是在声明 **Schema 文件自身采用 JSON Schema Draft 2020-12 语法**。

---

## 5. 哪些字段保留，哪些字段自动推导

项目文件的目标不是“把所有内部信息都写出来”，而是只让开发者维护真正属于工程语义的信息。

### 5.1 明确保留的字段

#### `language`

```json
"language": "cpp"
```

可选：

```text
cpp
python
lua
```

它描述开发语言。

#### `package.device.runtime`

即使 Runtime 可以部分从语言猜测，仍然显式保留：

```json
"runtime": "native"
```

原因是它属于最终安装包的运行语义，会进入 Manifest。

例如：

- C++：`native`；
- Python：`python`；
- Lua：当前使用 SDK `kavio_lua_runner`，所以安装层仍为 `native`。

如果只通过 `language` 推导 Runtime，会把“开发语言”和“设备启动机制”错误地绑成一个概念。

#### `package.device.entry`

这是最终设备运行入口，也属于 Package Manifest，不应隐藏。

#### `package.device.arguments`

运行参数会进入安装清单，因此显式保存。

#### `package.host`

Host Component 是否存在、包内文件名和 Panel 属性属于安装包信息，显式保存。

### 5.2 自动推导的字段

#### Device Builder

不再要求开发者填写：

```text
builder=cmake/python/lua
```

工具根据语言确定：

| language | builder |
| --- | --- |
| `cpp` | CMake |
| `python` | Python staging |
| `lua` | Lua staging + syntax check |

这是确定性关系，没有必要重复配置。

#### 基础 Runtime / Binding 依赖

开发者不再写：

```text
app_runtime
python_binding
lua_runtime
lua_binding
...
```

工具自动推导。

基础依赖：

| language | 自动依赖 |
| --- | --- |
| C++ | App Runtime |
| Python | App Runtime + Python Core Binding |
| Lua | App Runtime + Lua Runtime + Lua Core Binding |

如果工程声明：

```json
"sdk": {
  "modules": ["ui"]
}
```

再自动增加：

```text
UI Core
LVGL Backend
对应语言 UI Binding（Python/Lua）
```

这样第三方开发者不需要知道 SDK 内部 `.so` 的名字和 Binding 闭包。

#### C++ CMake Target

默认：

```text
build.target = package.device.entry
```

大多数工程无需填写 `build.target`。

只有 CMake Target 名称确实与最终设备入口不同，才显式配置：

```json
"build": {
  "target": "my_internal_target"
}
```

#### Device 源码目录

默认：

```text
device/
```

只有工程采用其他布局时才写：

```json
"build": {
  "source": "src/device"
}
```

#### Lua 脚本入口

Lua 安装入口仍是：

```json
"entry": "kavio_lua_runner"
```

Lua 脚本入口直接使用：

```json
"arguments": ["main.lua"]
```

不再重复维护单独 `scriptEntry`。

---

## 6. `package` 字段为什么保留为整体

`kavio_project.json.package` 设计成与最终 Package 元数据高度一致：

```json
"package": {
  "appId": "...",
  "version": "...",
  "name": "...",
  "description": "...",
  "device": {
    "runtime": "...",
    "entry": "...",
    "arguments": []
  },
  "host": {
    "binary": "host.dll",
    "panel": true
  }
}
```

打包阶段可以把这个对象作为 Manifest 的元数据基础，而不是根据 `language`、CMake Target 等开发信息逐字段重新猜测。

构建工具只额外生成：

- Manifest 协议版本；
- Package 文件列表；
- 文件大小；
- SHA-256；
- 配置存在状态等由打包器计算的派生信息。

---

## 7. `.kap` 中保存什么

`kavio_project.json` 是开发工程文件，**不会进入 `.kap`**。

最终包保存的是运行时需要的信息：

```text
manifest.json
icon.png
schema_description.json
device/
host/               # optional
signature.json       # signing enabled 时
```

`manifest.json` 保存：

- App ID；
- version；
- display name；
- description；
- Device runtime / entry / arguments；
- Python Runtime 配置（Python App）；
- Host Component 信息（存在时）；
- 文件路径、大小和 SHA-256。

不会保存：

- CMake Target；
- Device 源码目录；
- `sdk.modules`；
- 构建机器路径；
- `kavio_project.json`；
- 构建命令。

---

## 8. SDK 模块

当前公开可选模块：

```json
"sdk": {
  "modules": ["ui"]
}
```

### 不声明 `ui`

用于：

- Direct FrameBuffer；
- 自有 Renderer；
- 不需要 Kavio Widget/LVGL 的 App。

### 声明 `ui`

用于：

- Kavio Widget；
- SDK LVGL Backend；
- C++ Native LVGL extension。

这里声明的是**公开 SDK 能力**，不是内部 Shared Library 列表。

---

## 9. Host Component

如果 Package 有 Host：

```json
"package": {
  "host": {
    "binary": "host.dll",
    "panel": true
  }
}
```

Host 构建产物统一放到 `build/apps/<工程相对路径>/host/`，例如 `build/apps/KavioDemoCpp/host/host.dll`。该目录由构建工具创建，直接复制 Windows 编译生成的整个 `host/` 目录即可；其余依赖 DLL、资源子目录会一并打包。无需单独配置 Host 搜索路径。

设备侧：

```bash
./kavio build KavioDemoCpp
```

不检查 Host。

打包：

```bash
./kavio pack KavioDemoCpp
```

如果 `host/` 下缺少 `package.host.binary` 指定的 DLL，直接报错并打印目标路径。

---

## 10. 新建工程

第一步先查看模板：

```bash
./kavio new --list
```

不带工程名执行 `./kavio new` 也会显示模板列表。

普通 UI App 推荐：

```bash
./kavio new MyApp \
  --template cpp/kavio_widget \
  --app-id com.mycompany.my-app
```

Python：

```bash
./kavio new MyApp --template python/kavio_widget --app-id com.mycompany.my-app
```

Lua：

```bash
./kavio new MyApp --template lua/kavio_widget --app-id com.mycompany.my-app
```

生成后：

```text
kavio_app/MyApp/
├── kavio_project.json
├── icon.png
├── schema_description.json
├── README.md
└── device/
```

并立即出现在 `./kavio list`。

`new` 自动完成模板复制、token 替换、App ID/name 设置，以及 C++ Device Entry/CMake Target 唯一化。

## 11. 模板选择

正式模板只有 7 个：

```text
cpp/kavio_widget
cpp/framebuffer
cpp/advanced_ui
python/kavio_widget
python/framebuffer
lua/kavio_widget
lua/framebuffer
```

### `kavio_widget` — 默认推荐

适用于文字、按钮、图片、表单、设置页等普通 UI：

```text
App -> Kavio Widget -> SDK LVGL backend -> Presentation
```

应用代码不需要理解 LVGL Display、draw buffer 或 AppView 创建过程。

### `framebuffer`

适用于游戏、视频、OpenCV、Godot、自研 GUI、已有 Renderer：

```text
App -> Presentation -> FrameLease -> BGRA buffer -> Renderer -> Present
```

不声明 `sdk.modules.ui`。

### `advanced_ui` — 仅 C++

用于 Kavio Widget 无法表达、必须直接调用 raw LVGL Chart/Canvas/custom draw 的 C++ 专用界面。必须复用 SDK `LvglBackend` 已创建的同一个 LVGL runtime，不允许自己 `lv_init()` 或创建第二套 Display/AppView。

Python/Lua 当前不提供该模板，因为公开脚本 Binding 不暴露 raw LVGL C API，其自定义 Renderer 场景已由 `framebuffer` 覆盖。

完整选择说明见：

```text
kavio_app/Templates/README.md
```

---

## 12. 构建 SDK

Debug：

```bash
./kavio build sdk
```

Release：

```bash
./kavio build sdk --release
```

并行度：

```bash
./kavio build sdk --release --jobs 8
```

输出：

```text
build/sdk/debug/
build/sdk/release/
```

---

## 13. 构建 App

先创建真实工程，例如：

```bash
./kavio new MyApp --template cpp/kavio_widget --app-id com.example.my-app
```

然后：

```bash
./kavio build MyApp
./kavio build MyApp --release
```

`build` 自动准备 SDK，读取 `language`/`sdk.modules`，推导 Builder/Runtime/Binding 依赖，只构建或暂存 Device Component；不构建 Windows Host DLL，不生成 `.kap`。

Templates 本身没有 `kavio_project.json`，所以不能直接构建。

---

## 14. 打包 App

先完成相同配置的 Device build：

```bash
./kavio build MyApp --release
```

然后：

```bash
./kavio pack MyApp --release
```

指定输出路径：

```bash
./kavio pack MyApp --release --output /tmp/MyApp.kap
```

默认 `.kap` 输出到 `build/apps/<工程相对路径>/dist/`，工程目录可为任意层级，不强制语言目录。

如果工程声明 Host Component，`pack` 检查 `build/apps/<工程相对路径>/host/` 下的 Host DLL；缺失则失败。`build` 阶段只创建目标目录，不检查 Host DLL。设备额外文件放在同一工程构建根目录的 `device/` 下，保持目录结构打包。

---

## 15. 清理

全部：

```bash
./kavio clean all
```

只清 SDK：

```bash
./kavio clean sdk
```

单个工程：

```bash
./kavio clean MyApp
```

---

## 16. Python 工程

核心字段示例：

```json
{
  "projectVersion": 1,
  "language": "python",
  "sdk": {
    "modules": ["ui"]
  },
  "package": {
    "appId": "com.example.python-app",
    "version": "1.0.0",
    "name": "Python App",
    "description": "Example",
    "device": {
      "runtime": "python",
      "entry": "main.py",
      "arguments": [],
      "python": {
        "pythonVersion": "3.8",
        "installMode": "online",
        "requirements": "requirements.txt"
      }
    }
  }
}
```

构建工具会：

- 暂存 Python 源码；
- 排除 `__pycache__/*.pyc/*.pyo/build/dist`；
- 对全部 `.py` 做语法检查；
- 自动加入 Python Core Binding；
- 声明 UI 时自动加入 Python UI Binding。

---

## 17. Lua 工程

示例：

```json
{
  "projectVersion": 1,
  "language": "lua",
  "package": {
    "appId": "com.example.lua-app",
    "version": "1.0.0",
    "name": "Lua App",
    "description": "Example",
    "device": {
      "runtime": "native",
      "entry": "kavio_lua_runner",
      "arguments": ["main.lua"]
    }
  }
}
```

Lua 的两个概念要分开：

```text
开发语言        = lua
设备进程入口    = kavio_lua_runner
Lua 脚本入口    = package.device.arguments[0]
```

构建工具会使用 SDK Runner 对工程中的全部 `.lua` 文件执行语法检查。

---

## 18. C++ 工程

默认情况下：

```text
CMake Target = package.device.entry
```

因此推荐：

```json
"package": {
  "device": {
    "runtime": "native",
    "entry": "kavio_my_app",
    "arguments": []
  }
}
```

CMake：

```cmake
add_executable(kavio_my_app main.cpp)
```

如果 Target 与最终入口确实不同，可以显式：

```json
"build": {
  "target": "internal_target"
}
```

---

## 19. 图标和配置 Schema

工程根目录固定：

```text
icon.png
schema_description.json
```

图标要求：

- PNG；
- 宽高 1～512；
- 最大 512 KiB。

没有 App Configuration 时：

```json
{}
```

不要删除 `schema_description.json`。

---

## 20. 常见错误

### `./kavio list` 看不到工程

检查：

```text
<project>/kavio_project.json
```

是否存在且有效。

### C++ Target 找不到

默认 Target 与：

```text
package.device.entry
```

相同。检查 CMake `add_executable()` 名称。

### Python 工程打包失败

确认：

```text
package.device.entry
```

对应实际 `.py` 文件，且 `package.device.python.requirements` 文件存在。

### Lua 入口失败

确认：

```text
package.device.entry = kavio_lua_runner
package.device.arguments[0] = main.lua
```

且脚本文件实际存在。

### Host 缺失

先在 Windows PC 构建 Host，将 Host DLL 及其依赖复制到 `build/apps/<工程相对路径>/host/`，再执行 `pack`。

---

## 21. 推荐开发流程

普通 UI App：

```bash
./kavio new MyApp --template cpp/kavio_widget --app-id com.mycompany.my-app
./kavio build MyApp
./kavio pack MyApp
```

已经有自己的 Renderer：

```bash
./kavio new MyRenderer --template cpp/framebuffer --app-id com.mycompany.renderer
```

需要理解 Demo 应选哪一个时，先看：

```text
kavio_app/Templates/README.md
```
