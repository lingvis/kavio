# Kavio Host SDK

Kavio Host SDK 用于开发运行在 Windows 上的 Kavio Host Component。

Host Component 是一个 **Windows x64 DLL**。它适合实现必须运行在 Windows 侧的功能，例如系统信息采集、文件访问、图片解码、自定义 App Panel，以及通过 USB Channel 与 Kavio Device 上的 Device Component 交换业务数据。

Host SDK 是 header-only C ABI，不需要单独构建或链接 SDK 库。统一日志也是 header-only，需额外引入公共日志头文件目录。

## 1. 引入 SDK

将：

```text
<repository>/kavio_sdk/host/include
```

加入 Visual Studio 工程的 include path，然后：

```cpp
#include <kavio/KavioHost.h>
```

实现 DLL 导出函数的 `.cpp` 文件应在包含头文件前定义：

```cpp
#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>
```

并使用：

```cpp
KAVIO_HOST_ENTRY
KAVIO_HOST_CALL
```

定义导出入口。

## 2. 必须实现的 DLL 入口

```text
KavioHostCreate
KavioHostStart
KavioHostStop
KavioHostDestroy
KavioHostOnData
```

### `KavioHostCreate`

创建 Host Component 实例并保存 `KavioHostApi`。

### `KavioHostStart`

启动业务逻辑和工作线程。

### `KavioHostStop`

停止业务逻辑。返回后不应继续主动发送新的 Device 数据。

### `KavioHostDestroy`

释放由 `KavioHostCreate` 创建的组件实例和资源。

### `KavioHostOnData`

接收同一 App 的 Device Component 通过 USB Channel 发来的一条完整数据报。

传入的 `data` 只在本次调用期间有效。如果需要异步处理，Host Component 必须自行复制数据。

## 3. 可选 DLL 入口

```text
KavioHostPrepareStop
KavioHostOnDeviceState
KavioHostOpenPanel
```

### `KavioHostPrepareStop`

在正式 Stop 前停止接受新任务，并收敛正在处理的任务。

### `KavioHostOnDeviceState`

接收 Device Component 在线/离线状态变化。

### `KavioHostOpenPanel`

打开该 App 自己实现的 Windows App Panel。

不需要某项可选能力时，不导出对应函数即可。

## 4. KavioHostApi

`KavioHostCreateParams::host` 提供当前 Host Component 可调用的宿主能力。

| API | 用途 |
| --- | --- |
| `send` | 通过当前 App 的 USB Channel 向 Device Component 发送完整数据报 |
| `is_device_online` | 查询 Device Component 是否可双向通信 |
| `get_channel_id` | 获取当前 App 的稳定 Channel ID |
| `restart` | 请求重新启动当前 Host Component |
| `get_data_path` | 获取当前 App Host 私有可写数据目录 |

`KavioHostApi` 指针从 `KavioHostCreate` 开始有效，到 `KavioHostDestroy` 返回为止。

使用扩展字段前通过：

```cpp
KAVIO_HOST_API_HAS_FIELD(api, field)
```

检查当前 Host ABI 是否提供该字段。

## 5. USB Channel

Host Component 与 Device Component 的业务通信统一使用 USB Channel。

### Host → Device

调用：

```cpp
host_api->send(host_api->context, data, size);
```

### Device → Host

实现：

```cpp
KavioHostOnData(...)
```

App 自己定义消息格式。推荐把 Host 与 Device 共用的协议声明放在：

```text
kavio_app/<app_name>/common/
```

USB Channel 只负责完整业务数据报的双向传输。Host Component 不需要处理底层 USB 协议。

## 6. Device 在线状态

Host 可以主动查询：

```cpp
host_api->is_device_online(host_api->context)
```

也可以实现：

```cpp
KavioHostOnDeviceState(...)
```

接收状态变化。

Device 离线时，Host 不应假定新业务数据能够送达。对于周期任务、文件传输等业务，应在 App 层自行决定暂停、丢弃或恢复策略。

## 7. App Panel

需要 Windows 自定义界面时实现：

```cpp
KavioHostOpenPanel(...)
```

