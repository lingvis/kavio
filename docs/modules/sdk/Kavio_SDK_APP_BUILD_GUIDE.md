# Kavio SDK 与外部开发工程编译说明

> 适用目录：`kavio_sdk`、`kavio_app`  
> 适用对象：Device Component（设备组件）（应用程序）开发者、Host Component（主机组件）开发者、应用包集成人员

## 1. 目录结构与边界

仓库顶层目录按“框架、公共接口、业务项目”划分：

```text
project_root/
├── kavio_platform/          # 设备端平台框架，Kavio 官方维护
├── kavio_win/               # Windows 上位机框架，Kavio 官方维护
├── kavio_sdk/               # Host / Device 分离的公开 SDK
├── kavio_app/               # Kavio App Demo、业务项目和打包工具
└── docs/                    # 架构、协议、构建和打包文档
```

`kavio_sdk` 按运行位置拆分：

- `host/include/kavio/`：Windows Host Component 的稳定 C ABI；
- `device/include/kavio/`：Device C++ 公共 API；
- `device/py/`：Device Python SDK；
- `device/proto/`：Device SDK 协议唯一源文件；
- `device/src/internal/`：SDK 私有实现，业务 App 不允许直接依赖。

`kavio_app` 按业务项目组织。一个一级子目录代表一个完整应用包，而不是只代表一段设备端代码或一个 Host Component DLL（动态链接库）。

```text
kavio_app/<project_name>/
├── README.md                # 项目功能、数据协议和验证方法
├── device/                  # 必选：Device Component（设备组件） 源码和 CMake 构建入口
│   ├── build.sh
│   └── CMakeLists.txt
├── host/                    # 可选：Windows Host Component 源码和 VS 工程
│   ├── *.sln
│   └── *.vcxproj
├── package/                 # 必选：清单和静态打包资源
│   ├── kavio_package.json
│   ├── schema_description.json # 必需；无配置项时为 {}
│   └── icon.png             # 图标（可附其他静态资源）
├── output/                  # 构建产物，不提交版本库
│   ├── device/
│   └── host/
└── dist/                    # 最终安装包，不提交版本库
```

### 1.1 设计初衷

采用此结构的原因如下：

1. **业务内聚**：同一个 `appId（应用标识）` 对应的Device Component（设备组件）、Host Component、配置和打包清单放在一起。
2. **目录边界稳定**：Device Component、Host Component、配置、静态资源和打包清单始终位于同一个 App 一级目录，避免双端代码与发行定义分散。
3. **包即项目**：项目目录天然对应安装、升级、卸载和版本管理边界。
4. **构建工具解耦**：Linux 设备端继续使用 CMake（构建系统），Windows Host Component（主机组件）可以使用 Visual Studio 2015，但两端产物分别进入同一项目的 `output/device/` 和 `output/host/`。
5. **SDK 单一来源**：Platform、KavioWin、Device Component（设备组件） 和 Host Component 都从同级 `kavio_sdk` 获取公共定义，禁止复制 SDK 头文件到业务项目。
6. **示例可直接复制**：外部开发者复制一个完整项目目录即可开始开发，不需要再从三个顶层目录拼接样例。

---

## 2. 编译环境要求

### 2.1 Device Component（设备组件）

最低要求：

- Bash（命令行解释器）；
- CMake（构建系统）3.16 或更高版本；
- 支持 C++17（C++ 2017 标准）的 GCC（GNU 编译器）或 Clang（编译器）；
- GNU Make（构建工具）或其他 CMake 构建后端；
- `nproc`、`find`、`sed` 等常用 GNU（GNU 工具集）命令；
- pthread（POSIX 线程库）、`rt`（实时扩展库）、`dl`（动态加载库）等 Linux 系统库。

检查工具：

```bash
cmake --version
g++ --version
bash --version
python3 --version
```

首次使用前：

```bash
chmod +x kavio_sdk/device/build.sh
chmod +x kavio_app/*/device/build.sh
chmod +x kavio_app/kavio_app.py
```

### 2.2 Host Component

当前示例工程面向：

- Visual Studio 2015；
- Platform Toolset（平台工具集）`v140`；
- x64（64 位 Windows）；
- Windows SDK 8.1。

Host Component 业务代码只需要包含：

```text
kavio_sdk/host/include/kavio/KavioHost.h
```

Host Component 只依赖公开的 `KavioHost.h`；SDK 版本不再进入 App ABI。仍建议业务工程从仓库中的 `kavio_sdk/host/include` 引用该头文件，不要维护私有副本。

Host Component 不维护独立 Host Component API 版本，也不做 SDK 版本校验。`KavioAppHost` 仅通过 `struct_size` 检查公开 C ABI 结构边界。业务项目不得直接包含 `kavio_win` 私有头文件。

---

## 3. 第三方依赖目录

