# Kavio App 系统总体设计

> 文档文件：`Kavio_App_System_Design.md`
> 文档状态：当前架构基线，用于协议设计、代码实现、Review 和验收
> 适用范围：`KavioWin（Kavio 上位机）`、设备端 `Platform（平台常驻程序）`、`AppService（应用服务）`、`Kavio App（Kavio 应用）`、可选 Host Component（主机组件）、双端 `SDK（软件开发工具包）`

> 当前运行实现、SystemChannel/Endpoint 细节和设备默认路径由 Platform 内部实现基线维护（不属于对外 SDK 文档）；本文保留公开的 App 系统设计、分阶段目标与验收约束。

---

## 0. 术语与最终架构模型

Kavio 对外统一使用 **Kavio App（Kavio 应用）** 作为安装、版本、签名、升级、卸载和身份管理的最小业务单元。普通用户只需要理解 App，不需要理解 DLL、宿主或进程边界。

一个 Kavio App 使用唯一 `app_id（应用标识）`，并可由以下部分组成：

```text
Kavio App
├── Device Component（设备组件）                 必选
│   └── 运行于 Kavio 设备，由 Platform / AppService 管理
├── Host Component（主机组件）                  可选
│   └── 运行于 Windows，由 Kavio App Host 加载
│       └── App Panel（应用面板）                可选
└── App Configuration（应用配置）               可选
    └── Config Definition（配置定义与字段初始值）
```

术语边界固定如下：

| 名称 | 含义 | 用户是否需要理解 |
|---|---|---:|
| `Kavio App` | 完整应用单元；安装、签名、版本、升级、卸载和 `app_id` 的边界 | 是 |
| `Device Component` | App 在 Kavio 设备上运行的组件 | 通常否 |
| `Host Component` | App 在 Windows 上运行的可选 DLL 组件 | 通常否 |
| `Kavio App Host` | 加载并隔离 Host Component 的 Windows 宿主进程 | 否 |
| `App Panel` | Host Component 可选提供的自定义交互面板 | 只看到面板本身 |
| `App Configuration` | App 的通用配置能力 | 是 |
| `Config Definition` | 描述配置字段、类型、范围、发布初始值和展示信息的声明文件 | 否 |
| `App Settings` | KavioWin 根据 Config Definition 生成的用户配置页面 | 是 |

关键约束：

1. `app_id` 属于整个 Kavio App，Device Component 与 Host Component 不各自拥有独立 ID。
2. Device Component 与 Host Component 生命周期相互独立；一方停止不自动停止另一方。
3. App Configuration 与 App Panel 相互独立。App Settings 走 SystemChannel，不依赖 Host Component 是否运行。
4. Config Definition 是“配置定义”，不是“配置实例集合”；当前架构不引入 Profile（配置档案）概念。
5. Host Component 的实现形式当前是 DLL，但产品和 SDK 术语统一使用 Host Component，不把 DLL 实现形式暴露为产品概念。

## 1. 设计背景与系统定位

Kavio 已经打通设备与上位机之间的 `USB（通用串行总线）` 多通道通信，并具备Device Component（设备组件） 的安装、启动、停止、查询和状态上报协议基础。

系统采用以下业务模型：

> **Device Component（设备组件） 是主要业务运行单元；Host Component（主机组件）是可选的主机侧辅助组件。**

主要业务逻辑运行在Device Component（设备组件） 中。Host Component（主机组件）按需提供：

- App Settings（应用设置）页面；
- App Panel 自定义界面；
- 主机网络访问代理；
- 主机本地文件访问；
- 主机系统接口；
- 可视化交互、日志和调试界面；
- 设备侧不便直接实现的其他主机能力。

Device Component（设备组件） 与 Host Component（主机组件）是两个独立运行单元：

- 可以分别启动；
- 可以分别停止；
- 任一方停止不会自动停止另一方；
- USB 断开不会触发Device Component（设备组件） 停止；
- 是否随 Platform 启动只由 `auto_start（自动启动）` 决定；
- 当前不提供 App 与 Host Component（主机组件）的生命周期关联字段。

应用包可以只包含Device Component（设备组件），也可以同时包含Device Component（设备组件） 和上位机 `DLL（动态链接库）`。

Device Component（设备组件） 支持两种运行类型：

- `native（原生程序）`：编译后的 ARM 可执行程序；
- `python（Python 脚本程序）`：Python 源码及依赖描述，或可恢复的离线环境归档。

---

## 2. 第一版目标（阶段验收记录）

第一版应完成以下闭环：

1. 使用统一应用包安装Device Component（设备组件） 和可选 Host Component（主机组件）。
2. 支持只包含Device Component（设备组件）、不包含Host Component DLL 的应用包。
3. 支持原生程序和 Python 程序两种设备运行类型。
4. 由 AppService 统一负责：
   - 安装和覆盖更新；
   - 卸载；
   - Python 环境创建；
   - 部署；
   - 启动、停止和强制结束；
   - 进程退出监控；
   - 运行状态查询；
   - App 与上位机之间通信身份绑定；
   - 安装、部署和运行诊断日志采集及上报。
5. 支持Device Component（设备组件） 在 Platform 启动后自动启动一次，但不做异常退出后的守护式保活。
6. 支持Device Component（设备组件） 和 Host Component（主机组件）分别启动、停止，也支持管理界面的“全部启动”和“全部停止”。
7. 支持不含Host Component DLL 的 App 由 KavioWin 通用管理窗口启动、停止、查询和配置。
8. 支持 Python App 使用与 `app_id（应用唯一标识）` 对应的独立虚拟环境。
9. Python 环境可以通过在线依赖安装创建，也可以从安装包内的离线环境归档恢复。
10. Python 环境创建失败即视为整个应用安装失败。
11. 官方签名包在正常模式下安装和运行。
12. 未签名包只能在对应一侧开启开发模式后安装和运行。
13. 未签名Host Component DLL 和未签名Device Component（设备组件） 均允许在开发模式下使用。
14. 关闭某一侧开发模式时，该侧已经安装的未签名代码立即失去运行资格。
15. 一个Host Component（主机组件）对应一个独立 `KavioAppHost（Kavio 应用宿主）` 进程。
16. KavioWin 创建一个统一 `Named Pipe（命名管道）` 服务端，每个 Kavio App Host 使用一条独立连接。
17. 每个Device Component（设备组件） 只能使用与自身 app_id 对应的通信 `Channel（通道）`。

---

## 3. 第一版非目标（阶段验收记录）

第一版暂不实现：

- 在线应用商店和自动更新；
- 外部开发者公钥注册、吊销和完整证书体系；
- 多版本并存和历史版本回滚；
- Windows 完整沙箱；
- App 异常退出后的自动重启；
- App 与 Host Component（主机组件）的自动关联启动或关联停止；
- App 到 App 的通用服务发现和跨 App 调用；
- 高频大数据共享内存通道；
- 菜单启动界面的具体修改；
- 配置版本号和差异配置合并；
- AppService 代替业务 App 保存业务配置；
- App 子进程的独立 IPC 身份授权；
- Python 环境作为独立界面状态展示；
- 未签名应用在正常模式下运行。

---

## 4. 核心设计原则

1. Device Component（设备组件） 是主要业务主体，Host Component（主机组件）是可选辅助组件。
2. Platform 是Device Component（设备组件） 的唯一安装和进程管理者。
3. KavioWin 是 Windows 侧唯一的 USB 通信所有者。
4. Device Component（设备组件） 和 Host Component（主机组件）都不得直接访问 USB 设备。
5. Device Component（设备组件） 只能通过设备 SDK 与 Platform 通信。
6. Host Component（主机组件）只能通过 Kavio App Host SDK 与 KavioWin 通信。
7. app_id 同时作为安装身份、App 身份、业务 Channel 名称和状态查询身份。
8. App IPC 身份使用 Linux 内核返回的真实 PID 验证，不信任客户端声明的 PID。
9. AppService 不理解、不解析、不保存业务配置；业务配置由各 App 自行持久化。
10. 配置交互每次发送完整 `JSON（数据交换格式）`，不发送差异字段，不使用配置版本号。
11. `auto_start（自动启动）` 是当前唯一的 App 自动生命周期策略。
12. Host Component（主机组件）和Device Component（设备组件） 生命周期相互独立。
13. USB 或 Channel 不可用只改变通信状态，不自动改变 App 运行状态。
14. Python App 每个 app_id 使用一个独立虚拟环境，不与其他 App 共用可写环境。
15. Python 环境创建属于安装事务，环境创建失败即安装失败。
16. AppService 必须提供可查询、可上报的安装、环境、部署和运行诊断日志。
17. 官方签名是正常模式下的唯一代码信任路径。
18. 未签名代码只能在对应一侧开发模式开启时安装和运行。
19. 开发模式是运行时强制门槛，不只是安装时风险提示。
20. 内置 App 与用户安装 App 根据加载目录和安装来源区分，不能由安装包自行声明为内置 App。
21. Platform 核心能力必须保留在 Platform 内部，只有可独立失败和升级的业务功能才做成 App。

---

