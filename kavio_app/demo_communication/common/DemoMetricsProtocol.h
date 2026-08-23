#pragma once
/*
 * demo_communication 双端业务协议。
 *
 * 这是 Demo 自己的轻量协议，不属于 Kavio SDK 公共协议。Host Component 与
 * Device Component 会直接发送这些 packed POD 结构，因此两端必须使用相同版本、
 * 字段顺序和大小；修改协议时应同时修改两端代码。
 *
 * 当前 Windows/ARM 目标均为 little-endian。若未来需要支持不同端序的平台，
 * 不应继续直接发送 C++ 结构体，而应增加显式序列化。
 */


#include <cstdint>

namespace demo_metrics {

constexpr uint32_t kMagic = 0x4D434450;  // "PDCM"
constexpr uint16_t kVersion = 1;
// 定长响应最多预留 16 个固定磁盘，避免在 Demo 中引入变长序列化。
constexpr uint32_t kMaxDisks = 16;

enum class MessageType : uint16_t {
    Query = 1,
    Response = 2,
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint16_t type = 0;
    uint32_t size = 0;
};

struct QueryMessage {
    MessageHeader header;
    uint64_t sequence = 0;
};

struct DiskUsage {
    char name[16] = {};
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
};

// Host 每次返回完整快照；未使用的 disks 槽位保持为 0。
struct ResponseMessage {
    MessageHeader header;
    uint64_t sequence = 0;
    double cpu_usage_percent = 0.0;
    uint64_t memory_total_bytes = 0;
    uint64_t memory_used_bytes = 0;
    uint32_t disk_count = 0;
    DiskUsage disks[kMaxDisks];
};
#pragma pack(pop)

inline void Initialize(QueryMessage& message, uint64_t sequence) {
    message = QueryMessage();
    message.header.type = static_cast<uint16_t>(MessageType::Query);
    message.header.size = sizeof(message);
    message.sequence = sequence;
}

inline void Initialize(ResponseMessage& message, uint64_t sequence) {
    message = ResponseMessage();
    message.header.type = static_cast<uint16_t>(MessageType::Response);
    message.header.size = sizeof(message);
    message.sequence = sequence;
}

// 每次接收都先检查 magic/version/type/size，避免把其他业务数据误当成本协议。
inline bool IsValid(const MessageHeader& header, MessageType type, uint32_t size) {
    return header.magic == kMagic && header.version == kVersion &&
           header.type == static_cast<uint16_t>(type) && header.size == size;
}

}  // namespace demo_metrics
