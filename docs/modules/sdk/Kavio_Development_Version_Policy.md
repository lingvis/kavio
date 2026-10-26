# Kavio SDK 版本与兼容规则

## 1. SDK 版本

SDK 只维护三段式版本号：

```text
major.minor.patch
```

唯一数值来源位于 `kavio_sdk/device/include/kavio/KavioSdkVersion.h`：

```cpp
#define KAVIO_SDK_VERSION_MAJOR 3
#define KAVIO_SDK_VERSION_MINOR 0
#define KAVIO_SDK_VERSION_PATCH 0
```

`KAVIO_SDK_VERSION_STRING` 由上述三个字段自动生成，不再单独维护 `KAVIO_SDK_COMPAT_VERSION` 或其他 SDK 兼容版本号。

版本规则：

- 三段版本整体仅作为一个可排序的 SDK 版本号；
- `major` 不再具有“变化即不兼容”的特殊语义；
- 是否允许接入只由 Platform 配置的最低 SDK 版本决定；
- App、SHM、KavioWin Kavio App Host 内部管道等不再维护独立的 SDK/兼容版本号。

## 2. IPC Session 创建时的唯一版本校验

Device Component 创建 IPC Session 时，`A2PReqCreateSessionPb.sdk_version` 上报当前 SDK 的 `major/minor/patch`。

Platform 在 `IPCServer::HandleReqSessionCreate` 中只校验一次：

1. 客户端完整版本不得低于 Platform 配置的 IPC 最低 SDK 版本；
2. 不再单独比较 `major`，只按 `major/minor/patch` 的字典序比较完整版本。

最低接入版本由 Platform 的 IPC Server 配置：

```cpp
kIpcMinimumSdkVersion = { major, minor, patch }
```

例如最低接入版本为 `3.0.1`：

- `3.0.0`：拒绝；
- `3.0.1`：接受；
- `3.1.0`：接受；
- `4.0.0`：接受；
- `2.9.9`：拒绝。

版本不满足要求时，Platform 先返回 `MSG_P2A_RSP_ERROR` / `Common_Unsupported`，随后关闭该 IPC 连接。

Session 创建成功后，整个连接生命周期不再重复进行 SDK 版本检查。

## 3. IPC / protobuf

IPC protobuf 不维护独立协议版本号。

当前协议尚未作为稳定公共线协议发布，因此 schema 以当前实现为唯一基线：

- 删除字段时不保留由历史实现产生的 `reserved` tombstone；
- 普通 message 内由历史删除造成的字段号空洞应收敛为连续编号；
- 为消息分类、数值区段或明确扩展空间而设计的编号间隔保持不变；
- MessageType、USB 请求/响应分类区段、HID Usage 等具有外部语义的数值不得为了连续性而重排；
- schema 正式对外发布后，已发布字段号和枚举值进入稳定期，不再修改或复用。

## 4. SHM

SHM 不维护独立版本号，也不通过版本字段做兼容判断。

Create/Open/Attach 时校验一次固定布局和映射边界，包括 magic、header size、metadata size、总映射大小及容量关系。成功后热路径不再重复执行完整布局校验，只使用已确认的映射状态和读写边界。

SHM 布局应保持稳定。需要扩展时优先使用预留字段或新增独立结构，不修改已有字段位置和语义。

## 5. Host Component C ABI

App 不导出、不校验 SDK 版本号。

C ABI 通过 `struct_size` 判断结构中哪些字段实际存在。已有字段顺序、类型和函数签名不得改变；新增能力只能尾部追加。

## 6. KavioWin / Kavio App Host 管道

KavioWin 与 KavioAppHost 的管道帧不维护独立版本号。

每条消息仍必须校验 `magic`、`header_size`、`payload_size`，因为这些字段用于确认当前消息边界和读取长度。这属于帧安全校验，不属于版本兼容校验。