## 5. 总体架构

```text
KavioWin（Kavio 上位机）
├── PackageInstallService（应用包安装服务）
├── AppManager（应用管理器）
├── HostComponentRuntimeService（Host Component（主机组件）运行服务）
├── HostDeveloperModeService（上位机开发模式服务）
├── DeviceDeveloperModeProxy（设备开发模式代理）
├── AppControlProxy（Device Component（设备组件） 控制代理）
├── GenericConfigUI（通用声明式配置界面）
├── AppIpcServer（统一命名管道服务端）
├── AppChannelRouter（应用原始数据报路由器）
├── SystemChannelClient（系统通道客户端）
│   ├── DeviceSessionStateCallback（设备会话状态回调）
│   └── AppProtocolCallback（应用管理协议回调）
├── ChannelRegistry（业务通道注册表）
└── UsbService（USB 通信服务）
        │
        │ USB 系统协议、App 管理协议和业务 Channel
        ▼
Platform（设备平台程序）
├── AppService（应用服务）
│   ├── AppRegistry（应用注册表）
│   ├── AppInstaller（应用安装器）
│   ├── AppRuntimeManager（应用运行管理器）
│   ├── PythonEnvironmentManager（Python 环境管理器）
│   ├── AppProcessMonitor（应用进程监控器）
│   └── DeviceDeveloperModeManager（设备开发模式管理器）
├── IPCServer（本地进程间通信服务端）
├── AppMessageRouter（应用消息路由器）
└── UsbBridge（USB 桥接）
        │
        ▼
Device Component（设备组件）（主要业务）
```

Host Component（主机组件）运行结构：

```text
KavioWin.exe
    │
    │ 一个统一 Named Pipe 服务端
    │ 每个应用宿主一条独立连接
    ▼
KavioAppHost.exe
    └── host.dll（可选）
```

设备业务通信链路：

```text
Device Component（设备组件）
    ↔ Platform IPCServer
    ↔ Platform USB Bridge
    ↔ USB
    ↔ KavioWin UsbService
    ↔ AppChannelRouter
    ↔ 可选 Kavio App Host
```

KavioWin、Platform 和 Kavio App Host 仅按 `app_id（应用标识）` 路由完整原始数据报，不解析、增加、删除或转换数据报内部字段。通用声明式配置界面使用 SystemChannel，不接入应用业务 Channel。

业务 Channel 使用稳定的 `channel_id（通道标识）` 作为唯一 USB 路由标识。`channel_id` 由 KavioWin 和 Platform 使用同一算法根据规范化 `app_id（应用标识）` 独立计算，安装时必须校验不同 App 不得产生相同 ChannelID。设备端 IPC `session_id（会话标识）` 只用于 Platform 本地识别当前有效 IPC 会话，不进入 USB 帧，也不参与跨端寻址。

---

## 5.1 代码仓库目录设计

代码仓库必须区分“平台框架”“公共 SDK（软件开发工具包）”和“外部业务项目”三类边界：

```text
project_root/
├── kavio_platform/          # 设备端平台框架
├── kavio_win/               # Windows 上位机框架
├── kavio_sdk/               # 双端公共协议和公开 SDK
├── kavio_app/               # Kavio App Demo、业务项目与打包工具
└── docs/                    # 设计、协议、构建和打包文档
```

各目录职责如下：

| 目录 | 所有者 | 主要职责 | 禁止内容 |
|---|---|---|---|
| `kavio_platform` | Kavio 平台开发者 | 设备常驻框架、AppService、IPC、SHM（共享内存）、设备运行管理 | 外部业务 App、外部应用源码 |
| `kavio_win` | Kavio 上位机开发者 | USB 通信、应用管理、Kavio App Host、通用配置界面 | 某个业务项目专用应用源码 |
| `kavio_sdk` | Kavio SDK 维护者 | 公开 C++、Python、Host Component C ABI（主机组件 C 应用二进制接口）、共享协议 | Platform 或 KavioWin 私有业务实现 |
| `kavio_app` | 外部开发者及示例维护者 | 按 `appId（应用标识）` 组织Device Component（设备组件）、可选 Host Component、配置和打包清单 | 平台核心服务实现、复制版 SDK |
| `docs` | 项目维护者 | 架构基线、开发约束、构建和打包说明 | 构建产物和运行数据 |

`kavio_platform` 和 `kavio_win` 可以引用 `kavio_sdk` 的公共状态码、协议和接口定义，但这种引用不改变目录所有权：SDK 是公共契约，不是两个框架的公共杂项目录。只有外部开发者确实需要、且能够形成稳定兼容契约的内容才能进入 SDK。

## 5.2 外部业务项目目录设计

`kavio_app` 以完整 Kavio App 为一级目录边界，并在 App 内部明确区分 Device Component、Host Component、静态打包定义、构建产物和最终发行包：

```text
kavio_app/<project_name>/
├── README.md                    # 项目功能、通信契约和验证说明
├── device/                     # 必选，ARM Device Component 工程
│   ├── build.sh                # 只构建本项目设备端程序
│   ├── CMakeLists.txt
│   ├── src/ 或 main.cpp
│   └── build/                  # 本地构建目录，不提交版本库
├── host/                       # 可选，Windows Host Component 工程
│   ├── <project>.sln
│   ├── <app>.vcxproj
│   └── 应用源码
├── package/                    # 必选，只保存打包清单和静态资源
│   ├── kavio_package.json      # 项目唯一打包配置
│   ├── schema_description.json # 必需；无配置项时为 {}
│   └── icon.png                # App 图标（可另附静态资源）
├── output/                     # 双端构建产物，不提交版本库
│   ├── device/
│   └── host/
└── dist/                       # 最终 .kap 安装包
```

该结构的设计初衷是：

1. **一个项目对应一个 app_id**：目录、通信身份、安装身份、升级边界和版本保持一致。
2. **业务内聚、平台隔离**：Device Component 与 Host Component 位于同一业务项目，但各自的源码、构建脚本和工程文件不会混放。
3. **构建入口归属明确**：Linux/ARM 使用的 `build.sh` 位于 `device/`；Windows Host Component 使用 `host/` 中的 `.sln`、`.vcxproj`，由 Visual Studio 2015 或更高版本直接编译。
4. **静态定义与二进制分离**：`package/` 只维护清单、配置和图标；所有编译产物进入 `output/`，避免版本库中的静态文件与生成文件混杂。
5. **打包与编译解耦**：两端分别在各自环境中编译，`kavio_app.py` 只检查产物并临时合并 `package/ + output/`，不假装用一个脚本跨系统完成全部编译。
6. **发行边界清楚**：每个项目自己的 `dist/` 保存最终安装包，避免多个业务项目共享一个全局发行目录。
7. **Host Component 保持可选**：纯Device Component（设备组件） 省略 `host/` 和清单中的 `host` 字段，其余规则不变。
8. **示例即模板**：外部开发者复制一个完整项目目录即可开始开发，不需要从多个顶层目录手工拼装。

统一规则：

- `device/build.sh` 只构建当前项目 Device Component，并将主程序放入 `<project>/output/device/`；
- `host/*.sln` 只包含当前项目 Host Component；使用 Visual Studio 2015 或更高版本编译后，Post-Build Event 将主入口统一放入 `<project>/output/host/host.dll`；
- `package/kavio_package.json` 中的 `device.directory` 和 `host.directory` 分别指向最终安装包中的 `device` 和 `host` 目录；
- `kavio_app/kavio_app.py status` 分别检查 `package/` 中的静态文件和 `output/` 中的运行产物；
- `kavio_app/kavio_app.py package` 使用临时目录合并二者并生成 `<project>/dist/*.kap`；
- 不允许在 App 目录之外建立另一套平行业务源码树，也不允许维护项目私有 SDK 副本。

## 6. Channel 身份与组件 Endpoint 生命周期解耦

业务 Channel 是由 `app_id` 决定的稳定逻辑身份，不属于 Kavio App Host、Device Component 或某一次 USB 会话。Host Component 和 Device Component 是该 Channel 两端当前实际在线的 `Endpoint（端点）`。

正确设计是：

> **ChannelID 稳定存在，Endpoint 独立上线和下线；业务可通信状态由传输状态与对端 Endpoint 状态共同决定。**

原因：

- 纯 Device Component（设备组件）没有 Host Component 时仍可独立运行；
- Host Component 和 Device Component 可以任意先启动，任意一端上线后都能立即获得自己的 ChannelID；
- 任一组件停止只改变对端可通信状态，不改变 ChannelID；
- USB 重连只需要重新同步 Endpoint 事实状态，不需要重新分配 ChannelID。

### 6.1 ChannelID 生成与冲突规则

双端对每个 App 使用完全相同的确定性算法：

```text
app_id
→ 规范化/合法性校验
→ StableHash
→ ChannelID
```

