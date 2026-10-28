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
- protobuf 3.21.12 源码，放置在 `3rdParty/protobuf-3.21.12/`；
- JsonCpp 1.9.5 源码，放置在 `3rdParty/jsoncpp-1.9.5/`。

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

## 3.4 统一日志

Device C++ App 无需添加日志专用 `.a`：通过 `Kavio::DeviceSdk` 自动取得公共头文件和 spdlog 头文件搜索路径。

```cpp
#include <KLogger.h>
using namespace kavio::logging;

auto result = Logger::Init("main", Level::Info);
if (result.ok()) {
    KLOGI("Device App started");
}
```

Runtime 通过 `KAVIO_APP_LOG_DIR` 指定 App 专属目录，App 不接收路径参数。日志按 UTC 自然日切割，使用统一时间戳命名；跨日文件保留同一启动会话 ID，历史保留 72 小时，不通过 IPC 转发。Python 示例：

```python
from kavio_sdk import app_logger
logger = app_logger.init("main", "INFO")
logger.info("Device App started")
```

完整设计、文件路径、命名和轮转规则参见 [统一日志设计](../../docs/modules/sdk/Kavio_Logging_Design.md)。

## 4. C++ 生命周期

典型调用顺序：

1. 创建 `kavio::KavioClient`，并实现 `kavio::IClientMessageHandler`；
2. 使用 protobuf 的 `A2PReqCreateSessionPb` 设置会话参数；
3. 调用 `Start()` 启动后台 IPC 接收线程与消息派发线程；
4. 收到 `MSG_P2A_RSP_CREATE_SESSION` 后开始使用 RX、TX、Input、USB Channel 等能力；
5. 使用 SDK 异步接口提交业务请求，并通过 `OnMessage()` 收取 protobuf 应答；
6. Socket 断开时由 `OnDisconnected()` 处理本地连接状态，重新连接后重新建立所需资源；
7. 退出时异步销毁 TX Surface，并调用 `Stop()`。

```cpp
class AppHandler : public kavio::IClientMessageHandler {
public:
    void OnMessage(const kavio::ipc::proto::IPCPackagePb& message) override {
        using namespace kavio::ipc::proto;
        switch (message.header().msg_type()) {
        case MSG_P2A_RSP_CREATE_SESSION:
            // Session 已建立；可以发送业务请求。
            break;
        case MSG_P2A_RSP_STATUS:
        case MSG_P2A_RSP_ERROR:
            // 通过 message.header().track_id() 关联请求结果。
            break;
        default:
            break;
        }
    }
    void OnDisconnected() override { /* IPC 已断开，释放旧 Session 状态。 */ }
};

kavio::ipc::proto::A2PReqCreateSessionPb options;
options.set_client_name("my_device_app");
options.mutable_rx_params()->set_enabled(true);
options.mutable_rx_params()->set_frame_sample_interval(1);
options.mutable_tx_params()->set_enabled(false);
AppHandler handler;
kavio::KavioClient client;
client.Start(options, handler);
```

`Start()` 非阻塞；`options` 直接使用 protobuf 会话请求类型。若不设置 `rx_params`，SDK 默认启用 RX 且 `frame_sample_interval=1`；`tx_params` 默认关闭。可通过第三个参数 `auto_query_runtime_on_connect`（默认为 `true`）决定连接建立后是否自动查询 Runtime。SDK 会自动设置当前进程 PID，并以当前可执行文件名作为优先的客户端名称。

**所有业务 IPC 请求均为异步**：`Status::Ok()` 仅表示消息在本地成功提交，`request_id`/`track_id` 用于关联后续应答，不表示远端完成。SDK 不提供业务请求超时；需要超时的 App 自己管理。

`OnMessage()` 由 SDK 单一派发线程串行调用；耗时操作会延迟后续消息处理，但不直接阻塞 IPC 接收线程。不要在 `OnMessage()` 中等待其他消息完成。`handler` 必须存活至 `KavioClient` 析构完成，`Stop()` 不保证派发线程已完全退出。

## 5. RX

启用 RX（异步请求）：

