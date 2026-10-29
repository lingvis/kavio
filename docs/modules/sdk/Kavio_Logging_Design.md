# Kavio 统一日志系统设计与使用指南

本文件是当前 SDK 的正式日志设计说明。适用于 KavioWin、Platform、KavioAppHost、Windows Host Component（DLL）及 Device Native/Python App。

## 1. 基本规则

- 统一实现：C++14 **header-only** `kavio_sdk/common/logging/KLogger.h`，使用 SDK 随附 `common/logging/spdlog`（1.15.3、bundled fmt），**无需生成或链接日志专用库**。
- 日志宏：`KLOGT / KLOGD / KLOGI / KLOGW / KLOGE / KLOGC`，采用 fmt `{}` 占位符。源码文件、行号由宏记录。
- 只向本地文件输出日志，**不通过 IPC、USB、Channel 转发**。运行状态通知属于独立业务机制，不受此约束。
- **按 UTC 自然日切割**：同一天写同一个文件；UTC 日期变化即打开新文件。不按文件大小轮转。运行期即使没有新日志，C++ 后台刷新线程也会在检测到日期变化后切换文件；Python 在下一条日志发出时切换。
- **历史保留 72 小时**：按日志文件最后修改时间计算；当前正在写入的文件不删除。初始化时清理，之后每小时检查。当前目录以外的文件和不符合 Kavio 命名规则的文件不会被清理。
- 72 小时不是进程内文件大小或目录占用空间上限。单日文件没有大小限制；日志输出量过大仍然可能耗尽磁盘。
- 官方 App 初始化接口没有自定义目录参数。该限制属于开源 SDK 的默认约定，并非安全机制。

## 2. 路径与文件名

| 组件 | 日志目录 | 日志文件名前缀 |
| --- | --- | --- |
| KavioWin | `%APPDATA%\Kavio\log\` | `KavioWin` |
| Platform | `./logs/`（相对 Platform 工作目录） | `kavio` |
| KavioAppHost | `%APPDATA%\Kavio\log\<app_id>\` | `KavioAppHost` |
| Host Component DLL | 与 AppHost 相同的 App 专属目录 | `HostComponent` |
| Device Native/Python App | `<app_root_dir>/.logs/<app_id>/` | `main`（可改名） |

AppHost 和 AppService 通过 `KAVIO_APP_LOG_DIR` 将默认日志目录提供给 App；若变量缺失，`Logger::Init` 返回失败，不擅自写入当前工作目录。

文件名格式统一为：

```text
<name>_YYYYMMDD_HHMMSS_mmm_<pid>.log
```

所有日期/时间均为 **UTC**。同一时间戳和 PID 的文件已存在时，追加 `_1`、`_2` 等碰撞后缀。示例：

```text
KavioAppHost_20261028_235959_123_4568.log
KavioAppHost_20261029_000000_010_4568.log
HostComponent_20261028_235959_456_4568.log
HostComponent_20261029_000000_018_4568.log
```

跨天新文件继续使用上述**同一命名格式**，日期和时间改为新文件实际创建时刻，不追加 `.1.log` 或 `.2026-10-29` 后缀。

### 2.1 跨天会话关联与未来日志查询

`Logger::SessionId()` 是**首次初始化时确定的启动会话 ID**；跨天换文件时保持不变。`Logger::CurrentFileName()` 返回当前打开的文件名。每个日志文件首条元数据记录均写入同一 `session_id=...`，使以后可以将同一进程启动跨日产生的多份文件关联。

查询“最近一次启动的日志”时，不能简单只选最新文件：最新文件可能仅是同一长时间运行进程的新日期分段。未来日志检索服务应按文件中的 `session_id` 归组，再按启动时间降序筛选，并返回该会话的所有日切文件。本次仅提供命名与元数据，不新增文件传输/远程查询协议。

**注意**：AppHost 与 DLL 的 header-only Logger 在不同二进制模块中有独立静态状态，因此它们分别生成日志文件和会话 ID。它们归属于同一 `app_id` 文件夹，可以分别查询，不宣称两者具有相同会话 ID。

## 3. Device Native C++ App

```cpp
#include <KLogger.h>

int main() {
    auto result = kavio::logging::Logger::Init("main", kavio::logging::Level::Info);
    if (!result.ok()) return 1; // result.code / result.message

    KLOGI("App started, frame={}", 42);
    KLOGE("Operation failed, code={}", -1);

    kavio::logging::Logger::Shutdown();
    return 0;
}
```

`name` 只能为 1–64 字符的 ASCII 字母、数字、下划线和连字符；不得包含路径、文件扩展名。`SetLevel` 可动态设定级别，`Flush` 强制写盘，`Shutdown` 关闭文件。C++ 多线程并发写入安全。没有初始化的日志调用安全忽略。

## 4. 系统组件

使用同一份头文件，不提供旧 Logger 兼容层：

```cpp
using namespace kavio::logging;
Logger::InitSystem("KavioWin", Runtime::KavioWin, Level::Debug);
Logger::InitSystem("kavio", Runtime::Platform, Level::Debug);
Logger::InitSystem("KavioAppHost", Runtime::AppHost, Level::Debug);
```

系统组件与 App 均有各自的本地文件 sink。

## 5. Windows Host DLL：由宿主自动触发 DLL 自有日志初始化

**不再使用日志回调或 `KavioHostBindLogging`。** DLL 内部直接通过相同的 header-only Logger 写文件，因此 `KLOGI("frame={}", frame)` 的格式化与普通 C++ 程序完全一致，不需要开发者构造日志字符串或调用繁复的回调参数。

在**一个**实现 Host Component 导出的 `.cpp` 文件中：

```cpp
#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>

