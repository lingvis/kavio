# demo_rx_tx

## 1. 功能说明

`demo_rx_tx` 是最小的 Native Device Component 示例，只包含设备端代码，不需要 Windows Host Component。

运行后主要展示：

1. 启动 Kavio Device SDK 并等待 App 可用；
2. 启用 RX 并主动读取最新视频帧；
3. 查询一次运行统计信息；
4. 创建两个 TX Surface；
5. 基于 RX 画面生成两个动态 BGRA overlay；
6. 从终端读取键盘和方向键并发送 Kavio Input 事件；
7. Kavio 运行环境重新建立后自动恢复 RX/TX 资源；
8. 收到退出信号后清理资源并退出。

这个 Demo 适合作为第一次接入 Kavio C++ Device SDK 的起点。

## 2. 构建 Device Component

### 环境要求

- CMake 3.16 或更高版本；
- 支持 C++17 的编译器；
- 仓库中存在 `kavio_sdk/device`。

### Release 构建

```bash
cd kavio_app/demo_rx_tx/device
./build.sh -r
```

交叉编译：

```bash
./build.sh -r --toolchain /path/to/aarch64-toolchain.cmake
```

构建产物：

```text
kavio_app/demo_rx_tx/output/device/demo_rx_tx
```

清理：

```bash
./build.sh --clean
```

## 3. Host Component

本 Demo 不包含 Host Component，因此不需要 Windows 编译步骤。

## 4. 检查和打包

Device Component 构建完成后：

```bash
cd kavio_app
python3 kavio_app.py status demo_rx_tx
python3 kavio_app.py package demo_rx_tx
```

最终安装包：

```text
kavio_app/demo_rx_tx/dist/demo_rx_tx-1.0.0.kap
```

## 5. 目录结构

```text
demo_rx_tx/
├── device/
│   ├── CMakeLists.txt
│   ├── build.sh
│   └── main.cpp
└── package/
    ├── kavio_package.json
    ├── schema_description.json
    └── icon.png
```

该 Demo 没有 `host/`、`common/`、App Configuration 或 App Panel。

## 6. Device SDK 生命周期

`KavioClient::Start()` 启动 Device SDK 的连接与消息处理。App 在 `OnMessage()` 中收到 `MSG_P2A_RSP_CREATE_SESSION` 后，提交 TX 创建、RX 参数设置等异步请求。

Demo 的主流程为：

```text
Start
  -> WaitReady
  -> Setup
  -> Running
  -> Cleanup
  -> Exit
```

其中：

- `Setup`：启用 RX、查询 Stats、创建两个 TX Surface；
- `Running`：按固定间隔读取最新 RX 帧并更新 TX；
- 运行环境失效时释放本地状态并重新等待 ready；
- 恢复后重新创建需要的 RX/TX 资源。

业务代码不需要自行管理 Kavio 内部通信链路。

## 7. RX

默认轮询间隔：

```cpp
KAVIO_DEMO_RX_POLL_INTERVAL_MS = 33
```

即最多约每 33 ms 尝试读取一次最新 RX 帧。

`ReadLatestRxFrame()` 返回暂时没有新帧或视频尚未准备完成时，Demo 把它视为正常运行状态，不作为严重错误处理。

RX 处理入口主要位于：

```text
KavioDemoApp::OnRunningLoop()
```

## 8. TX

Demo 创建两个独立 TX Surface，并从同一 RX 帧生成两套动态 overlay，用于展示：

- 多 TX Surface；
- BGRA 像素写入；
- TX 位置与透明度；
- Kavio 运行环境恢复后的 Surface 重建。

主要 overlay 生成函数：

```text
MakeOverlay()
ProcessFrameAndOverlay()
```

可以直接替换为 OCR、目标检测结果、字幕、业务状态或其他图像处理输出。

## 9. Input

当 `stdin` 是 TTY 时，Demo 启动终端输入线程：

- 普通字符：发送对应键盘事件；
- 方向键：发送方向键事件；
- `Ctrl + 方向键`：按当前 RX 分辨率维护绝对鼠标位置并发送鼠标移动；
- `Ctrl + C`：退出。

如果 `stdin` 不是 TTY，只禁用终端输入，不影响 RX/TX 功能。

Input 相关代码集中在：

```text
TerminalInputGuard
SendKeyboardInput()
SendMouseInput()
```

## 10. 运行验证

1. 安装并启动 App；
2. 确认 RX 可用后 Demo 持续读取视频帧；
3. 确认设备输出两个 TX overlay；
4. 在终端输入按键，确认 Kavio Input 事件正常发送；
5. 暂时断开并恢复 Kavio 运行环境，确认 Demo 可以重新建立 RX/TX；
6. 使用 `Ctrl + C` 或 `SIGTERM` 退出，确认程序正常结束。

## 11. 常用修改入口

| 目标 | 文件/位置 |
| --- | --- |
| 修改 RX 轮询和帧处理 | `KavioDemoApp::OnRunningLoop()` |
| 修改 TX overlay | `MakeOverlay()`、`ProcessFrameAndOverlay()` |
| 修改 Input 映射 | `TerminalInputGuard` 和 Input 发送函数 |
| 修改默认 RX 轮询周期 | `KAVIO_DEMO_RX_POLL_INTERVAL_MS` |