### 3.1 Protobuf（协议缓冲区）

SDK 构建依赖：

```text
kavio_sdk/device/3rdParty/protobuf-3.21.12/
```

该目录必须包含 `CMakeLists.txt`。此外还需要 `3rdParty/jsoncpp-1.9.5/` 源码，
用于 App Configuration 的 JSON 校验与持久化。SDK 构建会自动：

1. 编译 Protobuf 静态库；
2. 编译 `protoc（协议代码生成器）`；
3. 扫描 `kavio_sdk/device/proto/*.proto`；
4. 生成 C++ 协议代码；
5. 生成 Python 协议绑定（含 `app_manifest_pb2.py`）和 SDK 版本文件；
6. 将协议代码编译进 SDK。

`*_pb2.py` 与 `sdk_version.py` 均由构建生成，不应作为源码手动维护或提交。

### 3.2 App 自有第三方库

Kavio SDK 不提供 OpenCV、TBB 等通用 App 开发库。SDK 的 `device/3rdParty` 只保存 Device SDK 自身构建所必需的依赖。

`demo_rx_tx` 直接绘制 BGRA 像素，不依赖 OpenCV/TBB。新 App 如果需要 OpenCV，应在自己的项目目录或项目依赖管理中声明，不得把 OpenCV 作为 Kavio SDK 公共库使用。

---

## 4. 单独编译 SDK

进入 SDK 目录：

```bash
cd kavio_sdk/device
```

Debug（调试）模式：

```bash
./build.sh
```

Release（发布）模式：

```bash
./build.sh -r
```

清理：

```bash
./build.sh -c
```

主要输出：

```text
kavio_sdk/device/build/lib/libkavio_sdk.a
kavio_sdk/device/build/sdk_proto_src/
kavio_sdk/device/py/kavio_sdk/proto_py/
```

Native C++ App 编译前必须先构建一次 Device SDK。SDK 构建会生成 `KavioDeviceSdkConfig.cmake`，后续多个 App 直接链接这套已构建静态库，不再把 SDK 源码加入各自构建目录。

---

## 5. 构建设备端项目

每个 Kavio App 的 `device/` 都是一个独立 CMake（构建系统）工程，并拥有自己的构建脚本。根目录 `kavio_app.py` 只负责发现、检查和打包 App，不负责编译 Device Component。

### 5.1 ARM 板端本机编译

以 `demo_communication` 为例：

```bash
cd kavio_app/demo_communication/device
./build.sh
```

Release（发布）模式：

```bash
./build.sh -r
```

### 5.2 x86 Linux 交叉编译

```bash
./build.sh -r --toolchain /absolute/path/to/aarch64-toolchain.cmake
```

脚本不会猜测工具链。未传工具链时，CMake 使用当前主机默认编译器。

### 5.3 构建输出

项目本地构建目录：

```text
kavio_app/<project_name>/device/build/
```

构建成功后，脚本将设备端主程序复制到：

```text
kavio_app/<project_name>/output/device/
```

`build.sh` 只负责当前项目的设备端构建和产物暂存，不处理安装包目标系统、处理器架构或硬件平台匹配。

### 5.4 清理

```bash
./build.sh -c
```

该命令删除当前项目的 `device/build/` 和该项目已经暂存的设备端主程序，不影响其他项目。

---

## 6. 构建 Host Component

Windows Host Component 的解决方案和工程文件全部位于项目的 `host/`。

以 `demo_communication` 为例：

```text
kavio_app/demo_communication/host/DemoAppCommunication.sln
```

请使用 **Visual Studio 2015 或更高版本**打开该 `.sln`，选择 `x64` 和需要的 `Debug/Release` 配置后直接编译。当前示例工程使用 `v140` 工具集；如果更高版本 Visual Studio 未安装该工具集，请按 IDE 提示安装对应 C++ 工具集或显式重定向工程。

PostBuildEvent（构建后事件）统一将主应用命名为 `host.dll`，并复制到：

```text
kavio_app/<project_name>/output/host/host.dll
```

应用依赖的其他 DLL 也应复制到同一平台目录。

---

## 7. 项目检查与打包

`kavio_app.py` 不负责编译 ARM App 或 Host Component DLL。它负责：

- 发现符合规则的业务项目；
- 读取 `package/kavio_package.json`；
- 从 `package/` 检查配置和图标；
- 从 `output/` 检查 Device Component 和 Host Component；
- 临时合并两类内容；
- 调用低级打包器生成 `.kap`。

### 7.1 列出项目

```bash
cd kavio_app
python3 kavio_app.py list
```

### 7.2 检查产物

```bash
python3 kavio_app.py status demo_communication
```

### 7.3 生成安装包

```bash
python3 kavio_app.py package demo_communication
```

默认输出：

```text
kavio_app/demo_communication/dist/demo_communication-1.0.0.kap
```