void CaptureFrame(unsigned frame) {
    KLOGI("Capture started, frame={}", frame);
}
```

`KavioHost.h` 随 `KAVIO_HOST_IMPLEMENTATION` **自动导出**两个 C ABI 生命周期函数：

- `KavioHostInitLogging()`：由 AppHost 在 `LoadLibraryExW()` 后、`KavioHostCreate()` 前调用；DLL 自行初始化文件 Logger，默认日志名为 `HostComponent`。
- `KavioHostShutdownLogging()`：由 AppHost 在 `KavioHostDestroy()` 后、`FreeLibrary()` 前调用，刷新并关闭 DLL 日志文件。

它们是零参数本地生命周期调用，**不是逐条日志回调**，也不要求第三方用户手工调用 `Logger::Init()`。DLL 业务只写 `KLOG*`。初始化失败会使 Host Component 加载失败，避免业务正常运行却静默丢日志。

DLL 与 AppHost 的日志保存在**同一个 App 日志目录，但不同文件**。这是选择 header-only、无日志回调、无跨 DLL Logger 对象共享后的明确取舍。不要尝试让独立的 sink 同时写同一文件，否则无法保证文件切换、写入原子性与关闭顺序。

开发者应在 `KavioHostStop/KavioHostDestroy` 阶段停止业务线程并保证不再写入，宿主随后关闭 DLL 日志并卸载模块。`KavioHostApi::log` 和日志 IPC 均已删除；旧组件需要重新编译。

## 6. Device Python App

Python 侧使用标准库 `logging.FileHandler` 封装，不依赖 C++ 扩展或第三方 Python 包：

```python
from kavio_sdk import app_logger

logger = app_logger.init("main", "INFO")
logger.info("Device app started, id=%s", app_id)
app_logger.shutdown()
```

Python 适配同样按 UTC 日期切割，写入同一 `session_id` 元数据，保留最近 72 小时。`app_logger.session_id()` 返回启动 ID；`app_logger.current_filename()` 返回当前文件名。Python 格式化沿用标准库 `%s` 风格，不是 C++ fmt `{}`。

## 7. 输出格式、清理与风险

```text
[2026-10-28T23:59:59.999Z] [4568:4610] [info] [main.cpp:80] App running
[2026-10-29T00:00:00.001Z] [4568:4610] [info] [main.cpp:81] Next UTC day
```

- 内容使用 UTF-8，记录 UTC 时间、PID/TID、日志等级、源码位置和消息；每份新文件另含标记启动会话的元数据行。
- C++ 使用 `basic_file_sink_mt`，Error/Critical 立即 flush，其余默认每秒 flush；切割时先打开新文件，打开失败则保留旧 sink，后续日志写入仍可继续，不因切割失败直接终止业务。
- 过期清理仅识别符合 Kavio 命名模式的普通日志文件，依据最后写入时间执行，当前本 Logger 打开的文件始终排除。
- 对于持续运行超过三天的进程，**已经跨日关闭的旧文件也会参与 72 小时清理**；当前日仍在写入的文件无容量限制。如果对应进程长期未运行，清理会等到下次有 Logger 启动时进行，不承诺停机期间定时清除。
- Linux Device AppService 现存的 stdout/stderr 重定向文件 `<app_root_dir>/.logs/<app_id>.log` 属于独立机制，不会计入本 Logger 的日切或清理范围。

## 8. 构建

Device SDK 的 CMake 配置导出 `common/logging`；Windows KavioWin、AppHost 和 Host SDK Demo 只需添加 `common/logging` 一个 include 路径。无需日志专用 `.lib/.a/.dll/.so`。当前公用源码以 C++14 为下限；Windows v140 与 VS680 ARM64 的完整构建及实机验证需在对应环境完成。

### 8.1 Windows 源码编码约定

现有部分 Windows 工程使用 VS2015/v140 和系统代码页（例如 CP936/GBK），不统一强制添加 `/utf-8`，以免改变旧 `.cpp` 文件中中文字符串常量的解释方式。公共 `KLogger.h` 对捆绑的 fmt 使用 `FMT_UNICODE=0` 跳过“编译器必须使用 UTF-8 执行字符集”的静态断言，并只在第三方头文件包含期间屏蔽代码页告警 C4819。此举仅保证代码在传统代码页下可以编译，**不进行文本编码转换**。

`KLOG*` 接收的 `std::string` 原样写入日志。若业务源文件中中文字符串字面量按 GBK 编译，产生的字节仍可能是 GBK，不能仅因为日志文件使用 `.log` 扩展名就认为内容必然是 UTF-8。需要 UTF-8 日志时，应由业务代码提供 UTF-8 字符串（例如通过现有宽字符串转 UTF-8 工具进行转换）；待工程源码全面统一到 UTF-8 后，可再启用编译器 `/utf-8` 和 fmt 的完整编码检查。

