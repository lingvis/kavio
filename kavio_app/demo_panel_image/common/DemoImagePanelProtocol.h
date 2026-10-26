#pragma once
/*
 * App Panel image demo protocol.
 *
 * The Host Component decodes PNG/GIF files to BGRA frames and sends them in chunks.
 * The Device Component only assembles raw frames and writes them to a TX surface.
 * All messages are little-endian POD structures local to this demo.
 */
#include <cstddef>
#include <cstdint>

// 定义 Host 图片传输与 Device 图像组装的一致消息布局及大小限制。
namespace demo_image_panel {

constexpr uint32_t kMagic = 0x474D494B;  // "KIMG"
constexpr uint16_t kVersion = 1;
// 这些上限同时约束 Host 解码和 Device 内存分配，修改时必须保持双端一致。
// 图片最大边长 4096、总解码像素数据 128MiB，两项共同限制内存峰值。
// 每包 256KiB：远低于 SDK Channel 1MiB 数据报上限，按 offset 可靠组装。
// 最多 4096 帧仍受 128MiB 总数据量限制，并非允许全部大尺寸帧同时存在。
constexpr uint32_t kMaxWidth = 4096;
constexpr uint32_t kMaxHeight = 4096;
// Frame count is only a hard abuse guard. Practical GIF size is constrained by
// kMaxDecodedBytes below, so small GIFs with hundreds of frames remain valid.
constexpr uint32_t kMaxFrames = 4096;
constexpr uint64_t kMaxDecodedBytes = 128ull * 1024ull * 1024ull;
constexpr uint32_t kChunkBytes = 256u * 1024u;

// 图片像素传输和位置控制拆成不同消息，拖动时无需重复上传图片。
enum class MessageType : uint16_t {
    ImageBegin = 1,
    FrameBegin = 2,
    FrameChunk = 3,
    ImageEnd = 4,
    Position = 5,
    DeviceReady = 6
};

#pragma pack(push, 1)
struct MessageHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t size;
};

struct ImageBeginMessage {
    MessageHeader header;
    uint64_t transfer_id;
    uint32_t width;
    uint32_t height;
    uint32_t frame_count;
};

struct FrameBeginMessage {
    MessageHeader header;
    uint64_t transfer_id;
    uint32_t frame_index;
    uint32_t delay_ms;
    uint32_t data_size;
};

// FrameChunk 后面紧跟 data_size 字节原始 BGRA 数据，因此 header.size 是结构体+payload 总长。
struct FrameChunkMessage {
    MessageHeader header;
    uint64_t transfer_id;
    uint32_t frame_index;
    uint32_t offset;
    uint32_t data_size;
    // data_size bytes immediately follow this structure.
};

struct ImageEndMessage {
    MessageHeader header;
    uint64_t transfer_id;
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

template <typename T>
inline void Initialize(T& message, MessageType type) {
    message = T{};
    message.header.magic = kMagic;
    message.header.version = kVersion;
    message.header.type = static_cast<uint16_t>(type);
    message.header.size = static_cast<uint32_t>(sizeof(T));
}

// 对固定长度消息使用该校验；FrameChunk 因带尾随 payload，由接收端额外检查长度。
inline bool IsValid(const MessageHeader& header, MessageType type, size_t actual_size) {
    return header.magic == kMagic &&
           header.version == kVersion &&
           header.type == static_cast<uint16_t>(type) &&
           header.size == actual_size;
}

}  // namespace demo_image_panel