- 稳定哈希使用共享 `USBFrame::HashChannelName()` 的 FNV-1a 32-bit 实现；业务 App 的规范 `app_id` 本身已限定为小写 `[a-z0-9.-]`；哈希结果若命中传输保留 ID，则按公共规则确定性重映射；
- KavioWin 和 Platform 均直接根据 `app_id` 独立计算 ChannelID，计算结果不依赖组件启动顺序或对端状态；
- 安装新 App 时必须计算其 ChannelID，并与所有已安装的其他 `app_id` 以及内置业务 Channel 保留 ID 比较；若发生相同 ChannelID，直接拒绝安装；
- 运行时仍保留防御性一致性检查，发现 `ChannelID -> 不同 app_id` 时拒绝绑定并记录严重错误；
- Host Component 由 KavioWin 的单实例运行时和一次性启动身份保证唯一；Device Component 若异常建立多个受管 IPC 会话，以最后登记的当前会话为有效 Endpoint，旧会话后续退出不得清除新会话状态。

### 6.2 Endpoint 状态同步

SystemChannel 使用独立状态消息同步两端当前业务组件：

```text
HostEndpointSnapshot / HostEndpointOnline / HostEndpointOffline
DeviceEndpointSnapshot / DeviceEndpointOnline / DeviceEndpointOffline
```

完整 Snapshot（快照）是权威状态：USB 进入可通信状态后双方各发送一次当前本地 Endpoint Snapshot；之后使用 Online/Offline 增量维护。每条 Endpoint 记录同时携带 `app_id + channel_id`，用于一致性校验和诊断。

Kavio App Host 启停规则：

- Host Component 启动：发布对应 Host Endpoint Online；
- Host Component 停止：发布对应 Host Endpoint Offline；
- Host Component 停止不会改变 ChannelID，也不会停止 Device Component；
- Device Component 的 IPC 会话上线/下线由 Platform 对称发布 Device Endpoint Online/Offline；
- 无对端 Endpoint 时业务原始数据报不缓存，发送直接返回通信不可用。

### 6.3 USB 断开与恢复

USB 断开：

- Device Component（设备组件）保持运行；
- App 与 Platform 的本地 IPC 会话保持；
- 双端清除对端 Endpoint 在线状态，但保留本地稳定 ChannelID；
- App/Host Component 的 `channel_ready` 变为 false；
- Platform 和 KavioWin 均不缓存应用通道原始数据报。

USB 恢复：

- 双端重新进入可通信状态后交换完整 Endpoint Snapshot；Platform 在离线后重新收到 Host 数据时主动发送 Device Endpoint Snapshot，KavioWin 收到 Device Snapshot 后回送当前 Host Endpoint Snapshot，因此心跳超时恢复也能重新收敛状态；
- 对端 Endpoint 状态按 Snapshot 恢复；
- ChannelID 不重新分配，原组件进程不需要因 USB 重连而重启。

### 6.4 原始数据报契约

应用业务 Channel 只承载不透明原始数据报：

- 一次发送对应对端一次接收回调，不合并、不拆分；
- 同一 app_id、同一方向严格保持先进先出；
- 单条数据报大小为 1 字节至 1 MiB；
- Channel 不可用时立即返回未就绪，不做离线缓存或自动重发；
- 发送成功只表示数据报已进入当前进程的有界发送队列，不表示Device Component（设备组件） 已接收或完成处理；
- 队列已满时返回独立错误，由应用自行决定限流、丢弃或重试；
- 请求、响应、通知、消息编号、序列号、校验、压缩、加密和可靠确认均由 Host Component 与Device Component（设备组件） 自行定义。

---

## 7. 应用包模型

建议安装包扩展名继续使用：

```text
*.kap
```

系统内部对象统一称为“应用包”，避免形成“必须包含Host Component（主机组件）”的误解。

### 7.1 必需内容

KavioWin 只扫描和加载包含有效 `manifest.json（应用清单）` 的应用目录。

- `manifest.json（应用清单）`；
- 图标和描述资源；
- Device Component（设备组件） 运行文件；
- 文件摘要列表。

### 7.2 可选内容

- `signature.json（数字签名）`；
- 上位机 `host/host.dll`；
- 声明式配置结构；
- Config Definition 字段初始值；
- Python 依赖描述；
- Python 完整离线环境归档；
- Python 本地依赖包目录；
- 其他 App 资源。

### 7.3 包类型

#### 双端应用包

```text
manifest.json
signature.json                 # 官方包存在；开发包可以省略
assets/
host/host.dll
schema_description.json
device/app 或 device/python_app/
```

#### 纯设备应用包

```text
manifest.json
signature.json                 # 官方包存在；开发包可以省略
assets/
schema_description.json
device/app 或 device/python_app/
```

`schema_description.json` 是包根目录必备文件，不属于 `host/`。KavioWin 读取 Config Definition 并生成配置界面，
完整配置通过 SystemChannel 交给Device Component（设备组件）；应用包没有 Host Component 时仍可使用 Config Definition。
纯设备应用包仍可通过 KavioWin 完成安装、启动、停止、查询、配置和卸载。

---

## 8. manifest.json（应用清单）

`manifest.json` 不再由打包工具手工拼接。唯一结构定义为 `kavio_sdk/device/proto/app_manifest.proto`，打包工具直接构造 `AppManifestPb` 并通过 protobuf JSON 序列化生成文件。JSON 字段名因此使用 protobuf JSON 默认的 lowerCamelCase（小驼峰）形式，例如 `manifest_version -> manifestVersion`、`app_id -> appId`、`auto_start -> autoStart`。

`manifest_version` 当前从 `KavioSdkVersion.h` 的 `KAVIO_MANIFEST_VERSION` 读取，现行值为 `1`。

### 8.1 原生 App 示例

```json
{
  "manifestVersion": 1,
  "appId": "com.kavio.example",
  "version": "1.0.0",
  "name": "Example App",
  "description": "Example device application",
  "icon": "assets/icon.png",

  "device": {
    "runtime": "native",
    "entry": "device/app",
    "arguments": [],
    "autoStart": false
  },

  "host": {
    "binary": "host/host.dll",
    "panel": false
  },

  "files": []
}
```

没有Host Component（主机组件）时，省略整个 `host` 对象。

`icon（应用图标）` 在当前公开打包工具中必填，必须指向包内有效的 **64×64 PNG** 文件。

`host` 只声明宿主 DLL 入口和是否提供 Panel，不保存独立 Host Component API 版本，也不声明日志开关。Host Component 不单独做 SDK 版本校验，公开 C ABI 通过 `struct_size` 做结构边界保护。KavioWin 启动任何 Host 应用时都会创建
`%APPDATA%\Kavio\log\<app_id>` 独立日志目录，并通过 `--log-dir` 传给 KavioAppHost。Host 应用日志不再提供全局启停开关。

### 8.2 Python App 示例

```json
{
  "manifestVersion": 1,
  "appId": "com.kavio.python-task",
  "version": "1.0.0",
  "name": "Python Task",
  "description": "Python device task",
  "icon": "assets/icon.png",

  "device": {
    "runtime": "python",
    "entry": "device/python_app/main.py",
    "arguments": [],
    "autoStart": false,
    "python": {
      "pythonVersion": "3.11",
      "installMode": "auto",
      "environmentArchive": "device/python_env.tar.zst",
      "environmentDefinition": "device/environment.yml",
      "requirements": "device/requirements.txt"
    }
  },

  "files": []
}
```

`environment_archive`、`environment_definition` 和 `requirements` 均为可选字段，但 Python App 必须至少提供一种可创建环境的输入。

### 8.3 Python 安装模式

| 值 | 含义 |
|---|---|
| `archive` | 必须从完整离线环境归档恢复 |
| `online` | 必须根据依赖描述在线创建环境 |
| `auto` | 有环境归档时优先恢复，否则在线创建 |

### 8.4 auto_start

`device.auto_start` 表示 Platform 启动后，AppService 是否自动启动该 App 一次。

规则：

- 该值是安装清单提供的初始默认值；
- 用户可以在管理窗口修改；
- AppRegistry 保存当前用户设置；
- 覆盖更新默认保留用户已经修改过的设置；
- App 异常退出后不自动拉起；
- USB 是否在线不影响 auto_start；
- Host Component（主机组件）是否运行不影响 auto_start。

### 8.5 不允许由清单声明的字段

以下内容只能由 Platform、KavioWin 或安装来源决定：

- App 是否属于系统内置；
- 安装根目录；
- 运行用户；
- 上位机开发模式；
- 设备开发模式；
- 是否允许未签名代码运行；
- 是否允许覆盖系统 App；
- Platform 核心权限；
- Python 软件源地址；
- 最终信任状态。

---

## 9. 生命周期设计

### 9.1 App 生命周期

Device Component（设备组件） 支持：

- 手动启动；
- 手动停止；
- Platform 启动后的 auto_start；
- 安装或更新前受控停止；
- Platform 退出时统一停止；
- 进程异常退出监控。

以下事件不停止Device Component（设备组件）：

- USB 断开；
- Channel 不可用；
- Host Component（主机组件）停止；
- Kavio App Host 异常退出；
- KavioWin 退出。

### 9.2 Host Component（主机组件）生命周期

Host Component（主机组件）支持：

