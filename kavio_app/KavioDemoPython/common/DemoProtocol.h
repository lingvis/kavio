/**
 * @file DemoProtocol.h
 * @brief Shared on-wire protocol used by the device and Windows Host demos.
 *
 * The complete packet is at most 1 MiB. Its 20-byte little-endian header
 * contains magic, version, message type, sequence, payload length, and an
 * FNV-1a checksum. This checksum detects accidental corruption; it does not
 * authenticate peers. Python and Lua implementations use the same field order.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kavio_demo {

constexpr uint32_t kMagic = 0x3144534Bu; // "KSD1" in little-endian byte order.
constexpr uint16_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 20;
constexpr size_t kMaxPayloadBytes = 1024u * 1024u - kHeaderSize;

/** Stable cross-language datagram identifiers. */
enum class MessageType : uint16_t {
    Hello = 1,
    HelloAck = 2,
    Ping = 3,
    Pong = 4,
    Navigate = 5,
    TransferData = 6,
    TransferAck = 7,
    QueryState = 8,
    StateReport = 9,
};

/** Zero-based navigation IDs; order must match Python/Lua page lists. */
enum class Page : uint8_t {
    Home = 0,
    Render = 1,
    Transfer = 2,
    HdmiRx = 3,
    HdmiTx = 4,
    Runtime = 5,
    Keyboard = 6,
    H264 = 7,
    Schema = 8,
    Mouse = 9,
};

/** Decoded datagram; payload bytes are owned by this struct. */
struct Packet {
    MessageType type = MessageType::Hello;
    uint32_t sequence = 0;
    uint32_t payload_checksum = 0;
    std::vector<uint8_t> payload;
};

inline void AppendU16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFFu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
}

inline void AppendU32(std::vector<uint8_t>& out, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

inline void AppendU64(std::vector<uint8_t>& out, uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
}

inline bool ReadU16(const uint8_t* data, size_t size, size_t offset, uint16_t& value)
{
    if (!data || offset + 2 > size) return false;
    value = static_cast<uint16_t>(data[offset]) |
            static_cast<uint16_t>(data[offset + 1] << 8);
    return true;
}

inline bool ReadU32(const uint8_t* data, size_t size, size_t offset, uint32_t& value)
{
    if (!data || offset + 4 > size) return false;
    value = static_cast<uint32_t>(data[offset]) |
            (static_cast<uint32_t>(data[offset + 1]) << 8) |
            (static_cast<uint32_t>(data[offset + 2]) << 16) |
            (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

inline bool ReadU64(const uint8_t* data, size_t size, size_t offset, uint64_t& value)
{
    if (!data || offset + 8 > size) return false;
    value = 0;
    for (int shift = 0; shift < 64; shift += 8)
        value |= static_cast<uint64_t>(data[offset + static_cast<size_t>(shift / 8)]) << shift;
    return true;
}

inline uint32_t Checksum(const uint8_t* data, size_t size)
{
    // FNV-1a is intentionally simple: this protocol is a demo integrity check, not a security primitive.
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

inline std::vector<uint8_t> MakePacketWithChecksum(MessageType type,
                                                   uint32_t sequence,
                                                   const uint8_t* payload,
                                                   size_t payload_size,
                                                   uint32_t payload_checksum)
{
    if (payload_size > kMaxPayloadBytes || (payload_size != 0 && !payload)) return {};
    std::vector<uint8_t> out;
    out.reserve(kHeaderSize + payload_size);
    AppendU32(out, kMagic);
    AppendU16(out, kProtocolVersion);
    AppendU16(out, static_cast<uint16_t>(type));
    AppendU32(out, sequence);
    AppendU32(out, static_cast<uint32_t>(payload_size));
    AppendU32(out, payload_checksum);
    if (payload_size) out.insert(out.end(), payload, payload + payload_size);
    return out;
}

inline std::vector<uint8_t> MakePacket(MessageType type,
                                       uint32_t sequence,
                                       const uint8_t* payload,
                                       size_t payload_size)
{
    return MakePacketWithChecksum(type, sequence, payload, payload_size,
                                  Checksum(payload, payload_size));
}

inline std::vector<uint8_t> MakePacket(MessageType type,
                                       uint32_t sequence,
                                       const std::vector<uint8_t>& payload)
{
    return MakePacket(type, sequence,
                      payload.empty() ? nullptr : payload.data(), payload.size());
}

inline std::vector<uint8_t> MakeTextPacket(MessageType type,
                                           uint32_t sequence,
                                           const std::string& text)
{
    return MakePacket(type, sequence,
                      reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

/**
 * @brief Validate a complete datagram and decode its fields.
 * @param data Pointer to the full packet, including its header.
 * @param size Available bytes at data.
 * @param out Receives owned decoded data on success.
 * @return True only when version, size, and checksum are valid.
 */
inline bool ParsePacket(const void* data, size_t size, Packet& out)
{
    if (!data || size < kHeaderSize) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t type = 0;
    uint32_t sequence = 0;
    uint32_t payload_size = 0;
    uint32_t checksum = 0;
    if (!ReadU32(bytes, size, 0, magic) ||
        !ReadU16(bytes, size, 4, version) ||
        !ReadU16(bytes, size, 6, type) ||
        !ReadU32(bytes, size, 8, sequence) ||
        !ReadU32(bytes, size, 12, payload_size) ||
        !ReadU32(bytes, size, 16, checksum)) {
        return false;
    }
    if (magic != kMagic || version != kProtocolVersion || payload_size > kMaxPayloadBytes)
        return false;
    if (size != kHeaderSize + static_cast<size_t>(payload_size)) return false;
    const uint8_t* payload = bytes + kHeaderSize;
    if (Checksum(payload, payload_size) != checksum) return false;
    out.type = static_cast<MessageType>(type);
    out.sequence = sequence;
    out.payload_checksum = checksum;
    out.payload.assign(payload, payload + payload_size);
    return true;
}

inline std::vector<uint8_t> EncodeU64(uint64_t value)
{
    std::vector<uint8_t> out;
    out.reserve(8);
    AppendU64(out, value);
    return out;
}

inline bool DecodeU64(const std::vector<uint8_t>& payload, uint64_t& value)
{
    return ReadU64(payload.data(), payload.size(), 0, value) && payload.size() == 8;
}

inline std::vector<uint8_t> EncodePage(Page page)
{
    return {static_cast<uint8_t>(page)};
}

inline bool DecodePage(const std::vector<uint8_t>& payload, Page& page)
{
    if (payload.size() != 1 || payload[0] > static_cast<uint8_t>(Page::Mouse)) return false;
    page = static_cast<Page>(payload[0]);
    return true;
}

inline std::vector<uint8_t> EncodeTransferAck(uint32_t bytes, uint32_t checksum)
{
    std::vector<uint8_t> out;
    out.reserve(8);
    AppendU32(out, bytes);
    AppendU32(out, checksum);
    return out;
}

inline bool DecodeTransferAck(const std::vector<uint8_t>& payload,
                              uint32_t& bytes,
                              uint32_t& checksum)
{
    return payload.size() == 8 &&
           ReadU32(payload.data(), payload.size(), 0, bytes) &&
           ReadU32(payload.data(), payload.size(), 4, checksum);
}

inline std::string PayloadAsString(const std::vector<uint8_t>& payload)
{
    return std::string(payload.begin(), payload.end());
}

} // namespace kavio_demo