```cpp
kavio::ipc::proto::RxParamsPb params;
params.set_enabled(true);
params.set_frame_sample_interval(1);
uint64_t request_id = 0;
auto status = client.SetRxParams(params, request_id);
// Platform 执行结果通过 OnMessage() 收到 MSG_P2A_RSP_STATUS / MSG_P2A_RSP_ERROR。
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

实际可读取帧率同时受 HDMI 输入帧率、设备负载和 App 自身读取频率影响。`ReadLatestRxFrame()` 返回当前最新帧副本，App 不需要管理底层视频缓冲生命周期。

使用 TX 参数时直接传入 `kavio::ipc::proto::TxParamsPb`（`enabled`、`z_order`），调用 `SetTxParams(params, request_id)`；无须再定义另一套参数结构。

## 6. TX Surface

创建 TX Surface 是异步请求。收到 `MSG_P2A_RSP_CREATE_TX_SHM` 后，**在 `OnMessage()` 中显式打开本地 SHM**，而不是等待创建函数同步返回图层对象：

```cpp
uint64_t request_id = 0;
client.CreateTxSurface(width, height, request_id);
```

```cpp
// 放在 AppHandler::OnMessage() 的 MSG_P2A_RSP_CREATE_TX_SHM 分支：
const auto& info = message.payload().p2a_rsp_create_tx_shm().tx_shm();
kavio::TxSurface surface;
auto status = client.OpenTxSurface(info, surface);
if (!status.ok()) {
    uint64_t release_request = 0;
    client.DestroyTxSurface(info.id(), release_request);
} else {
    // App 持有或移动保存 surface，随后可以 WriteFrame()。
}
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

销毁时调用 `surface.Destroy(request_id)` 异步提交远端销毁请求，等待 `OnMessage()` 返回相应状态；本地析构不等于远端资源已销毁。

## 7. USB Channel

Device Component 使用当前 App 唯一的 USB Channel 与 Host Component 交换业务数据。

发送：

```cpp
client.SendCommChannelData(data, size);
```

接收：

```cpp
// 在 AppHandler::OnMessage() 中处理 MSG_P2A_DATA_COMM_CHANNEL：
if (message.header().msg_type() == MSG_P2A_DATA_COMM_CHANNEL &&
    message.payload().has_p2a_data_comm_channel()) {
    const std::string& payload = message.payload().p2a_data_comm_channel().payload();
    // Parse App-defined payload.
}
```

通道状态：

```cpp
client.IsCommChannelReady();
// 通道状态变化由 MSG_P2A_NTF_COMM_PEER_STATE / MSG_P2A_NTF_USB_HOST_STATE 通知。
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

## 9. Runtime Data 与消息

C++ SDK 统一将 protobuf `IPCPackagePb` 投递到 `IClientMessageHandler::OnMessage()`，无需定义独立的回调结构。典型消息包括：

IPC 消息方向以 Device App 为基准：`A2P` 表示 App → Platform，`P2A` 表示 Platform → App；
`REQ` 为请求、`RSP` 为回复、`NTF` 为通知、`DATA` 为完整业务数据报。
异步请求只使用 `IPCPackagePb.header.track_id` 关联响应，不沿用旧版回调结构。

- `MSG_P2A_RSP_CREATE_SESSION`：Session 创建成功；
- `MSG_P2A_RSP_RUNTIME_DATA` / `MSG_P2A_NTF_RUNTIME_DATA`：Runtime 查询应答 / 主动通知；
- `MSG_P2A_NTF_RX_SHM_READY` / `MSG_P2A_NTF_RX_SHM_INVALIDATED`：RX 共享内存创建或失效；
- `MSG_P2A_RSP_CREATE_TX_SHM`：TX 创建成功，App 决定何时打开 SHM；
- `MSG_P2A_NTF_USB_HOST_STATE` / `MSG_P2A_NTF_COMM_PEER_STATE`：Host/Channel 状态；
- `MSG_P2A_DATA_COMM_CHANNEL`：Host Component 业务数据；
- `MSG_P2A_REQ_GET_APP_CONFIG` / `MSG_P2A_REQ_SET_APP_CONFIG`：独立的配置读取与更新请求；
- `MSG_P2A_RSP_CREATE_ENCODE_STREAM` / `MSG_P2A_NTF_ENCODE_FRAME_READY`：H.264 订阅结果与数据包通知；
- `MSG_P2A_RSP_STATUS` / `MSG_P2A_RSP_ERROR`：异步请求完成状态。

App 配置 GET 使用 `MSG_P2A_REQ_GET_APP_CONFIG` → `MSG_A2P_RSP_GET_APP_CONFIG`，
SET 使用 `MSG_P2A_REQ_SET_APP_CONFIG` → `MSG_A2P_RSP_SET_APP_CONFIG`；
SDK 负责构造并发送响应，Platform 负责经 SystemChannel 转发。

主动查询/读取接口包括：

```cpp
GetRuntimeData()
GetRxInfo()
IsUSBHostOnline()
IsCommChannelReady()
QueryStats(request_id)     // 异步：结果通过 MSG_P2A_RSP_QUERY_STATS 返回
```

H.264 通过 `SubscribeH264Stream(request_id)` 异步订阅公共流，收到 `MSG_P2A_RSP_CREATE_ENCODE_STREAM` 后调用 `OpenH264Stream(info)` 打开只读 SHM，再通过 `GetLatestH264PacketSeq()` / `ReadH264Packet()` 读取编码数据。编码参数由 Platform 统一决定，SDK 不设置公共 H.264 分辨率、帧率、码率或 GOP。

## 10. App Configuration

每个 `.kap` 都必须包含 `schema_description.json`；无配置项时使用 `{}`。
Device SDK 统一负责 Schema 校验、生成默认值、用户 JSON 持久化，以及上位机配置 GET/SET 响应。
SDK 启动时读取 `KAVIO_APP_SCHEMA_DESCRIPTION_PATH` 指定的 Schema，配置原子保存到
`KAVIO_APP_DATA_DIR/config.json`；已有用户配置非法时 `Start()` 返回错误，是否恢复默认值由 App 决定。

C++ App 获取和修改的是独立的 `Json::Value` 快照，不需要手工加载或保存文件：

```cpp
Json::Value current = client.GetAppConfig();
current["show_cpu"] = false;
auto status = client.UpdateAppConfig(current);
// 显式恢复默认值：client.ResetAppConfigToDefaults();
```

收到上位机完整 SET JSON 时，SDK 先校验、持久化并更新内存；只有成功的
`MSG_P2A_REQ_SET_APP_CONFIG` 才交付 `IClientMessageHandler::OnMessage()`，
业务代码此时通过 `GetAppConfig()` 读取最新配置并刷新业务状态。
GET 和失败的 SET 由 SDK 自动回复，不进入业务 `OnMessage()`；本地更新不主动通知 Host。

Python SDK 使用 `get_app_config()`、`update_app_config()` 和
`reset_app_config_to_defaults()`，同样不要求业务代码操作配置文件。

完整 Schema 格式、上位机同步链路参见
[`Kavio_App_Configuration_Spec.md`](../../docs/modules/app/Kavio_App_Configuration_Spec.md)，
使用示例参见 [`demo_communication`](../../kavio_app/demo_communication/README.md)。

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

Python SDK 以 Python 函数和 protobuf 作为公共接口。Session 协商在 `connect()` 中完成；之后的业务请求立即返回 `track_id`，结果统一由 `on_message(package)` 收取。连接关闭使用 `on_disconnected()` 通知。

```python
from kavio_sdk import IPCClient, ClientOptions, proto_py