- 手动启动；
- 手动停止；
- 安装或更新前受控停止；
- KavioWin 退出时由 Job Object 终止；
- Kavio App Host 异常退出监控。

Host Component（主机组件）启停不改变Device Component（设备组件） 运行状态。

### 9.3 组合操作

应用管理窗口可提供：

- 启动Device Component（设备组件）；
- 停止Device Component（设备组件）；
- 启动Host Component（主机组件）；
- 停止Host Component（主机组件）；
- 全部启动；
- 全部停止。

“全部启动”只是依次发起两个独立启动操作，不表示建立生命周期关联。

“全部停止”只是依次停止两个独立运行单元，不改变未来的独立启动规则。

### 9.4 Platform 启动顺序

```text
Platform 基础组件启动
→ IPCServer 启动
→ AppService 加载 AppRegistry
→ AppService 检查设备开发模式和信任状态
→ AppService 对具备运行资格的 auto_start App 启动一次
→ USB 服务启动
```

自动启动不依赖 USB 和 Host Component（主机组件）。

### 9.5 Platform 停止顺序

```text
AppService 停止全部Device Component（设备组件）
→ 等待 stop_timeout_ms
→ 强制结束未退出进程
→ IPCServer 停止
→ Platform 其他组件停止
```

---

## 10. 状态模型

### 10.1 安装状态

```text
未安装
接收安装包
校验安装包
创建 Python 环境
部署文件
注册应用
已安装
安装失败
文件损坏
签名无效
开发模式未开启
依赖环境创建失败
```

Python 环境不单独展示状态。环境创建属于安装过程：

- 环境成功创建后，应用才进入“已安装”；
- 环境创建失败，应用直接进入“安装失败”；
- 启动时发现环境缺失或损坏，应用状态转为“文件损坏”或“安装无效”。

### 10.2 Device Component（设备组件） 状态

```text
不存在
已停止
启动中
运行中
停止中
异常退出
启动失败
开发模式关闭，禁止运行
```

### 10.3 Host Component（主机组件）状态

```text
无上位机组件
已停止
启动中
运行中
停止中
异常退出
加载失败
开发模式关闭，禁止运行
```

### 10.4 通信状态

```text
USB 离线
USB 在线但 Channel 未发布
Channel 可用
```

通信状态独立于Device Component（设备组件） 和 Host Component（主机组件）运行状态。

### 10.5 信任状态

```text
Kavio 官方签名
签名未验证
签名无效
```

### 10.6 运行授权状态

```text
允许运行
需要上位机开发模式
需要设备开发模式
需要双端开发模式
开发模式已关闭，运行被阻止
```

### 10.7 界面展示示例

```text
Device Component（设备组件）：运行中
Host Component（主机组件）：已停止
通信：USB 离线
信任：Kavio 官方签名
```

上述组合属于正常状态。

未签名双端应用示例：

```text
Device Component（设备组件）：已停止
Host Component（主机组件）：已停止
信任：签名未验证
运行授权：需要双端开发模式
```

---

## 11. App 启动身份与 IPC 安全绑定

### 11.1 身份类型

#### app_id

稳定身份，用于安装、通信和状态管理。

#### runtime_instance_id（运行实例标识）

每次启动时由 AppService 本地生成，用于状态事件、日志和旧实例隔离。

#### PID（进程编号）

由 Linux 内核分配，是本地 IPC 权限验证依据。

### 11.2 SO_PEERCRED（套接字对端凭据）

Device Component（设备组件） 连接 Unix Domain Socket（Unix 域套接字）后，IPCServer 使用：

```cpp
getsockopt(fd, SOL_SOCKET, SO_PEERCRED, ...)
```

获取真实 PID、UID 和 GID。

绑定过程：

```text
真实 PID
→ AppService 查询当前运行记录
→ app_id
→ runtime_instance_id
→ IPC session
```

客户端请求中的 `client_pid` 只用于日志，不参与权限判断。

### 11.3 Channel 隔离

App SDK 不允许传入任意 channel_id，接口为：

```cpp
SendCommChannelData(const void* data, size_t size);
```

该接口发送一条完整原始数据报。Platform 根据真实 PID 找到 `app_id`，使用公共稳定哈希算法计算该 App 的 ChannelID，并只允许该 IPC 会话发送自己的业务 Channel。

ChannelID 不随 USB 重连或组件重启变化。SDK 可以只读查询自己的 ChannelID，但不能指定任意 ChannelID 发送数据。

Host 向 Device Component（设备组件）返回业务数据时，Platform 根据 USB 帧中的 `channel_id` 查找该 App 当前有效的本地 IPC 会话。`session_id` 仅用于 Platform 内部确认当前会话和过滤旧会话退出事件，不跨 USB 传输。目标 App 不在线、ChannelID 与 `app_id` 计算结果不一致或对端状态异常时均拒绝投递。

### 11.4 广播

广播只预留为未来 Platform 向Device Component（设备组件） 的系统广播能力。

第一版：

- 不支持 App 向其他 App 广播；
- 不支持上位机业务应用向所有 App 广播；
- 应用通道原始数据报必须按 app_id 隔离。

---

## 12. 配置系统

### 12.1 配置所有权

> **业务配置由 Device SDK 统一读取、校验和原子持久化；Device Component 负责把生效值应用到业务逻辑。AppService 不持久化业务配置。**

Platform 只承担 SystemChannel 到受管 App IPC 会话的可信路由：

- 不解析配置字段；
- 不保存或迁移配置 JSON；
- 不以安装包默认值冒充设备当前配置；
- 根据真实进程 PID 对应的 `app_id` 校验响应身份。

### 12.2 Config Definition 与 App Panel

Config Definition 和 App Panel 是两个独立的可选能力，不再使用 `none/native/config/hybrid` 配置模式组合。

| 能力 | 提供方式 | 主要用途 |
|---|---|---|
| Config Definition | 包根目录固定 `schema_description.json`，`{}` 表示无配置 | KavioWin 自动生成标准参数页面 |
| App Panel | Host Component 清单声明 `host.panel=true`，并导出面板接口 | 应用自定义数据展示、控制、文件操作和其他交互 |

约束：

- `schema_description.json` 必须存在；内容为 `{}` 是合法的无配置状态，不显示 App Settings 页面；
- 文件缺失或内容非法时打包失败；SDK 运行时加载 Schema 失败时启动失败，不以空配置继续运行；
- App Panel 不读取或修改 App Configuration，除非业务项目自行通过 Host Component / Device Component 协议实现相应功能；
- 没有 Host Component 的应用包不能提供 App Panel，但仍可单独提供 Config Definition。

### 12.3 Config Definition 约束

Config Definition 必须提供配置项 `key`、配置名称 `name`、配置类别 `category`、配置说明 `description` 和配置类型 `type`。当前支持 `boolean`、`integer`、`number`、`string`、`enum`。

完整现行规范见：[`Kavio_App_Configuration_Spec.md`](Kavio_App_Configuration_Spec.md)。Config Definition 每个字段必须提供 `value` 作为包内初始值。

### 12.4 App Configuration通信协议

App Configuration仅由 KavioWin 通用配置界面、SystemChannel、Platform 和Device Component（设备组件） 参与。Kavio App Host 和 Host Component 不接收 App Configuration、不提交 App Configuration，也不能通过宿主接口读取或修改 App Configuration。固定链路为：

```text
KavioWin GenericConfigUI
→ SystemChannel GetAppConfigRequestPb / SetAppConfigRequestPb
→ Platform 按 app_id 定向路由
→ App IPC `P2AReqGetAppConfigPb` 或 `P2AReqSetAppConfigPb`
→ Device SDK 读取或保存配置
→ 原路返回 GetAppConfigResultPb / SetAppConfigResultPb（错误使用 RspPackagePb 状态字段）
```

SystemChannel GET 使用字段号 340，SET 使用字段号 341；App IPC 使用 GET 75/76、SET 77/78 四种独立消息，并以 `IPCPackagePb.header.track_id` 关联请求与回复。Platform 保存待处理请求号并校验 App 真实身份、操作类型和响应对应关系。KavioWin 的系统协议层以统一应用管理协议回调向 `AppManager` 交付完整 `RspPackagePb`，由管理层根据 `body_case` 分发，不为每一种应用响应单独定义回调入口。

App Panel 不使用上述 App Configuration 配置链路。面板如需读取或修改Device Component（设备组件） 的业务数据，应通过 Host Component 与Device Component（设备组件） 自行定义的原始数据报协议完成，Kavio 框架不解析该协议。

### 12.5 配置读取流程

1. KavioWin 选择应用并发送 GET，请求体不携带配置；
2. Platform 将请求定向发送到相同 `app_id` 的运行中 App；
3. Device SDK 读取内存中的当前配置快照（首次运行已加载和校验）；
4. Device SDK 返回 Schema 与完整配置 JSON；
5. KavioWin 按 Config Definition 再次校验并更新可视化控件。

### 12.6 配置保存流程

