#include "FrameReceiver.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>

namespace kavio_virtual_hdmi {
namespace {

// 帧组装统一 BGRA8888，因此每像素固定 4 字节。
// 单次 RLE token 最多 128 像素，由 7-bit 计数字段决定；长度边界必须匹配 Host 编码端。
constexpr std::size_t kPixelBytes = 4;
constexpr std::size_t kMaxRlePacketPixels = 128;

// Tile 坐标与大小组合成唯一键，拒绝同帧重复矩形避免覆盖校验绕过。
std::uint64_t RectangleKey(std::uint16_t x, std::uint16_t y,
                           std::uint16_t width, std::uint16_t height) {
    return static_cast<std::uint64_t>(x) |
           (static_cast<std::uint64_t>(y) << 16) |
           (static_cast<std::uint64_t>(width) << 32) |
           (static_cast<std::uint64_t>(height) << 48);
}

const std::array<std::uint32_t, 256>& Crc32Table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> values{};
        for (std::uint32_t i = 0; i < values.size(); ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) ? (0xEDB88320u ^ (value >> 1u)) : (value >> 1u);
            }
            values[i] = value;
        }
        return values;
    }();
    return table;
}

// CRC32 必须覆盖重建后的完整画布，保证 P 帧依赖基线和 Host 端计算口径一致。
std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    const auto& table = Crc32Table();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

} // namespace

FrameReceiver::FrameReceiver()
    : front_(kFrameBytes, 0), back_(kFrameBytes, 0) {
    received_rectangles_.reserve(kMaxTilesPerFrame);
    tile_buffer_.reserve(static_cast<std::size_t>(kTileSize) * kTileSize * kBytesPerPixel);
}

void FrameReceiver::SetFrameError(FrameAssemblyError& error,
                                  FrameAckStatus status,
                                  KeyFrameRequestReason reason,
                                  std::uint64_t failed_frame_id,
                                  const std::string& message) const {
    error.status = status;
    error.key_frame_reason = reason;
    error.failed_frame_id = failed_frame_id;
    error.message = message;
}

// 检查尺寸、frame_id 与 base_frame_id；只有完整 I 帧才能重建丢失的 P 帧依赖。
bool FrameReceiver::Begin(const FrameBeginPayload& message, FrameAssemblyError& error) {
    if (has_begin_) {
        std::ostringstream out;
        out << "new frame " << message.frame_id << " arrived before frame "
            << begin_.frame_id << " completed";
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::FrameInterrupted,
                      begin_.frame_id, out.str());
        return false;
    }

    if (message.width != kFrameWidth || message.height != kFrameHeight ||
        message.stride != kFrameStride) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id, "frame geometry is not 720x720 BGRA");
        return false;
    }
    if (message.changed_tile_count > kMaxTilesPerFrame) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id,
                      "changed tile count is outside the supported range");
        return false;
    }

    const FrameType frame_type = static_cast<FrameType>(message.frame_type);
    if (frame_type != FrameType::IFrame && frame_type != FrameType::PFrame) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id, "unsupported frame type");
        return false;
    }

    if (frame_type == FrameType::IFrame) {
        if (message.base_frame_id != 0) {
            SetFrameError(error, FrameAckStatus::InvalidFrame,
                          KeyFrameRequestReason::InvalidFrame,
                          message.frame_id, "I frame must have base_frame_id=0");
            return false;
        }
        std::fill(back_.begin(), back_.end(), 0);
    } else {
        if (need_key_frame_ || last_good_frame_id_ == 0 ||
            message.base_frame_id != last_good_frame_id_) {
            std::ostringstream out;
            out << "P frame base mismatch: frame=" << message.frame_id
                << " base=" << message.base_frame_id
                << " last_good=" << last_good_frame_id_
                << " need_key=" << (need_key_frame_ ? 1 : 0);
            SetFrameError(error, FrameAckStatus::InvalidFrame,
                          KeyFrameRequestReason::BaseMismatch,
                          message.frame_id, out.str());
            return false;
        }
        back_ = front_;
    }

    begin_ = message;
    has_begin_ = true;
    received_tiles_ = 0;
    received_rectangles_.clear();
    start_time_ = std::chrono::steady_clock::now();
    return true;
}

