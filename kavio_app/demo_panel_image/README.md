# demo_panel_image

## 1. 功能说明

`demo_panel_image` 展示 Host Component 自定义 App Panel、Windows 图片解码、USB Channel 大块数据传输和 Device TX Surface 联动。

运行后：

1. 用户把 PNG 或 GIF 拖入 Windows App Panel；
2. Host Component 使用 WIC 解码为 BGRA8888；
3. Host 按帧、按数据块通过 USB Channel 上传图片；
4. Device Component 完整接收后切换当前图片；
5. Device 根据图片尺寸创建 TX Surface，并保留 Alpha；
6. GIF 按帧延迟播放；
7. 用户在 App Panel 中拖动图片时，只发送新的 X/Y 坐标，不重复上传像素数据。

这个 Demo 不包含 App Configuration，适合作为 App Panel 和较大二进制数据传输的参考实现。

## 2. 构建 Device Component

### 环境要求

- CMake 3.16 或更高版本；
- 支持 C++17 的编译器；
- 仓库中存在 `kavio_sdk/device`。

### Release 构建

```bash
cd kavio_app/demo_panel_image/device
./build.sh -r
```

交叉编译：

```bash
./build.sh -r --toolchain /path/to/aarch64-toolchain.cmake
```

构建产物：

```text
kavio_app/demo_panel_image/output/device/demo_panel_image
```

清理：

```bash
./build.sh --clean
```

## 3. 构建 Host Component

使用 Visual Studio 2015 或更高版本打开：

```text
kavio_app/demo_panel_image/host/DemoAppPanelImage.sln
```

构建配置：

```text
Platform:      x64
Configuration: Debug 或 Release
Toolset:       v140
```

Host Component 使用 Windows WIC、Shell 和 GDI 相关系统库，不需要额外图片解码库。

编译完成后自动暂存：

```text
kavio_app/demo_panel_image/output/host/host.dll
```

## 4. 检查和打包

Device 和 Host 都构建完成后：

```bash
cd kavio_app
python3 kavio_app.py status demo_panel_image
python3 kavio_app.py package demo_panel_image
```

最终安装包：

```text
kavio_app/demo_panel_image/dist/demo_panel_image-1.0.0.kap
```

## 5. 目录结构

```text
demo_panel_image/
├── common/DemoImagePanelProtocol.h
├── device/
│   ├── CMakeLists.txt
│   ├── build.sh
│   └── main.cpp
├── host/
│   ├── DemoAppPanelImage.sln
│   ├── DemoAppPanelImageHost.vcxproj
│   └── HostComponent.cpp
└── package/
    ├── kavio_package.json
    └── icon.png
```

## 6. 图片能力和限制

Demo 对外支持 PNG 和 GIF。

当前限制：

```text
最大宽度：       4096
最大高度：       4096
最大帧数：       4096
解码后总数据：   128 MiB
单个数据块：     256 KiB
输出像素格式：   BGRA8888
```

PNG Alpha 会保留。GIF 的所有帧需要使用相同逻辑画布；帧延迟最小按 20 ms 处理。

设备端不负责 PNG/GIF 解码，只接收 Host 已经解码完成的 BGRA 帧。

## 7. USB Channel 传输流程

协议定义：

```text
common/DemoImagePanelProtocol.h
```

一次图片上传使用独立 `transfer_id`：

```text
ImageBegin
  -> FrameBegin(0)
  -> FrameChunk...
  -> FrameBegin(1)
  -> FrameChunk...
  -> ...
  -> ImageEnd
  -> Position
```

Device 不会边收边替换当前图片。只有 `ImageEnd` 到达并确认所有帧完整后，才把新图片切换为 active image。上传失败不会破坏之前正在显示的内容。

位置同步使用独立 `PositionMessage`，所以拖动图片不会重复发送像素数据。

## 8. DeviceReady 握手

USB Channel 可用后，Device Component 每秒发送一次 `DeviceReady`。

Host 只有同时满足：

```text
Host Component 已启动
USB Channel 可用
Device Component 已进入业务循环
```

才开始上传图片或发送位置更新。

这个业务层握手适合需要确保 Device 已经准备好接收大块数据的 App。

## 9. App Panel

Host Component 创建独立 Windows UI 线程处理 App Panel，主要功能包括：

- 文件拖放；
- PNG/GIF 预览；
- GIF 播放；
- 图片拖动；
- 位置同步。

拖放使用 `WM_DROPFILES`。如果 Host 进程权限高于 Explorer，代码会尝试允许相关 Windows 消息通过，以减少跨完整性级别拖放失败的情况。

## 10. 运行验证

1. 安装并启动 App；
2. 打开 App Panel；
3. 拖入带 Alpha 的 PNG，确认 Panel 和设备 TX 都正确显示；
4. 拖入 GIF，确认两端按帧播放；
5. 在 Panel 中拖动图片，确认设备 TX 位置同步；
6. 拖动过程中确认没有重新上传整张图片；
7. Device 未就绪时拖入文件，确认 Host 给出明确提示。

## 11. 常用修改入口

| 目标 | 文件 |
| --- | --- |
| 修改协议和传输上限 | `common/DemoImagePanelProtocol.h` |
| 修改 Windows 图片解码 | `host/HostComponent.cpp` 中的解码逻辑 |
| 修改 App Panel 交互 | `host/HostComponent.cpp` 中的窗口消息处理 |
| 修改 Device 分块接收 | `device/main.cpp` |
| 修改 TX 创建和位置 | `device/main.cpp` |