1. KavioWin 从动态控件生成并校验完整配置 JSON；
2. 通过 SystemChannel 发送 SET；
3. Device SDK 对完整 JSON 执行 Schema 校验；
4. Device SDK 原子保存并更新内存配置，随后通知 App 应用业务参数；
5. Device SDK 返回实际持久化的完整配置；
6. KavioWin 仅在协议层和 App 业务层均成功时更新 App Settings（应用设置）界面缓存。

约束：

- 每次传输完整配置，不发送差异字段；
- 配置 JSON 最大 1 MiB；
- Config Definition 加载异常在应用管理页显示短句，格式为“问题，请检查项目”；实际文件路径和 JSON 解析细节写入日志；
- Config Definition 最大 256 KiB，最多 128 个字段；
- 配置 GET/SET 由 SDK 管理，不需要 App 手动注册 GET/SET 回调；
- App IPC 关闭立即失败；
- 15 秒未响应返回超时；
- App 未运行时读取和保存不可用。

### 12.7 Config Definition 初始化与配置恢复

打包工具要求同级 `schema_description.json` 文件必须存在，且仅 `{}` 表示无配置；
非空内容必须通过 Config Definition v2.0 校验。Manifest 不再包含 SchemaConfig 字段。
Platform 仅注入 `KAVIO_APP_SCHEMA_DESCRIPTION_PATH` 和 `KAVIO_APP_DATA_DIR`。
Device SDK 使用 JsonCpp 解析 Schema，首次运行自动创建 `config.json`；
已有文件不合法时 `Start()` 返回错误，是否调用 `ResetAppConfigToDefaults()` 由 App 决定。
SDK 自动处理 Host GET/SET 和持久化，成功 SET 经 OnMessage 通知 App；上位机通过 GET 主动获取 Schema/当前配置，经
IPC → Platform SystemChannel 发送给上位机。Platform 不解析业务 JSON。

## 13. Python 运行环境设计

### 13.1 总体方案

设备镜像预装 `Miniforge（精简 Conda 发行版）`，并保证 `conda` 已加入 Platform 进程的 `PATH（环境变量搜索路径）`。AppService 直接执行 `conda`，不保存或拼接 Miniforge 绝对路径。

每个用户安装 App 使用独立实例目录，Python 环境属于 App Runtime：

```text
conda                                               # 通过 PATH 直接解析系统 Conda 命令
/home/vs680/.kavio/apps/<app_id>/app/              # .kap 解包后的 App Package
/home/vs680/.kavio/apps/<app_id>/runtime/          # Platform 管理的 App Runtime
/home/vs680/.kavio/apps/<app_id>/runtime/python_env/ # Python App 独立环境
/home/vs680/.kavio/apps/.data/<app_id>/            # App 持久数据和配置
```

`app/` 与 `runtime/` 必须分离：前者只保存安装包发布内容，后者保存 Platform 为该 App 生成的运行信息和 Python 环境。Python 环境不使用全局平铺目录。

使用路径环境，而不是依赖 Conda 全局环境名称注册。安装期间先在临时目录准备环境，部署成功后进入：

```text
/home/vs680/.kavio/apps/<app_id>/runtime/python_env/
```

app_id 必须经过严格格式校验，禁止路径分隔符和目录穿越字符。

### 13.2 环境创建方式

#### 方式 A：完整离线环境归档

安装包包含：

```text
device/python_env.tar.zst
```

安装时：

1. 解压到临时环境目录；
2. 执行必要的环境重定位，例如 `conda-unpack`；
3. 验证 `bin/python`；
4. 验证主脚本语法；
5. 成功后切换为正式同名环境。

#### 方式 B：environment.yml 在线创建

安装包包含：

```text
device/environment.yml
```

AppService 使用：

```text
conda env create --prefix <temporary_env> --file environment.yml
```

软件源必须来自 Platform 配置的允许列表，不能由安装脚本执行任意 shell 命令。

#### 方式 C：requirements.txt 在线创建

安装包包含：

```text
device/requirements.txt
```

流程：

1. 根据 `python_version` 创建基础环境；
2. 使用环境中的 Python 执行：

```text
python -m pip install --no-input -r requirements.txt
```

3. 软件源使用 Platform 配置的 `pip_index_url`；
4. 验证解释器和主脚本。

### 13.3 auto 模式

`install_mode=auto` 时：

1. 有完整环境归档则优先恢复；
2. 否则有 environment.yml 则在线创建；
3. 否则有 requirements.txt 则在线创建；
4. 都没有则安装失败。

### 13.4 安装事务

Python 环境创建必须在临时目录执行：

```text
/home/vs680/.kavio/apps/.installing/<install_session_id>.python-env
```

只有以下条件全部满足时，应用才进入“已安装”：

- App 文件完整；
- 签名校验完成，或开发模式风险确认完成；
- Python 环境创建成功；
- `env/bin/python` 可执行；
- 主脚本存在；
- 主脚本通过最小语法检查。

任一步骤失败：

- 整个应用安装失败；
- 清理临时环境；
- 不允许启动；
- 不单独保留“Python 环境失败但应用已安装”的状态。

### 13.5 在线安装异步化

在线依赖安装可能持续较长时间，不能阻塞 USB 接收线程。

建议：

1. `InstallCommit（提交安装）` 快速返回“已接受，正在安装”；
2. AppService 工作线程执行环境创建；
3. 安装状态保持“安装中”；
4. AppService 持续更新安装状态和阶段事件；
5. KavioWin 通过状态事件或轮询 Query 获取结果；
6. 最终成功后进入“已安装”，失败则进入“安装失败”。

### 13.6 依赖源配置

应用包可以声明依赖，不能任意覆盖系统软件源安全策略。

建议配置：

```protobuf
bool allow_online_python_install = 1;
repeated string conda_channels = 2;
string pip_index_url = 3;
```

### 13.7 环境审计

在线环境创建成功后，AppService 保存审计信息：

- Conda 显式包列表；
- `pip freeze` 结果；
- 创建时间；
- 使用的软件源；
- 原始依赖文件摘要。

这些信息用于排查和复现，不作为单独运行状态展示。

### 13.8 Python 启动方式

AppService 使用参数数组直接执行：

```text
/home/vs680/.kavio/apps/<app_id>/runtime/python_env/bin/python
/home/vs680/.kavio/apps/<app_id>/app/<entry>
<arguments...>
```

必须使用 `execve（执行程序）`，不能通过 shell 拼接命令。

原生 App 和 Python App 都使用 `AppServiceConfigPb.run_user` 指定用户运行，第一版默认 `vs680`。

---

## 14. 签名、开发模式与运行授权

### 14.1 正常信任路径

官方应用包使用：

- 文件摘要：`SHA-256（安全散列算法）`；
- 数字签名：`Ed25519（爱德华曲线签名算法）`；
- KavioWin 和 Platform 内置 Kavio 官方公钥并分别校验。

官方签名有效的包在正常模式下可以安装和运行。

### 14.2 信任状态分类

| 状态 | 处理 |
|---|---|
| Kavio 官方签名有效 | 正常安装和运行 |
| 未提供签名 | 归类为“签名未验证”，需要开发模式 |
| 非官方或未知签名 | 归类为“签名未验证”，需要开发模式 |
| 声称使用 Kavio 官方 key_id 但签名无效 | 视为损坏或篡改，拒绝安装 |
| 官方签名有效但文件摘要不一致 | 视为损坏或篡改，拒绝安装 |

用户自行编写的程序应不携带伪造的官方签名声明，按未签名开发包安装。

### 14.3 双端开发模式

系统增加两个相互独立的开发模式：

#### host_developer_mode（上位机开发模式）

- 存储在 KavioWin 本地设置中；
- 由 KavioWin 设置页面打开或关闭；
- 决定未签名Host Component DLL 是否允许安装和运行；
- 关闭时，当前运行的未签名 Kavio App Host 必须停止；
- 关闭后，已经安装的未签名 DLL 保留文件，但禁止再次启动。

#### device_developer_mode（设备开发模式）

- 持久化在设备 Platform 系统配置中；
- 可以由 KavioWin 设置页面远程打开或关闭；
- Device Component（设备组件）Service 独立校验，不能只信任 KavioWin 请求；
- 决定未签名Device Component（设备组件） 是否允许安装和运行；
- 关闭时，当前运行的未签名Device Component（设备组件） 必须停止；
- 关闭后，已经安装的未签名 App 保留文件和数据，但禁止再次启动；
- 未签名 auto_start App 在设备开发模式关闭时不得自动启动。

### 14.4 不同应用包的开发模式要求

| 应用包内容 | 未签名时的要求 |
|---|---|
| 仅Device Component（设备组件） | 必须开启设备开发模式 |
| 仅上位机组件（未来预留） | 必须开启上位机开发模式 |
| Device Component（设备组件） + Host Component DLL | 必须同时开启上位机和设备开发模式 |

应用包是一个安装事务。双端未签名包如果只有一侧开发模式开启，则整个安装失败，不做部分安装。

### 14.5 安装时风险确认

开发模式只是允许进入未签名流程，不能代替用户确认。

每次安装或覆盖更新未签名包时，KavioWin 必须展示：