bool FrameReceiver::RequirePending(std::uint64_t frame_id,
                                   FrameAssemblyError& error) const {
    if (!has_begin_) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      frame_id, "frame message arrived without FRAME_BEGIN");
        return false;
    }
    if (begin_.frame_id != frame_id) {
        std::ostringstream out;
        out << "frame id " << frame_id << " does not match pending frame "
            << begin_.frame_id;
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::FrameInterrupted,
                      frame_id, out.str());
        return false;
    }
    return true;
}

bool FrameReceiver::DecodeRleBgra32(const std::uint8_t* encoded_data,
                                    std::size_t encoded_size,
                                    std::size_t expected_size,
                                    FrameAssemblyError& error) {
    if ((expected_size % kPixelBytes) != 0) {
        SetFrameError(error, FrameAckStatus::DecodeFailed,
                      KeyFrameRequestReason::DecodeError,
                      PendingFrameId(), "expected size is not a BGRA byte count");
        return false;
    }

    tile_buffer_.resize(expected_size);
    std::size_t source_pos = 0;
    std::size_t output_pos = 0;

    while (source_pos < encoded_size) {
        const std::uint8_t token = encoded_data[source_pos++];
        const std::size_t pixel_count = static_cast<std::size_t>(token & 0x7Fu) + 1u;
        if (pixel_count > kMaxRlePacketPixels) {
            SetFrameError(error, FrameAckStatus::DecodeFailed,
                          KeyFrameRequestReason::DecodeError,
                          PendingFrameId(), "invalid RLE packet length");
            return false;
        }
        const std::size_t byte_count = pixel_count * kPixelBytes;

        if ((token & 0x80u) != 0) {
            if (source_pos + kPixelBytes > encoded_size) {
                SetFrameError(error, FrameAckStatus::DecodeFailed,
                              KeyFrameRequestReason::DecodeError,
                              PendingFrameId(), "truncated repeated-pixel packet");
                return false;
            }
            if (output_pos + byte_count > expected_size) {
                SetFrameError(error, FrameAckStatus::DecodeFailed,
                              KeyFrameRequestReason::DecodeError,
                              PendingFrameId(), "RLE output exceeds expected size");
                return false;
            }
            const std::uint8_t* pixel = encoded_data + source_pos;
            source_pos += kPixelBytes;
            for (std::size_t i = 0; i < pixel_count; ++i) {
                std::memcpy(tile_buffer_.data() + output_pos + i * kPixelBytes,
                            pixel, kPixelBytes);
            }
            output_pos += byte_count;
        } else {
            if (source_pos + byte_count > encoded_size) {
                SetFrameError(error, FrameAckStatus::DecodeFailed,
                              KeyFrameRequestReason::DecodeError,
                              PendingFrameId(), "truncated literal packet");
                return false;
            }
            if (output_pos + byte_count > expected_size) {
                SetFrameError(error, FrameAckStatus::DecodeFailed,
                              KeyFrameRequestReason::DecodeError,
                              PendingFrameId(), "RLE output exceeds expected size");
                return false;
            }
            std::memcpy(tile_buffer_.data() + output_pos,
                        encoded_data + source_pos, byte_count);
            source_pos += byte_count;
            output_pos += byte_count;
        }
    }

    if (output_pos != expected_size) {
        std::ostringstream out;
        out << "decoded " << output_pos << " bytes, expected " << expected_size;
        SetFrameError(error, FrameAckStatus::DecodeFailed,
                      KeyFrameRequestReason::DecodeError,
                      PendingFrameId(), out.str());
        return false;
    }
    return true;
}

