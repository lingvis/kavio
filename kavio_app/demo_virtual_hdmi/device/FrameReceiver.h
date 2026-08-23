#pragma once

#include "VirtualHdmiProtocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace kavio_virtual_hdmi {

struct FrameAssemblyError {
    FrameAckStatus status = FrameAckStatus::InvalidFrame;
    KeyFrameRequestReason key_frame_reason = KeyFrameRequestReason::InvalidFrame;
    std::uint64_t failed_frame_id = 0;
    std::string message;
};

struct CompletedFrame {
    std::uint64_t frame_id = 0;
    std::uint64_t timestamp_ns = 0;
    std::uint32_t received_tile_count = 0;
    std::uint32_t elapsed_us = 0;
    FrameType frame_type = FrameType::IFrame;
};

// FrameReceiver runs only on the Device main/assembly thread.
// front_ is the latest CRC-valid dependency baseline. A P frame is accepted only
// when base_frame_id exactly matches last_good_frame_id_. Once any dependency or
// assembly error occurs the receiver enters resync and rejects P frames until a
// complete CRC-valid I frame arrives.
class FrameReceiver {
public:
    FrameReceiver();

    bool Begin(const FrameBeginPayload& message, FrameAssemblyError& error);
    bool ApplyTile(const TilePayloadHeader& message,
                   const std::uint8_t* encoded_data,
                   std::size_t encoded_size,
                   FrameAssemblyError& error);
    bool End(const FrameEndPayload& message,
             CompletedFrame& completed,
             FrameAssemblyError& error);

    // Drops only the currently incomplete frame. The last valid baseline remains.
    void ResetPending();
    // Enters resynchronization state. Existing pixels may remain on screen, but
    // no P frame is accepted until a valid I frame re-establishes the baseline.
    void EnterResync();
    // Used on channel/session reset; there is no valid dependency across sessions.
    void ResetStream();

    bool HasPendingFrame() const { return has_begin_; }
    std::uint64_t PendingFrameId() const { return has_begin_ ? begin_.frame_id : 0; }
    std::uint64_t LastGoodFrameId() const { return last_good_frame_id_; }
    bool NeedsKeyFrame() const { return need_key_frame_; }
    double PendingAgeSeconds() const;

    const std::vector<std::uint8_t>& Pixels() const { return front_; }

private:
    bool RequirePending(std::uint64_t frame_id, FrameAssemblyError& error) const;
    bool DecodeRleBgra32(const std::uint8_t* encoded_data,
                         std::size_t encoded_size,
                         std::size_t expected_size,
                         FrameAssemblyError& error);
    void SetFrameError(FrameAssemblyError& error,
                       FrameAckStatus status,
                       KeyFrameRequestReason reason,
                       std::uint64_t failed_frame_id,
                       const std::string& message) const;

    std::vector<std::uint8_t> front_;
    std::vector<std::uint8_t> back_;
    std::vector<std::uint8_t> tile_buffer_;
    FrameBeginPayload begin_{};
    bool has_begin_ = false;
    bool need_key_frame_ = true;
    std::uint64_t last_good_frame_id_ = 0;
    std::uint32_t received_tiles_ = 0;
    std::unordered_set<std::uint64_t> received_rectangles_;
    std::chrono::steady_clock::time_point start_time_{};
};

} // namespace kavio_virtual_hdmi