- 应用名称、版本和 app_id；
- 包 SHA-256；
- 是否包含Host Component DLL；
- 设备入口程序；
- 是否为 Python；
- 是否需要在线下载依赖；
- 未签名代码可能访问的主机或设备资源；
- 在线依赖可能引入第三方供应链代码的风险；
- 当前开启的开发模式。

用户必须执行明确确认后才提交安装。

### 14.6 Platform 和 KavioWin 的双重强制检查

未签名Device Component（设备组件）：

```text
KavioWin 风险确认
+ device_developer_mode=true
+ AppService 安装时再次校验
+ AppService 每次启动前再次校验
```

未签名Host Component DLL：

```text
KavioWin 风险确认
+ host_developer_mode=true
+ HostComponentRuntimeService 安装时再次校验
+ Kavio App Host 每次启动前再次校验
```

安装请求中的“用户已确认”标志不能替代开发模式。

### 14.7 开发模式关闭时的处理

关闭上位机开发模式：

1. 停止所有正在运行的未签名 Kavio App Host；
2. 将其状态设置为“开发模式关闭，禁止运行”；
3. 不删除安装文件和配置；
4. 官方签名应用不受影响。

关闭设备开发模式：

1. AppService 停止所有正在运行的未签名Device Component（设备组件）；
2. 将其状态设置为“开发模式关闭，禁止运行”；
3. auto_start 不再启动这些 App；
4. 不删除安装文件、Python 环境和 App 数据；
5. 官方签名 App 和固件内置 App 不受影响。

重新开启开发模式后，未签名组件重新获得手动启动和 auto_start 资格，但不会因为模式切换自动立即启动。

### 14.8 管理界面提示

开发模式打开时，KavioWin 顶部持续显示明显提示：

```text
上位机开发模式已开启：允许运行签名未验证的主机代码
设备开发模式已开启：允许设备运行签名未验证的应用
```

每个未签名应用条目持续显示：

```text
签名未验证
```

即使开发模式已经开启，也不能把未签名应用显示为“已信任”。

---

## 15. 安装和覆盖更新

### 15.1 通用流程

1. KavioWin 解析应用包并进行本地检查；
2. 对官方包进行签名校验；
3. 对未签名包检查相关开发模式并显示风险确认；
4. 停止目标Device Component（设备组件）；
5. 如果存在Host Component（主机组件），停止对应 Kavio App Host；
6. Platform 创建安装会话和临时目录；
7. 分片传输设备文件；
8. AppService 记录并上报接收和校验日志；
9. 校验文件大小、摘要和清单；
10. 设备端再次执行签名或开发模式检查；
11. 原生 App 设置权限；Python App 创建独立虚拟环境；
12. 部署完成后原子切换正式运行目录；
13. 写入 AppRegistry；
14. 安装上位机组件；
15. 双端均成功后，应用包状态进入“已安装”。

### 15.2 未签名双端包

未签名双端包安装前必须满足：

```text
host_developer_mode=true
且
device_developer_mode=true
```

任一条件不满足，整个安装事务失败。

### 15.3 不支持历史回滚

第一版：

- 不保留历史版本；
- 不提供版本回滚按钮；
- 更新失败后状态为“安装失败”；
- 用户通过再次覆盖安装修复。

仍使用临时目录和原子替换，目的是避免产生半文件，不代表提供版本回滚能力。

### 15.4 安装失败清理

必须清理：

- 安装临时目录；
- Python 临时环境；
- 未完成的安装会话；
- 未正式注册的 AppRecord；
- 临时 DLL 文件；
- 临时签名和摘要缓存。

保留失败日志，供 KavioWin 查询。

### 15.5 卸载

卸载流程：

1. 停止Device Component（设备组件）；
2. 停止Host Component（主机组件）；
3. 删除设备运行目录；
4. 删除 Python 环境；
5. 默认保留或删除 App 数据目录由用户选择；
6. 删除 AppRegistry 记录；
7. 删除上位机组件。

系统内置 App 不允许通过普通卸载接口删除。

---

## 16. AppService 设计

### 16.1 核心职责

AppService 负责：

- 扫描系统内置 App；
- 扫描用户安装 App；
- 安装会话管理；
- 原生运行文件准备；
- Python 环境创建和删除；
- App 启动、停止和强制结束；
- PID 到 app_id 映射；
- 进程退出监控；
- auto_start 调度；
- 设备开发模式检查；
- 未签名 App 运行资格检查；
- 状态事件上报；
- 通信路由所需 App 身份查询。

AppService 不负责：

- 保存业务配置；
- 理解业务配置字段；
- 缓存 USB 离线期间的应用通道原始数据报；
- 解释 App 私有协议；
- 异常退出后的自动保活；
- 控制Host Component（主机组件）生命周期；
- 保存、查询或上报Device Component（设备组件） 的 stdout/stderr。

### 16.2 AppServiceConfigPb

建议：

```protobuf
message AppServiceConfigPb {
  string app_root_dir = 1;
  string install_temp_dir = 2;
  uint32 stop_timeout_ms = 3;
  string run_user = 4;
  string builtin_app_root_dir = 5;
  bool developer_mode = 6;
  bool allow_online_python_install = 7;
  repeated string conda_channels = 8;
  string pip_index_url = 9;
}
```

当前 VS680 默认值：

```text
builtin_app_root_dir          = /home/vs680/kavio/apps
app_root_dir                  = /home/vs680/.kavio/apps
install_temp_dir              = /home/vs680/.kavio/apps/.installing
stop_timeout_ms               = 3000
run_user                      = vs680
developer_mode                = false
allow_online_python_install   = true
```

字段 `developer_mode` 表示设备开发模式。上位机开发模式不放入该配置，由 KavioWin 本地管理。

### 16.3 进程监控记录

记录：

- app_id；
- runtime_instance_id；
- PID；
- 启动时间；
- 运行类型；
- 信任状态；
- 启动时设备开发模式状态；
- 退出时间；
- 退出码或信号；
- 是否因关闭开发模式而停止；
- 最后错误。

---

## 17. Host Component（主机组件）设计

### 17.1 可选组件

`host` 对象不存在时：

- 不安装 DLL；
- 不创建 Kavio App Host；
- 界面显示“无上位机组件”；
- 仍可管理Device Component（设备组件）；
- 如果有 Config Definition，仍可通过 SystemChannel 配置运行中的 App，不依赖业务 Channel。

### 17.2 Kavio App Host 隔离

一个应用对应一个 Kavio App Host。

第一版：

- `Kavio App Host（应用宿主）` 使用当前 Windows 用户上下文正常启动；
- 不继承 KavioWin 句柄；
- 使用 `Job Object（作业对象）` 管理进程退出；
- 应用安装目录只读；
- 应用数据目录可读写。

### 17.3 统一 Named Pipe

KavioWin 创建统一服务端，例如：

```text
\\.\pipe\KavioAppService
```

每个 Kavio App Host 建立一条独立连接。

服务端和客户端管道句柄均使用 `FILE_FLAG_OVERLAPPED` 创建。服务端使用带 `OVERLAPPED` 的 `ConnectNamedPipe` 等待连接，连接、读取和写入均通过事件等待完成；停止服务或会话时使用停止事件和 `CancelIoEx` 取消未完成操作，不混用同步管道 IO。

连接通过 app_id、instance_id、一次性 launch_token 和宿主 PID 绑定身份。业务接口不允许应用指定其他 app_id。

### 17.4 Host Component 接口

第三方 Host Component 只包含 `KavioHost.h（Kavio 应用接口头文件）`。应用不依赖 KavioWin、LibUtils、命名管道、SDK 版本头、USB 协议、SystemChannel 协议或任何内部消息编号。

必须导出：

```text
KavioHostCreate
KavioHostStart
KavioHostStop
KavioHostDestroy
KavioHostOnData
```

可选导出：

```text
KavioHostPrepareStop
KavioHostOnDeviceState
KavioHostOpenPanel
```

Kavio App Host 向 DLL 提供：

```text
send
is_device_online
get_channel_id
restart
log
get_data_path
```

接口不提供 App Configuration 读取、提交或变更通知能力。Host Component 与Device Component（设备组件） 的消息格式完全由第三方定义。

### 17.5 Kavio App Host 内部通信

KavioWin 与 Kavio App Host 之间的命名管道协议只区分握手、生命周期控制、宿主状态、日志、重启请求、Channel 状态和 Channel 数据。`ChannelData（通道数据）` 的负载是不透明原始数据报，Kavio App Host 和 KavioWin 均不解析其内部结构。

Kavio App Host 使用独立的有界发送队列和应用回调队列：

- 数据、Channel 状态和App Panel 打开请求不在命名管道读取线程中调用第三方 DLL，而是在单独线程中串行回调；
- 第三方数据回调阻塞不会阻塞宿主管道控制消息读取；
- 停止时先关闭新业务入口，调用 PrepareStop，再停止回调分发，最后按 Stop、Destroy 顺序释放应用；
- 队列达到数量或字节上限时拒绝新数据，不允许无界增长。

### 17.6 辅助能力

Host Component（主机组件）可以为Device Component（设备组件） 提供：

