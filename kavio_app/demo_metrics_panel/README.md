# demo_metrics_panel

## 1. 功能说明

`demo_metrics_panel` 展示 Python Device Component 与 Windows Host Component 组合开发 Kavio App 的方式。

运行后：

1. Python Device Component 每 2 秒通过 USB Channel 请求 Windows CPU、内存和固定磁盘信息；
2. Host Component 返回当前指标；
3. Python 在设备端生成 560×220 的 BGRA8888 TX 内容；
4. Host Component 提供 App Panel，并显示同尺寸预览矩形；
5. 用户拖动 Panel 中的矩形后，Host 把新的 X/Y 坐标发送给 Python Device Component；
6. Device 在后续渲染中更新 TX 位置。

这个 Demo 用于展示 Python App 的完整构建、USB Channel 通信、TX 输出和 App Panel 联动。

## 2. 准备 Python Device Component

Python Device Component 不执行 Native 编译。`device/build.sh` 负责语法检查、SDK 暂存和运行依赖整理。

推荐仓库结构：

```text
<repository>/
├── kavio_app/
│   └── demo_metrics_panel/
└── kavio_sdk/
    └── device/
        └── py/
            ├── kavio_sdk/
            │   └── __init__.py
            └── requirements.txt
```

执行：

```bash
cd kavio_app/demo_metrics_panel/device
./build.sh
```

默认 SDK 包目录：

```text
<repository>/kavio_sdk/device/py/kavio_sdk
```

如果使用其他 SDK 位置，`--sdk-dir` 必须指向包含 `__init__.py` 的 `kavio_sdk` 包目录：

```bash
./build.sh --sdk-dir /path/to/kavio_sdk/device/py/kavio_sdk
```

准备完成后输出：

```text
kavio_app/demo_metrics_panel/output/device/
├── main.py
├── protocol.py
├── renderer.py
├── kavio_bridge.py
├── requirements.txt
└── kavio_sdk/
```

`output/device/requirements.txt` 会合并 Kavio Python SDK 和本 Demo 的依赖。

清理：

```bash
./build.sh --clean
```

## 3. 构建 Host Component

使用 Visual Studio 2015 或更高版本打开：

```text
kavio_app/demo_metrics_panel/host/DemoPythonMetricsPanel.sln
```

构建配置：

```text
Platform:      x64
Configuration: Debug 或 Release
Toolset:       v140
```

编译完成后自动暂存：

```text
kavio_app/demo_metrics_panel/output/host/host.dll
```

## 4. 检查和打包

Python Device 文件和 Host DLL 都准备完成后：

```bash
cd kavio_app
python3 kavio_app.py status demo_metrics_panel
python3 kavio_app.py package demo_metrics_panel
```

最终安装包：

```text
kavio_app/demo_metrics_panel/dist/demo_metrics_panel-1.0.0.kap
```

`package/kavio_package.json` 已声明：

```text
runtime:        python
entry:          main.py
pythonVersion:  3.10
installMode:    auto
requirements:   requirements.txt
```

## 5. 目录结构

```text
demo_metrics_panel/
├── common/DemoPythonMetricsProtocol.h
├── device/
│   ├── main.py
│   ├── protocol.py
│   ├── renderer.py
│   ├── kavio_bridge.py
│   ├── requirements.txt
│   └── build.sh
├── host/
│   ├── DemoPythonMetricsPanel.sln
│   ├── DemoPythonMetricsPanelHost.vcxproj
│   └── HostComponent.cpp
└── package/
    ├── kavio_package.json
    └── icon.png
```

## 6. USB Channel 协议

Windows 侧协议：

```text
common/DemoPythonMetricsProtocol.h
```

Python 侧协议：

```text
device/protocol.py
```

消息类型：

```text
Query         Python -> Host
Response      Host   -> Python
Position      Host   -> Python
DeviceReady   Python -> Host
```

两端都使用 little-endian 定长结构。修改协议字段时，必须同时更新 C++ 结构和 Python `struct.Struct` 定义。

Host 最多返回 8 个固定磁盘，当前 Device UI 最多显示前 3 个。

## 7. Python 代码分层

### `main.py`

负责 App 主循环、查询周期、状态快照和 TX 更新时机。

### `protocol.py`

负责 USB Channel 消息编码、解码和公共头校验。

### `renderer.py`

只使用 Python 标准库生成 BGRA8888 像素，不依赖第三方 UI 或图像库。

### `kavio_bridge.py`

把 Kavio Python SDK 的生命周期、USB Channel 和 TX API 收敛为 Demo 需要的最小接口。

业务代码优先通过 `kavio_bridge.py` 使用 SDK。SDK API 发生调整时，可以优先在这一层适配，而不把底层接口细节散落到 `main.py`。

## 8. DeviceReady 与位置同步

USB Channel 可用后，Python Device Component 每秒发送一次 `DeviceReady`。

Host 只有收到 DeviceReady 后才下发 Panel 位置，避免 Device 业务循环尚未准备完成时提前发送控制消息。

Panel 只负责 Windows 侧预览和交互。设备 TX 的像素由 `renderer.py` 独立生成，Panel 不是 TX 的像素来源。

## 9. 运行验证

1. 安装并启动 App；
2. 打开 App Panel；
3. 确认设备 TX 出现 `PYTHON METRICS DEMO`；
4. 确认 CPU、内存和磁盘数据约每 2 秒更新；
5. 拖动 Panel 中的矩形；
6. 确认设备 TX 坐标同步变化；
7. 检查 Device 日志，确认没有 Python import、USB Channel 或 TX Surface 错误。

## 10. 常用修改入口

| 目标 | 文件 |
| --- | --- |
| 修改 USB Channel 协议 | `common/DemoPythonMetricsProtocol.h`、`device/protocol.py` |
| 修改 Windows 指标采集 | `host/HostComponent.cpp` |
| 修改 Python TX UI | `device/renderer.py`、`device/main.py` |
| 修改 Python SDK 适配 | `device/kavio_bridge.py` |
| 修改 App Panel | `host/HostComponent.cpp` |
