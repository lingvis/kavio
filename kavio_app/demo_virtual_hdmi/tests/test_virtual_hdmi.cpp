#include "FrameReceiver.h"
#include "VirtualHdmiProtocol.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

using namespace kavio_virtual_hdmi;

namespace {

std::uint32_t Crc32(const std::vector<std::uint8_t>& data) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t value : data) {
        crc ^= value;
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

FrameBeginPayload BeginPayload(std::uint64_t id,
                               std::uint64_t base_id,
                               std::uint32_t tiles,
                               FrameType frame_type) {
    FrameBeginPayload begin{};
    begin.frame_id = id;
    begin.base_frame_id = base_id;
    begin.timestamp_ns = id * 100;
    begin.width = kFrameWidth;
    begin.height = kFrameHeight;
    begin.stride = kFrameStride;
    begin.changed_tile_count = tiles;
    begin.frame_type = static_cast<std::uint8_t>(frame_type);
    return begin;
}

TilePayloadHeader TileHeader(std::uint64_t id,
                             std::uint16_t x, std::uint16_t y,
                             std::uint16_t width, std::uint16_t height,
                             TileCodec codec,
                             std::uint32_t encoded_size) {
    TilePayloadHeader tile{};
    tile.frame_id = id;
    tile.x = x;
    tile.y = y;
    tile.width = width;
    tile.height = height;
    tile.codec = static_cast<std::uint8_t>(codec);
    tile.raw_size = static_cast<std::uint32_t>(width) * height * kBytesPerPixel;
    tile.encoded_size = encoded_size;
    return tile;
}

void TestPackedProtocolLayout() {
    static_assert(kProtocolVersion == 1, "streaming protocol version");
    static_assert(sizeof(MessageHeader) == 12, "MessageHeader size");
    static_assert(sizeof(AppReadyPayload) == 20, "AppReadyPayload size");
    static_assert(sizeof(FrameBeginPayload) == 44, "FrameBeginPayload size");
    static_assert(sizeof(TilePayloadHeader) == 28, "TilePayloadHeader size");
    static_assert(sizeof(FrameEndPayload) == 16, "FrameEndPayload size");
    static_assert(sizeof(FrameAckPayload) == 20, "FrameAckPayload size");
    static_assert(sizeof(KeyFrameRequestPayload) == 24, "KeyFrameRequestPayload size");
}

void TestIFrameAndPFrameDependency() {
    FrameReceiver receiver;
    FrameAssemblyError error;
    CompletedFrame completed;

    const std::vector<std::uint8_t> raw = {
        0x01, 0x02, 0x03, 0xff,
        0x01, 0x02, 0x03, 0xff,
        0x01, 0x02, 0x03, 0xff,
        0x01, 0x02, 0x03, 0xff,
    };
    assert(receiver.NeedsKeyFrame());
    assert(receiver.Begin(BeginPayload(1, 0, 1, FrameType::IFrame), error));
    const TilePayloadHeader first = TileHeader(1, 0, 0, 2, 2, TileCodec::RawBgra,
                                                static_cast<std::uint32_t>(raw.size()));
    assert(receiver.ApplyTile(first, raw.data(), raw.size(), error));

    std::vector<std::uint8_t> expected(kFrameBytes, 0);
    std::memcpy(expected.data(), raw.data(), 8);
    std::memcpy(expected.data() + kFrameStride, raw.data() + 8, 8);
    FrameEndPayload end1{};
    end1.frame_id = 1;
    end1.frame_crc32 = Crc32(expected);
    assert(receiver.End(end1, completed, error));
    assert(completed.frame_type == FrameType::IFrame);
    assert(receiver.Pixels() == expected);
    assert(receiver.LastGoodFrameId() == 1);
    assert(!receiver.NeedsKeyFrame());

    const std::vector<std::uint8_t> changed = {0xaa, 0xbb, 0xcc, 0xff};
    assert(receiver.Begin(BeginPayload(2, 1, 1, FrameType::PFrame), error));
    const TilePayloadHeader second = TileHeader(2, 1, 1, 1, 1, TileCodec::RawBgra,
                                                 static_cast<std::uint32_t>(changed.size()));
    assert(receiver.ApplyTile(second, changed.data(), changed.size(), error));
    std::memcpy(expected.data() + kFrameStride + 4, changed.data(), changed.size());
    FrameEndPayload end2{};
    end2.frame_id = 2;
    end2.frame_crc32 = Crc32(expected);
    assert(receiver.End(end2, completed, error));
    assert(completed.frame_type == FrameType::PFrame);
    assert(receiver.Pixels() == expected);
    assert(receiver.LastGoodFrameId() == 2);
}

void TestPFrameRequiresExactBase() {
    FrameReceiver receiver;
    FrameAssemblyError error;

    assert(!receiver.Begin(BeginPayload(2, 1, 0, FrameType::PFrame), error));
    assert(error.key_frame_reason == KeyFrameRequestReason::BaseMismatch);
    assert(receiver.NeedsKeyFrame());

    // Establish frame 10 with an all-black I frame. Zero changed tiles are legal
    // because the I-frame canvas starts from zero before CRC validation.
    assert(receiver.Begin(BeginPayload(10, 0, 0, FrameType::IFrame), error));
    std::vector<std::uint8_t> black(kFrameBytes, 0);
    FrameEndPayload end{};
    end.frame_id = 10;
    end.frame_crc32 = Crc32(black);
    CompletedFrame completed;
    assert(receiver.End(end, completed, error));
    assert(receiver.LastGoodFrameId() == 10);

    assert(!receiver.Begin(BeginPayload(12, 11, 0, FrameType::PFrame), error));
    assert(error.key_frame_reason == KeyFrameRequestReason::BaseMismatch);
    receiver.EnterResync();
    assert(receiver.NeedsKeyFrame());

    // A valid I frame always re-establishes synchronization regardless of the existing baseline.
    assert(receiver.Begin(BeginPayload(20, 0, 0, FrameType::IFrame), error));
    end.frame_id = 20;
    end.frame_crc32 = Crc32(black);
    assert(receiver.End(end, completed, error));
    assert(receiver.LastGoodFrameId() == 20);
    assert(!receiver.NeedsKeyFrame());
}

void TestChecksumFailureRequiresResync() {
    FrameReceiver receiver;
    FrameAssemblyError error;
    CompletedFrame completed;
    std::vector<std::uint8_t> black(kFrameBytes, 0);

    assert(receiver.Begin(BeginPayload(1, 0, 0, FrameType::IFrame), error));
    FrameEndPayload end{};
    end.frame_id = 1;
    end.frame_crc32 = Crc32(black);
    assert(receiver.End(end, completed, error));

    assert(receiver.Begin(BeginPayload(2, 1, 0, FrameType::PFrame), error));
    end.frame_id = 2;
    end.frame_crc32 = 0x12345678u;
    assert(!receiver.End(end, completed, error));
    assert(error.status == FrameAckStatus::ChecksumMismatch);
    assert(error.key_frame_reason == KeyFrameRequestReason::ChecksumMismatch);
    receiver.EnterResync();
    assert(receiver.NeedsKeyFrame());
    assert(receiver.LastGoodFrameId() == 1);
}

void TestRleAndDuplicateRejection() {
    FrameReceiver receiver;
    FrameAssemblyError error;

    assert(receiver.Begin(BeginPayload(10, 0, 1, FrameType::IFrame), error));
    // 0x83 = repeated run of 4 pixels, followed by one BGRA pixel.
    const std::vector<std::uint8_t> encoded = {0x83, 0x10, 0x20, 0x30, 0xff};
    const TilePayloadHeader rle = TileHeader(10, 0, 0, 2, 2, TileCodec::RleBgra32,
                                             static_cast<std::uint32_t>(encoded.size()));
    assert(receiver.ApplyTile(rle, encoded.data(), encoded.size(), error));

    receiver.ResetPending();
    assert(receiver.Begin(BeginPayload(11, 0, 2, FrameType::IFrame), error));
    const std::vector<std::uint8_t> pixel = {0, 0, 0, 0xff};
    const TilePayloadHeader tile = TileHeader(11, 0, 0, 1, 1, TileCodec::RawBgra,
                                              static_cast<std::uint32_t>(pixel.size()));
    assert(receiver.ApplyTile(tile, pixel.data(), pixel.size(), error));
    assert(!receiver.ApplyTile(tile, pixel.data(), pixel.size(), error));
    assert(error.status == FrameAckStatus::InvalidFrame);
}

void TestTruncatedRleRejected() {
    FrameReceiver receiver;
    FrameAssemblyError error;
    assert(receiver.Begin(BeginPayload(20, 0, 1, FrameType::IFrame), error));
    const std::vector<std::uint8_t> truncated = {0x80, 0x01};
    const TilePayloadHeader tile = TileHeader(20, 0, 0, 1, 1, TileCodec::RleBgra32,
                                              static_cast<std::uint32_t>(truncated.size()));
    assert(!receiver.ApplyTile(tile, truncated.data(), truncated.size(), error));
    assert(error.status == FrameAckStatus::DecodeFailed);
    assert(error.key_frame_reason == KeyFrameRequestReason::DecodeError);
}

} // namespace

int main() {
    TestPackedProtocolLayout();
    TestIFrameAndPFrameDependency();
    TestPFrameRequiresExactBase();
    TestChecksumFailureRequiresResync();
    TestRleAndDuplicateRejection();
    TestTruncatedRleRejected();
    std::cout << "demo_virtual_hdmi streaming protocol tests passed" << std::endl;
    return 0;
}