- 主机网络访问；
- 本地文件选择和文件读写；
- 主机 UI；
- 主机系统接口；
- 调试、日志和数据展示。

这些能力是否可用只影响业务功能，不影响Device Component（设备组件） 生命周期。

### 17.7 上位机开发模式检查

HostComponentRuntimeService 在以下阶段检查 host_developer_mode：

- 未签名 DLL 安装前；
- 覆盖更新前；
- Kavio App Host 每次启动前；
- 开发模式关闭时。

关闭开发模式后，未签名 Kavio App Host 必须立即停止。

---

## 18. 内置 App 与用户 App

MFS、TUN 等由 KavioWin 和 Platform 直接实现的内置服务不属于 App，不进入本章的安装、扫描、签名、运行和 IPC 模型。内置服务使用 `kavio.builtin.*` 保留 Channel，并由 SystemChannel 控制面管理。

### 18.1 区分方式

```text
/home/vs680/kavio/apps/<app_id>       → Builtin（系统内置，Package Root）
/home/vs680/.kavio/apps/<app_id>/        → Installed App Instance Root
  ├─ app/                                → App Package Root
  └─ runtime/                            → Platform 管理的 App Runtime
```

来源由扫描目录和安装流程决定，不由 manifest 声明。

### 18.2 信任规则

- 固件镜像内置 App 由固件发布链信任；
- 内置 App 不依赖设备开发模式；
- 用户安装的官方签名 App 不依赖设备开发模式；
- 用户安装的未签名 App 必须依赖设备开发模式。

### 18.3 同名规则

如果内置目录和用户目录存在相同 app_id：

- 内置 App 优先；
- 用户 App 不加载；
- 安装服务拒绝安装同名用户 App；
- 返回明确错误。

### 18.4 Platform 核心边界

必须留在 Platform 内部：

- USB 通信；
- IPCServer；
- AppService；
- 系统配置；
- 安装和权限管理；
- 输入、显示等核心管理。

可以做成内置 App：

- 后台数据采集；
- 可独立升级的算法服务；
- 本地任务执行；
- 独立硬件业务；
- 失败后不影响 Platform 核心的扩展能力。

---

## 19. KavioWin 管理窗口

### 19.1 应用条目展示

应用列表第一列直接显示清单声明的 PNG 图标，`App ID` 后紧跟安装状态列；安装任务从取得有效 `app_id` 后即进入列表，状态至少区分“安装中”“安装失败”“已安装”。安装失败条目保留到用户执行卸载/清理；有部分组件时清理已存在组件，没有任何已安装部分时直接删除条目。

每个应用条目展示：

- PNG 图标、App ID、安装状态、版本和描述；
- 运行类型：原生或 Python；
- Device Component（设备组件） 状态；
- Host Component（主机组件）状态；
- USB 和 Channel 状态；
- auto_start；
- 信任状态；
- 运行授权状态；
- 是否需要在线安装依赖；
- 最近安装阶段；
- 最后错误。

不展示独立的 Python 环境状态。

### 19.2 操作按钮

| 条件 | 操作 |
|---|---|
| 纯Device Component（设备组件） | 启动 App、停止 App、配置、卸载 |
| 含Host Component（主机组件） | 启动 App、停止 App、启动应用、停止应用、全部启动、全部停止、配置、卸载 |
| 无配置 | 不显示配置入口 |
| App 未运行或 SystemChannel 不可用 | 配置入口禁用或提示先启动并连接设备 |
| 签名未验证 | 持续显示风险标记和运行授权条件 |
| 安装失败 | 允许重新覆盖安装，不允许启动 |
| 开发模式关闭 | 未签名对应组件的启动按钮禁用 |

### 19.3 开发模式设置

设置页面提供两个独立开关：

```text
上位机开发模式
设备开发模式
```

操作设备开发模式要求设备在线。KavioWin 将请求发送给 Platform，最终状态以 Platform 返回结果为准。

关闭开发模式前，界面提示将停止对应一侧所有正在运行的未签名代码。

## 20. 协议定义

### 20.1 usb_protocol.proto

USB SystemChannel 使用 `ReqPackagePb` 承载请求，使用 `RspPackagePb` 承载应答。应答包统一包含：

```protobuf
message RspPackagePb {
  HeaderPb header = 1;
  int32 status_code = 2;
  string status_message = 3;
  oneof body {
    // 具体应答消息
  }
}
```

`status_code` 直接传输 `kavio::StatusCode` 的整数值，`status_message` 保存面向日志和界面的状态描述。USB Protobuf 协议不定义独立错误码枚举。

`StatusCode` 使用统一的领域前缀和数值分段：

| 数值范围 | 前缀 | 适用范围 |
|---|---|---|
| `0~99` | `StatusCode::Common_` | 参数、状态、资源、超时和通用协议错误 |
| `100~149` | `StatusCode::Session_` | 会话生命周期和会话资源限制 |
| `150~199` | `StatusCode::Media_` | 视频和图像帧状态 |
| `200~209` | `StatusCode::SHM_` | SHM 创建、映射和读写操作 |
| `210~299` | `StatusCode::IPC_` | IPC Socket 和 IPC 传输错误 |
| `300~399` | `StatusCode::USB_` | USB SystemChannel 和Device Component（设备组件） 管理错误 |

成功状态固定为 `StatusCode::OK`。USB Device Component（设备组件） 管理状态使用 `StatusCode::USB_AppXxx` 命名，明确区分请求体错误和 App 安装、校验、运行环境错误。

错误发生位置负责生成准确的 `StatusCode`。`AppService` 返回业务状态，Platform 的 USB 协议层只将 `Status.code` 和 `Status.message` 写入应答，不解析错误字符串。KavioWin 直接读取 `status_code` 和 `status_message`，以 `status_code == StatusCode::OK` 判断请求是否成功。

`StatusCodeToString(StatusCode)` 和 `StatusCodeToString(int32_t)` 返回稳定的作用域状态码符号名。日志同时打印状态码数值、符号名和状态描述，未知数值统一返回 `StatusCode::Unknown`。

`StatusCode.h` 定义作用域枚举 `enum class StatusCode` 和字符串转换函数；`Status.h` 定义统一的 `Status`，仅包含 `StatusCode code`、`std::string message`、`ok()` 和 `name()`。Platform、SDK 与 KavioWin 共用该 `Status`，协议响应状态不定义局部包装结构。`Types.h` 只包含 `Status.h` 并继续定义依赖 C++17 的其他 SDK 类型。

SystemChannel 请求和应答覆盖：

- 设备授权；
- 安装开始、安装分片、安装提交和安装取消；
- 卸载、启动、停止、查询和列表；
- 自动启动设置；
- 设备开发模式查询和设置；
- App Configuration 读取和保存；
- 内置服务查询和启停；
- Device Component（设备组件） 和内置服务状态事件。

AppService 不保存 Config Definition 业务配置；App Configuration通过 USB SystemChannel 路由到目标Device Component（设备组件），由Device Component（设备组件） 校验、应用和持久化。

### 20.2 ipc.proto

`MSG_A2P_DATA_COMM_CHANNEL`（App → Platform）与 `MSG_P2A_DATA_COMM_CHANNEL`（Platform → App）分别承载业务数据。IPC 协议定义：

- USB Host 在线状态；
- App 自身 Channel 可用状态；
- `P2AReqGetAppConfigPb`/`A2PRspGetAppConfigPb` 与 `P2AReqSetAppConfigPb`/`A2PRspSetAppConfigPb`；
- App 受控重启请求；
- SDK 自动读取、保存配置并发送响应，只有成功的 SET 通过统一 `OnMessage()` 通知 App。

## 21. 建议数据结构

### 21.1 AppRecord

```cpp
struct AppRecord {
    std::string app_id;
    std::string version;
    std::string bundle_hash;

    AppOrigin origin;
    AppRuntime runtime;
    AppTrustLevel trust_level;

    bool auto_start;
    bool has_host_app;
    bool auto_start_attempted;

    std::string runtime_root;
    std::string data_root;
    std::string entry_path;
    std::vector<std::string> arguments;

    std::string python_env_path;
    PythonInstallMode python_install_mode;

    AppInstallState install_state;
    AppRuntimeState runtime_state;
    AppRunAuthorization run_authorization;

    uint64_t runtime_instance_id;
    pid_t pid;
    int last_exit_code;
    uint64_t start_time_ms;
    uint64_t last_exit_time_ms;
    std::string last_error;
};
```

### 21.2 枚举

```cpp
enum class AppRuntime {
    Native,
    Python
};

enum class AppTrustLevel {
    BuiltinTrusted,
    OfficialSigned,
    Unverified,
    Invalid
};

enum class PythonInstallMode {
    Archive,
    Online,
    Auto
};

enum class AppStartSource {
    PlatformAutoStart,
    HostManagement,
    DeviceMenu,
    InternalSystem
};

enum class AppRunAuthorization {
    Allowed,
    DeviceDeveloperModeRequired,
    PackageInvalid
};
```

### 21.3 HostComponentRecord