proto = proto_py.ipc_pb2
client = IPCClient()

def on_message(package):
    if package.header.msg_type == proto.MSG_P2A_RSP_CREATE_TX_SHM:
        # Platform 已创建 TX；App 自主决定何时映射。
        client.open_tx_shm(package.payload.p2a_rsp_create_tx_shm.tx_shm)
    elif package.header.msg_type == proto.MSG_P2A_RSP_CREATE_ENCODE_STREAM:
        client.open_h264_stream(package.payload.p2a_rsp_create_encode_stream.stream)
    elif package.header.msg_type in (proto.MSG_P2A_RSP_STATUS, proto.MSG_P2A_RSP_ERROR):
        # header.track_id 与发起异步请求时返回的 track_id 对应。
        pass

session = proto.A2PReqCreateSessionPb(client_name="my_python_app")
session.rx_params.enabled = False
session.rx_params.frame_sample_interval = 1
session.tx_params.enabled = True
session.tx_params.z_order = 100
client.connect(ClientOptions(), session, on_message)
request_id = client.create_tx_shm(560, 220, proto.PIXEL_FORMAT_BGRA8888)
```

`ClientOptions.handshake_timeout_s` 只用于 Session 建立，**不用于业务请求**。业务请求不等待 Platform 完成，业务超时由 App 自己处理。发送请求返回的 `track_id` 与回复 `header.track_id` 对应。

SDK 内部有一个 IPC 接收线程和一个独立的消息派发线程。`on_message()` 串行执行；耗时处理会延迟后续消息，但不会直接阻塞 IPC 接收。队列有消息条数和字节数上限，溢出会使连接失效，不静默丢弃业务消息。

Python SHM 提供 `FrameShm`（RX/TX 双缓冲，使用 Linux `libatomic` 跨进程同步）以及 `EncodedStreamShm`（H.264 只读环形缓存）。App 收到 H.264 订阅成功回复后调用 `open_h264_stream(info)`，通过 `latest_h264_packet_seq(stream_id)` 和 `read_h264_packet(stream_id, seq)` 读取独立数据副本。已覆盖或尚未提交的包返回 `None`，解码应从完整 SPS/PPS/IDR 开始。编码分辨率、帧率和码率由 Platform 决定。

RX SHM 在收到 `MSG_P2A_NTF_RX_SHM_READY` 时由 SDK 自动打开；TX 和 H.264 SHM 由 App 显式打开。Linux Device 运行环境必须提供 `libatomic.so.1`。Python 3.10 及以上版本适用。

完整 Python App 参考：

```text
kavio_app/demo_metrics_panel
```

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
