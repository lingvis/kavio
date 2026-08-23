#pragma once

#include <kavio/Types.h>
#include "ShmBase.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kavio {

constexpr uint32_t kEncodedStreamShmMagic = 0x4553484D;  // ESHM
constexpr uint32_t kEncodedStreamDefaultPacketCapacity = 16;
constexpr uint32_t kEncodedStreamDefaultPayloadCapacity = 4 * 1024 * 1024;

inline uint32_t EncodedAtomicLoadU32(const uint32_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void EncodedAtomicStoreU32(uint32_t* p, uint32_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

inline uint64_t EncodedAtomicLoadU64(const uint64_t* p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

inline void EncodedAtomicStoreU64(uint64_t* p, uint64_t v) {
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

inline uint64_t EncodedAtomicFetchAddU64(uint64_t* p, uint64_t v) {
    return __atomic_fetch_add(p, v, __ATOMIC_ACQ_REL);
}

inline void EncodedAtomicThreadFenceRelease() {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

inline void EncodedAtomicThreadFenceAcquire() {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}


inline bool EncodedPacketIsH264StreamData(const uint8_t* data, size_t size) {
    return data && size >= 5 &&
           data[0] == 0x00 && data[1] == 0x00 &&
           data[2] == 0x00 && data[3] == 0x01;
}

inline uint32_t EncodedPacketFirstH264NalType(const uint8_t* data, size_t size) {
    if (!EncodedPacketIsH264StreamData(data, size)) {
        return 0;
    }
    return data[4] & 0x1F;
}

enum EncodedPacketFlags : uint32_t {
    ENCODED_PACKET_FLAG_KEY_FRAME = 1u << 0,
    ENCODED_PACKET_FLAG_IDR       = 1u << 1,
    ENCODED_PACKET_FLAG_SPS       = 1u << 2,
    ENCODED_PACKET_FLAG_PPS       = 1u << 3,
    ENCODED_PACKET_FLAG_HEADER    = 1u << 4,
};

struct EncodedStreamHeader {
    uint32_t magic = kEncodedStreamShmMagic;

    // 固定布局头大小。布局通过 magic 和结构尺寸校验。
    uint32_t header_size = 0;
    uint32_t packet_meta_size = 0;
    uint32_t reserved_layout = 0;

    uint32_t stream_id = 0;
    uint32_t session_id = 0;

    uint32_t codec = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t gop = 0;

    uint32_t packet_capacity = 0;
    uint32_t payload_capacity = 0;
    uint32_t total_shm_size = 0;

    uint64_t write_seq = 0;
    uint32_t write_index = 0;
    uint32_t payload_write_offset = 0;
    uint64_t reserved0 = 0;
    uint64_t reserved1 = 0;
};

struct EncodedPacketMeta {
    uint32_t writing = 0;
    uint32_t valid = 0;

    uint64_t seq = 0;
    uint64_t pts_us = 0;
    uint64_t dts_us = 0;
    uint64_t timestamp_ns = 0;

    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t flags = 0;
    uint32_t nal_type = 0;
};

static_assert(sizeof(EncodedStreamHeader) == 96, "Unexpected EncodedStreamHeader layout");
static_assert(sizeof(EncodedPacketMeta) == 56, "Unexpected EncodedPacketMeta layout");

struct EncodedPacketWriteResult {
    bool ok = false;
    uint64_t seq = 0;
    uint32_t index = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
};

struct EncodedPacket {
    uint64_t seq = 0;
    uint64_t pts_us = 0;
    uint64_t dts_us = 0;
    uint64_t timestamp_ns = 0;
    uint32_t flags = 0;
    uint32_t nal_type = 0;
    std::vector<uint8_t> data;
};

class EncodedStreamShm final : public ShmBase {
public:
    struct CreateConfig {
        std::string name;
        uint32_t session_id = 0;
        uint32_t stream_id = 0;
        uint32_t codec = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t fps = 0;
        uint32_t bitrate_kbps = 0;
        uint32_t gop = 0;
        uint32_t packet_capacity = kEncodedStreamDefaultPacketCapacity;
        uint32_t payload_capacity = kEncodedStreamDefaultPayloadCapacity;
    };

    struct OpenConfig {
        std::string name;

        // 0 表示按 POSIX SHM 实际大小映射；非 0 时按指定大小映射并校验底层对象不小于该值。
        size_t size = 0;
        ShmAccess access = ShmAccess::ReadOnly;
    };

    EncodedStreamShm();
    ~EncodedStreamShm() override;

    EncodedStreamShm(const EncodedStreamShm&) = delete;
    EncodedStreamShm& operator=(const EncodedStreamShm&) = delete;

    EncodedStreamShm(EncodedStreamShm&& other) noexcept;
    EncodedStreamShm& operator=(EncodedStreamShm&& other) noexcept;

    static std::unique_ptr<EncodedStreamShm> Create(
        const CreateConfig& config,
        std::string* out_error = nullptr);

    static std::unique_ptr<EncodedStreamShm> Open(
        const OpenConfig& config,
        std::string* out_error = nullptr);

    EncodedPacketWriteResult WritePacket(const void* data,
                                         uint32_t size,
                                         uint64_t pts_us,
                                         uint64_t dts_us,
                                         uint64_t timestamp_ns,
                                         uint32_t flags,
                                         uint32_t nal_type);

    Status ReadPacket(uint64_t seq, EncodedPacket& out_packet) const;
    uint64_t LatestSeq() const;

    const EncodedStreamHeader* Header() const { return header_; }
    EncodedStreamHeader* Header() { return header_; }

    static size_t MinimumHeaderSize(uint32_t packet_capacity);
    static size_t ComputeTotalSize(uint32_t packet_capacity,
                                   uint32_t payload_capacity);

private:
    bool CreateInternal(const CreateConfig& config,
                        std::string* out_error);

    bool OpenInternal(const OpenConfig& config,
                      std::string* out_error);

    bool AttachLayout(void* base,
                      size_t size,
                      uint32_t packet_capacity,
                      std::string* out_error);

    bool AttachExistingLayout(void* base,
                              size_t size,
                              std::string* out_error);
    bool ValidateLayout(std::string* out_error) const;

    bool InitializeLayout(uint32_t session_id,
                          uint32_t stream_id,
                          uint32_t codec,
                          uint32_t width,
                          uint32_t height,
                          uint32_t fps,
                          uint32_t bitrate_kbps,
                          uint32_t gop,
                          uint32_t packet_capacity,
                          uint32_t payload_capacity,
                          uint32_t total_size,
                          std::string* out_error);

    void ResetLayout() noexcept override;

    EncodedStreamHeader* header_ = nullptr;
    EncodedPacketMeta* packets_ = nullptr;
    uint8_t* payload_ = nullptr;
};

}  // namespace kavio