指定输出路径：

```bash
python3 kavio_app.py package demo_communication \
  --output /tmp/demo_communication.kap
```

`tools/build_package.py` 是低级打包器。它要求调用方已经准备好包含清单和全部运行文件的完整暂存目录；普通开发流程不应直接用项目的 `package/` 调用它。

---

## 8. 新建一个完整业务项目

推荐结构：

```text
kavio_app/my_project/
├── README.md
├── device/
│   ├── build.sh
│   ├── CMakeLists.txt
│   └── main.cpp
├── host/                    # 没有 Host Component 时省略
├── package/
│   └── kavio_package.json
├── output/                 # 自动生成
└── dist/                   # 自动生成
```

设备端 `CMakeLists.txt` 必须是可独立配置的工程。关键路径计算方式可参考两个现有 Demo：

```cmake
get_filename_component(KAVIO_PROJECT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(KAVIO_REPOSITORY_ROOT "${KAVIO_PROJECT_DIR}/../.." ABSOLUTE)
set(KAVIO_DEVICE_SDK_DIR "${KAVIO_REPOSITORY_ROOT}/kavio_sdk/device/build")

find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH)
```

关键要求：

1. 可执行文件名必须与 `package/kavio_package.json` 的 `device.entry` 一致；
2. `device/build.sh` 中的 `TARGET_NAME` 必须与 CMake 目标一致；
3. 业务项目只能引用 `kavio_sdk` 公开接口，并通过 `Kavio::DeviceSdk` 链接预构建 SDK；
4. Windows 工程只能放在 `host/`；
5. 编译产物只能进入 `output/`，不能写入 `package/`；
6. 同一个 `appId` 的双端代码、配置和清单必须留在同一个一级项目中。

---

## 9. Device SDK 第三方依赖边界

`kavio_sdk/device/3rdParty` 只承载 SDK 自身必需依赖。业务 App 的图像、AI、网络等第三方库由 App 自己管理。`demo_rx_tx` 不依赖 OpenCV，可作为无额外图像库的 RX/TX 示例。

---

## 10. 当前示例

### 10.1 demo_communication（系统资源监控）

设备端：

```bash
cd kavio_app/demo_communication/device
./build.sh -r
```

Windows Host Component（主机组件）：使用 Visual Studio 2015 或更高版本打开 `host/DemoAppCommunication.sln`，选择 `x64` 和需要的 `Debug/Release` 配置后编译。

检查和打包：

```bash
cd kavio_app
python3 kavio_app.py status demo_communication
python3 kavio_app.py package demo_communication
```

### 10.2 demo_rx_tx（收发共享内存示例）

```bash
cd kavio_app/demo_rx_tx/device
./build.sh -r
cd ../..
python3 kavio_app.py status demo_rx_tx
python3 kavio_app.py package demo_rx_tx
```

---

## 11. 常见问题

### 11.1 找不到 kavio_sdk

必须保持：

```text
project_root/kavio_sdk/
project_root/kavio_app/
```

业务项目的 `device/CMakeLists.txt` 根据固定的仓库相对关系定位 SDK。

### 11.2 构建出了 x86 程序

说明在 x86 Linux 上未传交叉编译工具链。使用：

```bash
./build.sh --toolchain /path/to/aarch64-toolchain.cmake
```

`build.sh` 会在编译成功后将目标程序复制到 `output/device/`。

### 11.3 status 显示设备入口缺失

检查：

- 是否运行了当前项目的 `device/build.sh`；
- 输出文件名是否与 `device.entry` 一致；
- 文件是否位于 `output/<device.directory>/`。

### 11.4 status 显示 host.dll 缺失

检查：

- 是否使用与当前 KavioWin 工程一致的 Visual Studio 配置构建；
- PostBuildEvent 是否成功；
- `KavioOutputRoot` 是否指向 `..\output`；
- 文件是否位于 `output/<host.directory>/host.dll`。

### 11.5 package 中是否可以放二进制

不应放。`package/` 是版本管理中的清单和静态资源目录；二进制、模型、运行依赖等生成或部署产物统一放入 `output/` 对应平台目录。

### 11.6 Platform 和 KavioWin 是否也应迁入 kavio_app

不应迁移。二者是 Kavio 运行框架，不属于任何单个 `appId`。

---

## 12. 推荐命令汇总

```bash
# 单独验证 SDK
cd kavio_sdk/device
./build.sh -r

# 构建Device Component（设备组件）
cd ../../kavio_app/demo_communication/device
./build.sh -r

# 检查和打包
cd ../..
python3 kavio_app.py status demo_communication
python3 kavio_app.py package demo_communication
```

Windows：使用 Visual Studio 2015 或更高版本打开 `kavio_app/demo_communication/host/DemoAppCommunication.sln`，选择 `x64` 和需要的 `Debug/Release` 配置后编译。