```cpp
struct HostComponentRecord {
    std::string app_id;
    std::string version;
    HostComponentTrustLevel trust_level;
    HostComponentRuntimeState runtime_state;
    HostComponentRunAuthorization run_authorization;
    uint64_t runtime_instance_id;
    uint32_t process_id;
    std::string last_error;
};
```

---

## 22. 实施阶段记录（历史）

### 阶段 0：清单和协议冻结

- `manifest_version` 使用 `KAVIO_MANIFEST_VERSION` 的当前值（1），仅在格式契约调整时再评估变更；
- 不定义任何 App 与 Host Component（主机组件）的生命周期关联字段；
- host 对象保持可选；
- 保留 native/python runtime；
- 保留 auto_start；
- 冻结 Python archive/online/auto 安装模式；
- 定义双端开发模式；
- 定义 SystemChannel Config Definition 请求/响应与 App IPC 请求/响应。

### 阶段 1：生命周期与 Channel 解耦

- 删除 USB 离线停止 App 的逻辑；
- 对端 Endpoint Offline 只改变通信就绪状态，不停止 App；
- 删除 Kavio App Host 停止联动 App 的逻辑；
- ChannelID 独立于 Kavio App Host 和 USB 会话生命周期；
- USB 恢复后通过双端 Endpoint Snapshot 恢复通信状态；
- 保留 SO_PEERCRED PID 强绑定；
- auto_start 不依赖 USB 和 Kavio App Host。

### 阶段 2：配置职责调整

- AppService 只提供 App 数据目录；
- SDK 增加标准完整配置请求和响应；
- App 自行读取、保存和应用配置；
- KavioWin 配置页面要求 App 运行且 SystemChannel 可用。

### 阶段 3：Python 环境安装

- 镜像预装 Miniforge；
- Python 环境归属 App Runtime，路径固定为 `<app_id>/runtime/python_env/`；
- 支持环境归档恢复；
- 支持 environment.yml 在线创建；
- 支持 requirements.txt 在线创建；
- 环境创建放入安装工作线程；
- 环境失败导致整个安装失败；
- 不增加独立 Python 环境状态。

### 阶段 4：双端开发模式

- KavioWin 增加 host_developer_mode；
- Platform 增加 device_developer_mode；
- 增加设备开发模式查询和设置协议；
- 未签名双端包要求双端模式同时开启；
- 每次启动前重新校验开发模式；
- 关闭模式时停止对应未签名进程；
- 管理界面增加持续风险提示和运行授权状态。

### 阶段 5：KavioWin 管理界面

- 分离Device Component（设备组件）、Host Component（主机组件）和通信状态；
- 显示信任和运行授权状态；
- 增加单独和组合启动停止；
- 增加 auto_start 编辑；
- 增加双端开发模式设置；
- 不显示 Python 环境独立状态。

### 阶段 6：稳定性验证

重点验证：

- Device Component（设备组件） 在 USB 断开后继续运行；
- Host Component（主机组件）停止后Device Component（设备组件） 继续运行；
- Device Component 停止后Host Component（主机组件）可以继续运行；
- USB 恢复后 App 不重启即可恢复通信；
- 纯Device Component（设备组件） 在无 Kavio App Host 时仍有 SystemChannel 通用配置能力；
- auto_start App 在 Platform 启动后只自动启动一次；
- App 异常退出后不自动保活；
- Python 在线依赖安装成功和失败流程；
- Python 离线环境恢复；
- 环境失败导致整体安装失败；
- 官方签名包在正常模式安装和运行；
- 未签名纯Device Component（设备组件） 只有设备开发模式开启时才能安装和运行；
- 未签名Host Component DLL 只有上位机开发模式开启时才能安装和运行；
- 未签名双端包要求双端开发模式；
- 关闭设备开发模式会停止运行中的未签名 App；
- 关闭上位机开发模式会停止运行中的未签名 Kavio App Host；
- 重新开启模式后不会自动立即启动；
- AppService 不持久化业务配置；
- Host Component 不读取或修改 App Configuration；
- Host Component 与Device Component（设备组件） 之间按完整原始数据报双向通信；
- App 只能访问自身 Channel。

---

## 23. 第一版验收标准（阶段验收记录）

1. 可以安装含Device Component（设备组件） 和可选 Host Component DLL 的官方签名包。
2. 可以安装只包含Device Component（设备组件） 的官方签名包。
3. 未签名纯Device Component（设备组件） 只有设备开发模式开启时才能安装。
4. 未签名Host Component DLL 只有上位机开发模式开启时才能安装。
5. 未签名双端包只有双端开发模式均开启时才能安装。
6. 官方签名无效或官方签名文件摘要不一致时不能按未签名包直接安装。
7. 原生 App 使用指定普通用户运行。
8. Python App 使用 app_id 对应的独立环境运行。
9. Python App 可以在线安装依赖，也可以恢复完整离线环境。
10. Python 环境创建失败时整个应用安装失败。
11. 不显示独立 Python 环境运行状态。
12. auto_start App 在 Platform 启动后只自动启动一次。
13. App 异常退出后不自动保活。
14. USB 断开不会停止Device Component（设备组件）。
15. Host Component（主机组件）停止不会停止Device Component（设备组件）。
16. Device Component（设备组件） 停止不会停止Host Component（主机组件）。
17. USB 恢复后原 App 进程可以恢复业务 Channel。
18. App 和 Host Component（主机组件）可以分别启动和停止。
19. 纯Device Component（设备组件） 不需要 Kavio App Host 也可以被管理和配置。
20. App 只能访问自身 app_id 对应 Channel。
21. PID 身份来自 SO_PEERCRED，不信任客户端声明。
22. AppService 不持久化业务配置。
23. KavioWin 每次向 App 发送完整 App Configuration，不发送差异字段，不使用 config_version。
24. Device SDK 统一校验和持久化 App Configuration，App 应用生效值；Host Component 不参与配置链路。
25. Host Component 与Device Component（设备组件） 之间使用保持边界和顺序的完整原始数据报，框架不解析业务协议。
26. 关闭设备开发模式后，运行中的未签名Device Component（设备组件） 被停止且不能再次启动。
27. 关闭上位机开发模式后，运行中的未签名 Kavio App Host 被停止且不能再次启动。
28. 未签名应用在管理页面持续显示“签名未验证”。
29. 内置 App 与用户 App 来源明确，用户包不能覆盖内置 App。
30. Platform 核心服务不会被错误包装为普通可卸载 App。

---

## 24. 最终设计结论

1. Device Component（设备组件） 是主要业务主体，Host Component（主机组件）是可选辅助组件。
2. 应用包可以包含Device Component（设备组件） 和 Host Component DLL，也可以只包含Device Component（设备组件）。
3. Device Component（设备组件） 和 Host Component（主机组件）生命周期完全独立。
4. 当前不定义 App 与 Host Component（主机组件）的关联关闭策略。
5. auto_start 是唯一的 App 自动生命周期策略，只在每次 Platform 启动周期尝试一次，不做 daemon 保活。
6. USB 断开、Channel 消失、Kavio App Host 停止或 KavioWin 退出都不会自动停止Device Component（设备组件）。
7. Channel 由 KavioWin 应用管理层维护，不跟随 Kavio App Host 生命周期。
8. AppService 负责安装、Python 环境、部署、启动停止、进程监控和身份绑定，不负责业务配置持久化。
9. 业务配置由 Device SDK 保存，App 负责应用生效值；KavioWin 每次发送完整 JSON。
10. 原生 App 和 Python App 使用同一生命周期和状态模型。
11. Python App 使用独立 Miniforge 环境。
12. Python 环境可在线创建，也可从离线归档恢复；环境失败即整个应用安装失败。
13. Python 环境不作为独立界面状态展示。
14. 官方签名是正常模式下的代码信任路径。
15. 系统分别提供上位机开发模式和设备开发模式。
16. 未签名Host Component DLL 在上位机开发模式下可以安装和运行。
17. 未签名Device Component（设备组件） 在设备开发模式下可以安装和运行。
18. 未签名双端包要求双端开发模式同时开启。
19. 开发模式关闭后，对应一侧已经安装的未签名代码保留但立即失去运行资格，正在运行的进程必须停止。
20. 管理页面持续显示开发模式风险提示和“签名未验证”标记。
21. 内置 App 和用户 App 按来源区分，系统内置 App 不允许普通覆盖或卸载。
22. 一个 Kavio App Host 对应一个应用，所有 Kavio App Host 连接同一个 KavioWin Named Pipe 服务端，但各自使用独立连接。
23. Kavio App Host 是第三方 DLL 的生命周期容器和原始数据桥，不参与 Host Component 与Device Component（设备组件） 的业务协议。
24. App Configuration仅由 KavioWin、SystemChannel、Platform 和Device Component（设备组件） 管理，Host Component 不接入该配置链路。
25. App 身份通过 SO_PEERCRED 实际 PID 绑定，并只能使用自身 app_id Channel。

该方案兼顾官方应用、内部开发、用户自编 Python 脚本、纯设备任务、主机辅助能力和现场诊断，同时将正常运行环境与开发模式下的未签名代码风险明确隔离。
