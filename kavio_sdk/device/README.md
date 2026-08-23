# Kavio Device SDK

Kavio Device SDK 用于开发运行在 Kavio Device 上的 Device Component，支持 C++ 和 Python。App 通过统一 SDK 使用设备能力，不直接依赖具体 SoC 或板级实现。

Device Component 可以读取 HDMI RX 视频帧、创建 TX 图层、发送键盘鼠标事件、读取设备运行状态、处理 App Configuration，并通过 USB Channel 与同一 App 的 Windows Host Component 通信。

## 1. 目录结构

```text
device/
├── CMakeLists.txt
├── build.sh
├── include/kavio/          # C++ 公共 API
├── src/                    # SDK 实现
│   └── internal/           # SDK 私有实现
├── proto/                  # Device SDK 协议定义
├── py/kavio_sdk/           # Python SDK
├── tools/
└── 3rdParty/               # Device SDK 构建依赖
```

## 2. 开发能力

| 能力 | 说明 |
| --- | --- |
| Client 生命周期 | 启动、停止、断开恢复和 ready/invalid 状态通知 |
| Runtime Data | 查询 RX、TX、输入设备运行状态 |
| RX | 启用/关闭 RX 取帧，读取最新视频帧 |
| TX | 创建 BGRA8888 Surface、写帧、清屏、销毁 |
| TX 参数 | 启用 TX、设置 App 层级 `z_order` |
| Input | 发送键盘和鼠标事件 |
| USB Channel | 与同一 App 的 Windows Host Component 双向传输业务数据 |
| App Configuration | 接收配置读写请求并返回处理结果 |
| Stats | 查询 Device 运行统计 |

当前公开像素格式定义包括：

```text
UYVY
YUYV
BGRA8888
```

`CreateTxSurface()` 创建的 TX Surface 使用 `BGRA8888`。

## 3. C++ Device Component

### 3.1 构建要求

- CMake 3.16 或更高版本；
- C++17 编译器；
- POSIX Threads；
- `rt`；
- protobuf 3.21.12 源码，放置在 `3rdParty/protobuf-3.21.12/`。

### 3.2 在 App 中引用 SDK

Native App 使用 SDK 已构建产物，不把 Device SDK 源码加入自己的构建图。先执行 `kavio_sdk/device/build.sh`，随后在 App 中加载生成的 CMake package：

```cmake
set(KAVIO_DEVICE_SDK_DIR "${REPOSITORY_ROOT}/kavio_sdk/device/build")
find_package(KavioDeviceSdk CONFIG REQUIRED
    PATHS "${KAVIO_DEVICE_SDK_DIR}"
    NO_DEFAULT_PATH)

target_link_libraries(my_device_app PRIVATE Kavio::DeviceSdk)
```

业务代码包含：

```cpp
#include <kavio/KavioClient.h>
```

`Kavio::DeviceSdk` 会公开 SDK include 路径，并链接 SDK 构建阶段已经生成的静态依赖。App 不需要引用 `src/internal/`，也不会重新编译 SDK 源码。

### 3.3 单独构建 SDK

Debug：

```bash
cd kavio_sdk/device
./build.sh
```

Release：

```bash
./build.sh --release
```

清理：

```bash
./build.sh --clean
```

主要输出到：

```text
kavio_sdk/device/build/lib/
kavio_sdk/device/build/KavioDeviceSdkConfig.cmake
```

## 4. C++ 生命周期

典型调用顺序：

1. 创建 `kavio::KavioClient`；
2. 配置 `SessionOptions` 和 `Callbacks`；
3. 调用 `Start()`；
4. 等待 `on_client_ready`；
5. Client ready 后使用 RX、TX、Input、USB Channel 等能力；
6. 收到 `on_client_invalid` 后停止使用旧 RX 帧和旧 TX Surface；
7. Client 恢复并再次触发 `on_client_ready` 后重新创建需要的运行资源；
8. 退出时销毁 TX Surface，并调用 `Stop()`。

`Start()` 是非阻塞调用。返回成功表示后台启动已提交，业务代码仍应以 `on_client_ready` 或 `IsClientReady()` 作为可用条件。

## 5. RX

启用 RX：

```cpp
kavio::RxParams params;
params.enabled = true;
params.frame_sample_interval = 1;

client.SetRxParams(params, timeout);
```

读取最新帧：

```cpp
kavio::ImageFrame frame;
auto status = client.ReadLatestRxFrame(frame);
```

`frame_sample_interval` 表示 Device Runtime 每隔多少个源视频帧向该 App 提供一次 RX 更新：

- `1`：每个可用源帧；
- `2`：每两个源帧采样一次；
- `N`：每 N 个源帧采样一次。

实际可读取帧率同时受 HDMI 输入帧率、设备负载和 App 自身读取频率影响。

`ReadLatestRxFrame()` 返回的是当前最新帧副本，App 不需要管理底层视频缓冲生命周期。

## 6. TX Surface

创建 TX Surface：

```cpp
kavio::TxSurface surface;
auto status = client.CreateTxSurface(width, height, surface, timeout);
```

提交一帧：

```cpp
kavio::ImageFrame frame;
frame.width = width;
frame.height = height;
frame.stride = width * 4;
frame.pixel_format = kavio::PixelFormat::kBGRA8888;
frame.dest_pos_x = 100;
frame.dest_pos_y = 100;
frame.data = ...;

surface.WriteFrame(frame);
```

