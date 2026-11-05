# Kavio Device SDK 构建依赖

依赖清单的唯一维护位置是 `build_tools/dependencies.json`。本文件说明约束与安装操作；更新固定版本时，必须同时更新清单及 `kavio_sdk/device/CMakeLists.txt` 中对应目录。所有第三方源码路径均相对于仓库，不通过系统全局搜索替代。

## 检查

在项目根目录执行：

```bash
./kavio doctor --all
```

如仅构建 C++ 且不启用 UI，可执行 `./kavio doctor`；其他组合使用 `--python`、`--lua`、`--ui`。每次 `./kavio build` 也会针对该项目自动执行同样的依赖检查。检查器负责提前诊断缺失依赖，CMake 在 configure 阶段再次严格检查实际启用的依赖。

## SDK 自带依赖

| 依赖 | 固定版本 | 固定路径（相对仓库根目录） |
|---|---|---|
| Protobuf（含 protoc） | 3.21.12 | `kavio_sdk/device/3rdParty/protobuf-3.21.12/` |
| JsonCpp | 1.9.5 | `kavio_sdk/device/3rdParty/jsoncpp-1.9.5/` |
| Lua | 5.4.1 | `kavio_sdk/device/3rdParty/lua-5.4.1/` |
| LVGL | 9.4.0 | `kavio_sdk/device/3rdParty/lvgl-9.4.0/` |
| FreeType | 2.10.0 | `kavio_sdk/device/3rdParty/freetype-2.10.0/` |

Lua 使用扁平源码目录（`lua.h` 与 `lapi.c` 在同一级）；不再自动选择 `lua-5.4*`，不再扫描全部 `.c` 文件。LVGL 和 FreeType 使用 SDK 指定版本；启用 UI 后若缺失即配置失败。Protobuf 的 `protoc` 由固定版本源码构建，不能用 PATH 下其他版本替代。

## 系统工具与开发依赖

目标环境基准：Ubuntu 20.04 / AArch64（VS680）。

| 依赖 | 版本策略 | Ubuntu 包 |
|---|---|---|
| CMake | 最低 3.16，设备基准 3.16.3 | `cmake` |
| GCC / G++ | 最低 9，设备基准 9.4 | `gcc g++` |
| Make | 系统提供 | `make` |
| Python（构建工具） | 最低 3.8 | `python3` |
| CPython Native Binding | **固定 ABI 3.8**，解释器 `/usr/bin/python3.8`，开发头 `/usr/include/python3.8/Python.h`、开发库 `/usr/lib/aarch64-linux-gnu/libpython3.8.so` | `python3.8-dev` |
| POSIX Threads / libc / `dl` / `m` / `rt` | 随设备系统工具链 | 通常由 `libc6-dev` 提供 |

安装基准工具：

```bash
sudo apt update
sudo apt install cmake build-essential python3 python3.8-dev
```

`build-essential` 包含常用编译器/Make/开发头；安装完成仍需运行检查脚本确认版本。系统工具使用最低版本限制而非精确补丁号，避免 Ubuntu 安全更新造成无谓阻断。Python 3.8 的 **minor ABI** 则是硬约束，不能自动退到其他 Python 版本。`kavio_project.json` 中独立 App 环境的 `pythonVersion` 必须与 SDK Native Binding 的 CPython 3.8 ABI 一致；单独创建 Conda 环境不会改变 SDK 原生扩展的编译 ABI。

## Windows Host

Windows DLL 不使用上面的 Linux 工具链；各 Demo 的 `.vcxproj` 目前配置了 VS v140 工具集与 Windows SDK 8.1。编译 Host DLL 需安装对应的 VS C++ 工具集和 Windows SDK。Host 的工程头文件相对路径保持现有设计，与 Device SDK 依赖查找相互独立。

## 维护原则

1. 增减依赖或固定版本时同时修改 `dependencies.json` 和相关 CMake 文件。
2. 缺少启用组件所需依赖时应失败，不自动降级或替换为其他安装位置的版本。
3. 不对源码目录做模糊版本查找；构建生成文件（Protobuf 等）可由明确的源文件列表生成。
4. 第三方库内部为了实现功能执行的系统库探测，属于该库自身配置行为；SDK 已明确关闭 FreeType 未使用的可选压缩/字体整形依赖。不要把库的可选依赖误写成 SDK 必装依赖。
