# demo_communication

## 1. 功能说明

`demo_communication` 展示一个包含 Native Device Component、Windows Host Component 和 App Configuration 的完整 Kavio App。

运行后：

1. Device Component 每 2 秒通过 USB Channel 请求一次 Windows 系统指标；
2. Host Component 采集 CPU、物理内存和固定磁盘信息并返回；
3. Device Component 把最新结果绘制到 TX Surface；
4. App Configuration 提供 CPU、Memory、Disk 三个显示开关；
5. 配置会保存到 App 数据目录，并在后续启动时继续生效。

这个 Demo 适合作为 Host/Device 双端通信和 App Configuration 的基础模板。

## 2. 构建 Device Component

### 环境要求

- CMake 3.16 或更高版本；
- 支持 C++17 的编译器；
- 仓库中存在 `kavio_sdk/device`；
- 目标设备为 AArch64，或者提供对应交叉编译工具链。

### Release 构建

```bash
cd kavio_app/demo_communication/device
./build.sh -r
```

交叉编译：

```bash
./build.sh -r --toolchain /path/to/aarch64-toolchain.cmake
```

该 Demo 默认检查最终可执行文件是否为 AArch64。只有已经通过其他方式确认目标架构时才使用：

```bash
./build.sh -r --skip-arch-check
```

构建产物：

```text
kavio_app/demo_communication/output/device/demo_communication
```

清理 Device 构建：

```bash
./build.sh --clean
```

## 3. 构建 Host Component

使用 Visual Studio 2015 或更高版本打开：

```text
kavio_app/demo_communication/host/DemoAppCommunication.sln
```

构建配置：

```text
Platform:      x64
Configuration: Debug 或 Release
Toolset:       v140
```

Host 工程通过相对路径引用：

```text
kavio_sdk/host/include
```

编译完成后，Post-Build Event 自动暂存：

```text
kavio_app/demo_communication/output/host/host.dll
```

## 4. 检查和打包

Device 和 Host 都构建完成后：

```bash
cd kavio_app
python3 kavio_app.py status demo_communication
python3 kavio_app.py package demo_communication
```

最终安装包输出到：

```text
kavio_app/demo_communication/dist/demo_communication-1.0.0.kap
```

`status` 会同时检查 App 图标、App Configuration 文件和声明的运行产物。

## 5. 目录结构

```text
demo_communication/
├── common/DemoMetricsProtocol.h
├── device/
│   ├── CMakeLists.txt
│   ├── build.sh
│   └── main.cpp
├── host/
│   ├── DemoAppCommunication.sln
│   ├── DemoAppCommunicationHost.vcxproj
│   └── HostComponent.cpp
└── package/
    ├── kavio_package.json
    ├── icon.png
    └── schema_description.json
```

`common/DemoMetricsProtocol.h` 是 Host 和 Device 共用的 USB Channel 协议定义。

## 6. USB Channel 协议

协议定义：

```text
common/DemoMetricsProtocol.h
```

当前使用定长 POD 消息，不依赖 Protobuf：

```text
QueryMessage       Device -> Host
ResponseMessage    Host   -> Device
```

每条消息包含 magic、协议版本、消息类型、消息长度和 `sequence`。`sequence` 用于关联请求与响应。

Host 最多返回 16 个固定磁盘。CPU 使用率由两次 `GetSystemTimes()` 累计值计算差值，因此 Host 第一次采样返回 `0` 属于正常行为。

App Configuration 只决定哪些指标绘制到 TX，不会停止指标查询。这样可以把业务通信和显示配置保持独立。

## 7. App Configuration

配置定义：

```text
package/schema_description.json
```

当前配置项：

```text
show_cpu
show_memory
show_disks
```

SDK 通过 `KAVIO_APP_SCHEMA_DESCRIPTION_PATH` 读取固定 Schema，
通过 `KAVIO_APP_DATA_DIR` 保存有效配置 `config.json`，无需 App 代码自行读写文件。
首次运行从 Schema 字段 `value` 生成默认 JSON。例如：

```json
{"show_cpu":true,"show_memory":true,"show_disks":true}
```

Host 更新时只发送完整 JSON；SDK 在 `HandlePacket()` 中预先校验、保存、更新内存，
成功后通过既有的 `OnMessage()` 交付 SET，App 在其中调用 `GetAppConfig()`
读取最新配置。上位机在 `ChannelReady` 获得 App 列表后，对运行中的 App
主动查询 Schema 与当前配置；设备不主动推送配置快照。
如果用户文件非法，`Start()` 返回错误，
Demo 默认退出；仅在用户显式设定 `KAVIO_DEMO_RESTORE_CONFIG=1` 时恢复默认配置。
`schema_description.json` 必须存在，没有配置的 App 必须明确写 `{}`。

## 8. TX 输出

Device Component 创建一个固定的 TX Surface：

```text
Size:     760 x 420
Format:   BGRA8888
Position: 0, 0
z-order:  100
```

只有收到新的指标响应或配置发生变化时才重新绘制，不会在主循环中无条件刷新 TX。

## 9. 运行验证

1. 安装生成的 `.kap`；
2. 启动 App；
3. 确认设备端 TX 显示 CPU、Memory 和 Disk 数据；
4. 打开 App Configuration；
5. 分别关闭/打开三个显示项，确认 TX 内容同步变化；
6. 重新启动 App，确认配置保持；
7. 确认关闭所有显示项后 USB Channel 指标查询仍持续工作。

## 10. 常用修改入口

| 目标 | 文件 |
| --- | --- |
| 修改 Host/Device 消息字段 | `common/DemoMetricsProtocol.h` |
| 修改 Windows 数据采集 | `host/HostComponent.cpp` |
| 修改查询周期 | `device/main.cpp` 中的 `kQueryIntervalMs` |
| 修改 TX 内容 | `device/main.cpp` 中的绘制逻辑 |
| 修改 App Configuration | `package/schema_description.json` 和 `device/main.cpp` |