TX 支持：

- BGRA8888 像素；
- Alpha；
- 独立 Surface 尺寸；
- `dest_pos_x` / `dest_pos_y` 定位；
- `valid_rect` 有效区域；
- 多个 TX Surface；
- App 级 `z_order`。

平台允许的同时 TX Surface 数量由设备 Runtime 配置决定，可通过运行统计查询，不应在 App 中写死平台上限。

需要确认资源已释放时显式调用：

```cpp
surface.Destroy(timeout);
```

## 7. USB Channel

Device Component 使用当前 App 唯一的 USB Channel 与 Host Component 交换业务数据。

发送：

```cpp
client.SendCommChannelData(data, size);
```

接收：

```cpp
callbacks.on_comm_channel_data = [](const std::vector<uint8_t>& payload) {
    // Parse App-defined payload.
};
```

通道状态：

```cpp
client.IsCommChannelReady();
client.GetCommChannelId();
```

C++ Device API 的单条发送数据报大小为：

```text
1 byte ~ 1 MiB
```

USB Channel 传输的是完整 App 数据报。消息格式、版本和分包策略由 App 自己定义。

建议把 Host 与 Device 共用的业务协议放在 App 的 `common/` 目录中。

## 8. Input

发送输入事件：

```cpp
client.SendInputEvent(event);
```

公开输入模型包括：

- 键盘按下/抬起；
- 鼠标移动；
- 左/右/中键；
- 鼠标滚轮。

键盘键值使用 HID Keyboard/Keypad 通用编码。

Kavio Runtime 的输入输出模式定义包括：

```text
USB
BLE
2.4G
```

具体输出模式由设备配置决定，App 只负责发送标准输入事件。

## 9. Runtime Data 与回调

`KavioClient::Callbacks` 可接收：

- `on_client_ready`：Client 可用；
- `on_client_invalid`：Client 已失效；
- `on_runtime_changed`：设备运行状态更新；
- `on_user_config_changed`：用户配置更新；
- `on_rx_changed`：RX 状态或 RX Frame 可用性变化；
- `on_usb_host_online_changed`：Windows USB Host 在线状态变化；
- `on_comm_channel_ready_changed`：USB Channel 可用状态变化；
- `on_comm_channel_data`：收到 Host Component 业务数据；
- `on_app_config_request`：收到 App Configuration 请求。

主动查询接口包括：

```cpp
GetRuntimeData()
GetUserConfig()
GetRxInfo()
IsUSBHostOnline()
IsCommChannelReady()
QueryStats(...)
```

## 10. App Configuration

标准 App Configuration 由 `.kap` 中的配置定义描述。

Device Component 收到配置操作时，会触发：

```cpp
on_app_config_request
```

处理完成后使用：

```cpp
SendAppConfigResponse(...)
```

返回相同的 `request_id` 和操作类型。

完整示例见：

```text
kavio_app/demo_communication
```

## 11. Python Device Component

Python 包：

```text
kavio_sdk/device/py/kavio_sdk
```

运行依赖：

```bash
python3 -m pip install -r kavio_sdk/device/py/requirements.txt
```

Python SDK 的 `sdk_version.py` 和 `proto_py/*_pb2.py` 由 Device SDK 构建入口统一生成。执行：

```bash
cd kavio_sdk/device
./build.sh
```

同一次构建会同时生成 C++ SDK 库、CMake package 和 Python SDK 生成文件。

开发环境可设置：

```bash
export PYTHONPATH=/path/to/repository/kavio_sdk/device/py:${PYTHONPATH}
```

Python SDK 当前入口类为：

```python
from kavio_sdk import IPCClient
```

常用能力包括：

```text
connect / disconnect
set_rx_params
read_latest_rx_frame
set_tx_params
create_tx_shm
write_tx_frame
destroy_tx_shm
send_input_event
send_comm_channel_data
query_runtime_data
query_user_config
send_app_config_response
```

完整 Python App 参考：

```text
kavio_app/demo_metrics_panel
```

该 Demo 的目标 Python 版本为 3.10。

## 12. SDK 版本

版本唯一来源：

```text
include/kavio/KavioSdkVersion.h
```

当前版本：

```text
3.0.0
```

Python 的 `sdk_version.py` 由同一版本头自动生成。

Device Component 应使用与目标设备软件版本匹配的 SDK。连接建立时，设备会检查 SDK 版本是否满足最低接入版本。

## 13. Public / Internal 规则

App 可以直接依赖：

```text
include/kavio/KavioClient.h
include/kavio/KavioSdkVersion.h
include/kavio/Status.h
include/kavio/Types.h
py/kavio_sdk/
```

App 不应直接依赖：

```text
src/internal/
build/sdk_proto_src/
```

`proto/` 是 SDK 的协议源文件。生成的 C++ / Python protobuf 文件由构建系统维护。

## 14. 推荐参考 Demo

| 目标 | Demo |
| --- | --- |
| RX / TX / Input | `kavio_app/demo_rx_tx` |
| Host ↔ Device USB Channel | `kavio_app/demo_communication` |
| 图片、App Panel、TX 联动 | `kavio_app/demo_panel_image` |
| Python Device Component | `kavio_app/demo_metrics_panel` |