Panel 属于 Host Component 自己的 UI，可用于：

- 文件选择和拖放；
- Windows 数据展示；
- App 自定义配置；
- 预览和交互控制；
- 向 Device Component 发送业务命令。

标准 App Configuration 与 App Panel 是两种独立能力：

- 简单配置优先使用 `.kap` 中的标准 App Configuration；
- 需要复杂 Windows UI 时使用 Host Panel。

## 8. Host 私有数据目录

通过：

```cpp
host_api->get_data_path(host_api->context)
```

获取该 App Host Component 的私有可写数据目录。

App 的持久化缓存、Host 侧配置和业务数据应保存在该目录，不要依赖 DLL 所在目录可写。

## 9. 日志

Host DLL 通过统一 header-only Logger **直接写入独立本地日志文件**，不经过 AppHost 日志回调或 IPC。与 AppHost 共用 App 专属目录，但分别保存 `HostComponent_*.log` 与 `KavioAppHost_*.log`。除 `kavio_sdk/host/include` 外，额外加入：

```text
kavio_sdk/common/logging
```

使用示例：

```cpp
#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>  // SDK 自动导出 DLL 日志生命周期入口

void OnStarted() {
    KLOGI("Host Component started, frame={}", 42);
}
```

AppHost 加载 DLL 后自动调用 `KavioHostInitLogging()`，DLL 内部打开自己的文件；卸载前自动调用 `KavioHostShutdownLogging()`。开发者无需显式调用 `Logger::Init()`，也不需要处理日志回调。按 **UTC 自然日** 切割为 `HostComponent_YYYYMMDD_HHMMSS_mmm_pid.log`，历史保留 72 小时，不按大小轮转。

`KavioHostApi::log` 已删除；请使用 `KLOGT/KLOGD/KLOGI/KLOGW/KLOGE/KLOGC`。旧 Host DLL 需要重新编译。

请阅读 [统一日志模块完整设计](../../docs/modules/sdk/Kavio_Logging_Design.md)。

## 10. ABI 约束

Host ABI 遵循以下规则：

- 使用 C ABI；
- Windows 架构为 x64；
- 结构体使用 `struct_size` 表示可用字段范围；
- 新字段只在结构体尾部扩展；
- ABI 边界不传递 C++ STL 对象；
- ABI 边界不传播 C++ 异常；
- ABI 边界不跨运行库分配和释放内存；
- Host Component 只依赖 `KavioHost.h` 中的公开类型和函数。

## 11. 最小实现

```cpp
#define KAVIO_HOST_IMPLEMENTATION
#include <kavio/KavioHost.h>

struct MyHost {
    const KavioHostApi* api = nullptr;
};

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostCreate(
    const KavioHostCreateParams* params,
    KavioHostHandle* out_host)
{
    if (!params || !out_host || !params->host) {
        return KavioResult_InvalidArgument;
    }

    MyHost* self = new MyHost();
    self->api = params->host;
    *out_host = self;
    return KavioResult_Success;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStart(
    KavioHostHandle host)
{
    return host ? KavioResult_Success : KavioResult_InvalidArgument;
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostStop(
    KavioHostHandle host)
{
    return host ? KavioResult_Success : KavioResult_InvalidArgument;
}

KAVIO_HOST_ENTRY void KAVIO_HOST_CALL KavioHostDestroy(
    KavioHostHandle host)
{
    delete static_cast<MyHost*>(host);
}

KAVIO_HOST_ENTRY KavioResult KAVIO_HOST_CALL KavioHostOnData(
    KavioHostHandle host,
    const void* data,
    size_t size)
{
    if (!host || !data || size == 0) {
        return KavioResult_InvalidArgument;
    }

    return KavioResult_Success;
}
```

## 12. 构建输出

Kavio App Demo 约定最终 Host DLL 暂存为：

```text
<app>/output/host/host.dll
```

再由 `kavio_app` 打包工具和 Device Component 一起生成 `.kap`。

完整 Host 示例：

```text
kavio_app/demo_communication
kavio_app/demo_panel_image
kavio_app/demo_metrics_panel
```
