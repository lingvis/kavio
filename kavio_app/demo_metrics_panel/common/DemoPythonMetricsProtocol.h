#pragma once
/*
 * demo_metrics_panel Windows 侧业务协议定义。
 *
 * Python 侧必须与 device/protocol.py 保持逐字节一致。这里使用 #pragma pack(1)
 * 和 static_assert 固定结构大小，Python 侧使用显式 little-endian struct 格式。
 * 修改任意字段时，要同步修改两端结构、消息长度和解析逻辑。
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace demo_python_metrics {

constexpr uint32_t kMagic = 0x4D59504Bu;  // "KPYM"
constexpr uint16_t kVersion = 1;
// ResponseMessage 为定长消息，因此固定预留最多 8 个磁盘槽位。
constexpr uint32_t kMaxDisks = 8;

enum class MessageType : uint16_t {
    Query = 1,      // Python Device Component -> Host：请求一份 Windows 指标快照。
    Response = 2,   // Host -> Python Device Component：返回完整指标快照。
    Position = 3,   // Host -> Python Device Component：更新 TX 矩形左上角坐标。
    DeviceReady = 4,   // Python Device Component -> Host：业务循环已就绪，可接收位置命令。
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t size;
};

struct QueryMessage {
    MessageHeader header;
    uint64_t sequence;
};

struct DiskUsage {
    char name[16];
    uint64_t total_bytes;
    uint64_t used_bytes;
};

struct ResponseMessage {
    MessageHeader header;
    uint64_t sequence;
    double cpu_usage_percent;
    uint64_t memory_total_bytes;
    uint64_t memory_used_bytes;
    uint32_t disk_count;
    DiskUsage disks[kMaxDisks];
};

struct PositionMessage {
    MessageHeader header;
    int32_t x;
    int32_t y;
};

struct DeviceReadyMessage {
    MessageHeader header;
};
#pragma pack(pop)

// 这些断言既是协议文档，也是防止 VS 编译选项或结构修改悄悄改变线上布局的保护。
static_assert(sizeof(MessageHeader) == 12, "unexpected MessageHeader packing");
static_assert(sizeof(QueryMessage) == 20, "unexpected QueryMessage packing");
static_assert(sizeof(DiskUsage) == 32, "unexpected DiskUsage packing");
static_assert(sizeof(ResponseMessage) == 304, "unexpected ResponseMessage packing");
static_assert(sizeof(PositionMessage) == 20, "unexpected PositionMessage packing");
static_assert(sizeof(DeviceReadyMessage) == 12, "unexpected DeviceReadyMessage packing");

inline void InitializeHeader(MessageHeader& header, MessageType type, uint32_t size) {
    header.magic = kMagic;
    header.version = kVersion;
    header.type = static_cast<uint16_t>(type);
    header.size = size;
}

inline void Initialize(QueryMessage& message, uint64_t sequence) {
    std::memset(&message, 0, sizeof(message));
    InitializeHeader(message.header, MessageType::Query, sizeof(message));
    message.sequence = sequence;
}

inline void Initialize(ResponseMessage& message, uint64_t sequence) {
    std::memset(&message, 0, sizeof(message));
    InitializeHeader(message.header, MessageType::Response, sizeof(message));
    message.sequence = sequence;
}

inline void Initialize(PositionMessage& message, int32_t x, int32_t y) {
    std::memset(&message, 0, sizeof(message));
    InitializeHeader(message.header, MessageType::Position, sizeof(message));
    message.x = x;
    message.y = y;
}

inline void Initialize(DeviceReadyMessage& message) {
    std::memset(&message, 0, sizeof(message));
    InitializeHeader(message.header, MessageType::DeviceReady, sizeof(message));
}

inline bool IsValid(const MessageHeader& header, MessageType type, size_t size) {
    return header.magic == kMagic &&
           header.version == kVersion &&
           header.type == static_cast<uint16_t>(type) &&
           header.size == size;
}

}  // namespace demo_python_metrics