// Tile 解码后写入 back_，此阶段不更新对外展示的 front_。
bool FrameReceiver::ApplyTile(const TilePayloadHeader& message,
                              const std::uint8_t* encoded_data,
                              std::size_t encoded_size,
                              FrameAssemblyError& error) {
    if (!RequirePending(message.frame_id, error)) return false;
    if (received_tiles_ >= begin_.changed_tile_count) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id,
                      "received more tiles than FRAME_BEGIN declared");
        return false;
    }
    if (message.width == 0 || message.height == 0 ||
        static_cast<std::uint32_t>(message.x) + message.width > kFrameWidth ||
        static_cast<std::uint32_t>(message.y) + message.height > kFrameHeight) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id, "tile rectangle is outside the frame");
        return false;
    }

    const std::uint64_t rectangle = RectangleKey(
        message.x, message.y, message.width, message.height);
    if (received_rectangles_.find(rectangle) != received_rectangles_.end()) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id, "duplicate tile rectangle");
        return false;
    }

    const std::uint64_t expected64 = static_cast<std::uint64_t>(message.width) *
                                     message.height * kBytesPerPixel;
    if (expected64 > std::numeric_limits<std::size_t>::max() ||
        message.raw_size != expected64) {
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id,
                      "tile raw size does not match its rectangle");
        return false;
    }
    const std::size_t expected_size = static_cast<std::size_t>(expected64);

    const std::uint8_t* decoded = nullptr;
    const TileCodec codec = static_cast<TileCodec>(message.codec);
    if (codec == TileCodec::RawBgra) {
        if (encoded_size != expected_size) {
            SetFrameError(error, FrameAckStatus::DecodeFailed,
                          KeyFrameRequestReason::DecodeError,
                          message.frame_id, "raw tile byte count is invalid");
            return false;
        }
        decoded = encoded_data;
    } else if (codec == TileCodec::RleBgra32) {
        if (!DecodeRleBgra32(encoded_data, encoded_size, expected_size, error)) {
            return false;
        }
        decoded = tile_buffer_.data();
    } else {
        SetFrameError(error, FrameAckStatus::DecodeFailed,
                      KeyFrameRequestReason::DecodeError,
                      message.frame_id, "unsupported tile codec");
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(message.width) * kBytesPerPixel;
    for (std::uint32_t row = 0; row < message.height; ++row) {
        const std::size_t source_offset = static_cast<std::size_t>(row) * row_bytes;
        const std::size_t destination_offset =
            (static_cast<std::size_t>(message.y) + row) * kFrameStride +
            static_cast<std::size_t>(message.x) * kBytesPerPixel;
        std::memcpy(back_.data() + destination_offset,
                    decoded + source_offset, row_bytes);
    }

    received_rectangles_.insert(rectangle);
    ++received_tiles_;
    return true;
}

// 完整 Tile 数、CRC 与帧尾校验通过后交换前后缓冲，保证消费者只看到完整帧。
bool FrameReceiver::End(const FrameEndPayload& message,
                        CompletedFrame& completed,
                        FrameAssemblyError& error) {
    if (!RequirePending(message.frame_id, error)) return false;
    if (received_tiles_ != begin_.changed_tile_count) {
        std::ostringstream out;
        out << "received " << received_tiles_ << " tiles, expected "
            << begin_.changed_tile_count;
        SetFrameError(error, FrameAckStatus::InvalidFrame,
                      KeyFrameRequestReason::InvalidFrame,
                      message.frame_id, out.str());
        return false;
    }

    const std::uint32_t checksum = Crc32(back_.data(), back_.size());
    if (checksum != message.frame_crc32) {
        std::ostringstream out;
        out << "frame CRC32 mismatch: got 0x" << std::hex << checksum
            << ", expected 0x" << message.frame_crc32;
        SetFrameError(error, FrameAckStatus::ChecksumMismatch,
                      KeyFrameRequestReason::ChecksumMismatch,
                      message.frame_id, out.str());
        return false;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start_time_).count();
    const FrameType completed_type = static_cast<FrameType>(begin_.frame_type);
    std::swap(front_, back_);

    completed.frame_id = begin_.frame_id;
    completed.timestamp_ns = begin_.timestamp_ns;
    completed.received_tile_count = received_tiles_;
    completed.elapsed_us = elapsed <= 0 ? 0u : static_cast<std::uint32_t>(
        std::min<std::int64_t>(elapsed, std::numeric_limits<std::uint32_t>::max()));
    completed.frame_type = completed_type;

    last_good_frame_id_ = begin_.frame_id;
    need_key_frame_ = false;
    ResetPending();
    return true;
}

void FrameReceiver::ResetPending() {
    has_begin_ = false;
    begin_ = FrameBeginPayload{};
    received_tiles_ = 0;
    received_rectangles_.clear();
    start_time_ = std::chrono::steady_clock::time_point{};
}

void FrameReceiver::EnterResync() {
    ResetPending();
    need_key_frame_ = true;
}

// Session 重置必须同时清空 pending 和 P 帧基线，防止跨会话复用旧画面。
void FrameReceiver::ResetStream() {
    ResetPending();
    last_good_frame_id_ = 0;
    need_key_frame_ = true;
}

double FrameReceiver::PendingAgeSeconds() const {
    if (!has_begin_) return 0.0;
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start_time_).count();
}

} // namespace kavio_virtual_hdmi
