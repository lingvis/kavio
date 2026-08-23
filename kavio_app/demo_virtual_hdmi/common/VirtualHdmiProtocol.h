#pragma once

#include <cstddef>
#include <cstdint>

namespace kavio_virtual_hdmi {

constexpr std::uint32_t kProtocolMagic = 0x4D44484Bu; // "KHDM" in little endian.
// App-internal protocol version. Host and Device must use the same value.
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint32_t kFrameWidth = 720;
constexpr std::uint32_t kFrameHeight = 720;
constexpr std::uint32_t kBytesPerPixel = 4;
constexpr std::uint32_t kPixelFormatBgra8888 = 3; // Kavio SDK BGRA8888.
constexpr std::uint32_t kFrameStride = kFrameWidth * kBytesPerPixel;
constexpr std::uint32_t kFrameBytes = kFrameStride * kFrameHeight;
constexpr std::uint16_t kTileSize = 64;
constexpr std::uint32_t kMaxTilesPerFrame =
    ((kFrameWidth + kTileSize - 1) / kTileSize) *
    ((kFrameHeight + kTileSize - 1) / kTileSize);
constexpr std::uint32_t kMaxTilePayloadBytes = kTileSize * kTileSize * kBytesPerPixel + 512;

// The Host Component accepts payloads up to 1 MiB. A tile message is intentionally
// kept below 20 KiB, so it is always sent as one complete comm-channel datagram.
constexpr std::size_t kMaxDatagramBytes = 64u * 1024u;

enum class MessageType : std::uint16_t {
    AppReady = 1,
    FrameBegin = 2,
    Tile = 3,
    FrameEnd = 4,
    FrameAck = 5,
    KeyFrameRequest = 6,
};

enum class FrameType : std::uint8_t {
    IFrame = 0,
    PFrame = 1,
};

enum class TileCodec : std::uint8_t {
    RawBgra = 0,
    RleBgra32 = 1,
};

enum class FrameAckStatus : std::uint32_t {
    Ok = 0,
    InvalidMessage = 1,
    InvalidFrame = 2,
    DecodeFailed = 3,
    ChecksumMismatch = 4,
    TxWriteFailed = 5, // Reserved for diagnostics; ACK is now sent before TX write.
    Timeout = 6,
};

enum class KeyFrameRequestReason : std::uint32_t {
    BaseMismatch = 1,
    FrameTimeout = 2,
    ChecksumMismatch = 3,
    DecodeError = 4,
    StreamReset = 5,
    FrameInterrupted = 6,
    QueueOverflow = 7,
    InvalidFrame = 8,
};

#pragma pack(push, 1)

struct MessageHeader {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t type;
    std::uint32_t payload_size;
};

struct AppReadyPayload {
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t pixel_format; // 3 = BGRA8888 in the Kavio SDK.
    std::uint32_t max_fps;
    std::uint32_t tile_size;
};

struct FrameBeginPayload {
    std::uint64_t frame_id;
    // I frame: 0. P frame: exact Device frame_id that this frame depends on.
    std::uint64_t base_frame_id;
    std::uint64_t timestamp_ns;
    std::uint32_t width;
    std::uint32_t height;
    std::uint32_t stride;
    std::uint32_t changed_tile_count;
    std::uint8_t frame_type;
    std::uint8_t reserved[3];
};

struct TilePayloadHeader {
    std::uint64_t frame_id;
    std::uint16_t x;
    std::uint16_t y;
    std::uint16_t width;
    std::uint16_t height;
    std::uint8_t codec;
    std::uint8_t reserved[3];
    std::uint32_t raw_size;
    std::uint32_t encoded_size;
};

struct FrameEndPayload {
    std::uint64_t frame_id;
    std::uint32_t frame_crc32;
    std::uint32_t reserved;
};

// ACK is cumulative state/flow feedback, not a Stop-and-Wait permission token.
// frame_id is the latest frame that Device reconstructed and CRC-validated; Host
// may use it to release all outstanding sliding-window entries up to frame_id.
struct FrameAckPayload {
    std::uint64_t frame_id;
    std::uint32_t status;
    std::uint32_t received_tile_count;
    std::uint32_t elapsed_us;
};

struct KeyFrameRequestPayload {
    std::uint64_t last_good_frame_id;
    std::uint64_t failed_frame_id;
    std::uint32_t reason;
    std::uint32_t reserved;
};

#pragma pack(pop)

static_assert(sizeof(MessageHeader) == 12, "unexpected MessageHeader layout");
static_assert(sizeof(AppReadyPayload) == 20, "unexpected AppReadyPayload layout");
static_assert(sizeof(FrameBeginPayload) == 44, "unexpected FrameBeginPayload layout");
static_assert(sizeof(TilePayloadHeader) == 28, "unexpected TilePayloadHeader layout");
static_assert(sizeof(FrameEndPayload) == 16, "unexpected FrameEndPayload layout");
static_assert(sizeof(FrameAckPayload) == 20, "unexpected FrameAckPayload layout");
static_assert(sizeof(KeyFrameRequestPayload) == 24, "unexpected KeyFrameRequestPayload layout");

} // namespace kavio_virtual_hdmi
