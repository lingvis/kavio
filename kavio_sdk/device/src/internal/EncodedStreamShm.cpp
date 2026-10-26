#include "internal/EncodedStreamShm.h"

#include <cstring>
#include <limits>
#include <utility>

namespace kavio {

EncodedStreamShm::EncodedStreamShm()
    : ShmBase(ShmKind::EncodedStream) {
}

EncodedStreamShm::~EncodedStreamShm() {
    Close();
}

EncodedStreamShm::EncodedStreamShm(EncodedStreamShm&& other) noexcept
    : ShmBase(std::move(other)),
      header_(other.header_),
      packets_(other.packets_),
      payload_(other.payload_) {
    other.ResetLayout();
}

EncodedStreamShm& EncodedStreamShm::operator=(EncodedStreamShm&& other) noexcept {
    if (this != &other) {
        Close();
        ShmBase::operator=(std::move(other));
        header_ = other.header_;
        packets_ = other.packets_;
        payload_ = other.payload_;
        other.ResetLayout();
    }
    return *this;
}

std::unique_ptr<EncodedStreamShm> EncodedStreamShm::Create(
    const CreateConfig& config,
    std::string* out_error) {
    auto shm = std::make_unique<EncodedStreamShm>();
    if (!shm->CreateInternal(config, out_error)) {
        return nullptr;
    }
    return shm;
}

std::unique_ptr<EncodedStreamShm> EncodedStreamShm::Open(
    const OpenConfig& config,
    std::string* out_error) {
    auto shm = std::make_unique<EncodedStreamShm>();
    if (!shm->OpenInternal(config, out_error)) {
        return nullptr;
    }
    return shm;
}

void EncodedStreamShm::ResetLayout() noexcept {
    header_ = nullptr;
    packets_ = nullptr;
    payload_ = nullptr;
}

EncodedPacketWriteResult EncodedStreamShm::WritePacket(const void* data,
                                                       uint32_t size,
                                                       uint64_t pts_us,
                                                       uint64_t dts_us,
                                                       uint64_t timestamp_ns,
                                                       uint32_t flags,
                                                       uint32_t nal_type) {
    std::lock_guard<std::mutex> writer_lock(writer_mutex_);
    EncodedPacketWriteResult result;
    if (!IsWritable() || !IsValid() || !data || size == 0 ||
        size > header_->payload_capacity) return result;
    if (header_->codec == 1 &&
        !EncodedPacketIsH264StreamData(static_cast<const uint8_t*>(data), size)) return result;

    uint32_t offset = EncodedAtomicLoadU32(&header_->payload_write_offset);
    if (static_cast<uint64_t>(offset) + size > header_->payload_capacity) offset = 0;
    const uint64_t seq = EncodedAtomicLoadU64(&header_->write_seq) + 1;
    const uint32_t index = static_cast<uint32_t>(seq % header_->packet_capacity);
    EncodedPacketMeta* meta = packets_ + index;

    // A packet can be overwritten by *payload* wrap long before its metadata
    // slot is recycled. Invalidate every overlapping slot before copying bytes.
    const uint64_t epoch = EncodedAtomicLoadU64(&header_->reserved0);
    EncodedAtomicStoreU64(&header_->reserved0, (epoch & ~uint64_t(1)) + 1);
    for (uint32_t i = 0; i < header_->packet_capacity; ++i) {
        auto& old = packets_[i];
        if (i == index || EncodedAtomicLoadU32(&old.valid) == 0) continue;
        const uint64_t old_end = static_cast<uint64_t>(old.offset) + old.size;
        const uint64_t new_end = static_cast<uint64_t>(offset) + size;
        if (static_cast<uint64_t>(old.offset) < new_end &&
            static_cast<uint64_t>(offset) < old_end) {
            EncodedAtomicStoreU32(&old.valid, 0);
        }
    }
    EncodedAtomicStoreU32(&meta->writing, 1);
    EncodedAtomicStoreU32(&meta->valid, 0);
    std::memcpy(payload_ + offset, data, size);
    meta->seq = seq;
    meta->pts_us = pts_us;
    meta->dts_us = dts_us;
    meta->timestamp_ns = timestamp_ns;
    meta->offset = offset;
    meta->size = size;
    meta->flags = flags;
    meta->nal_type = nal_type;
    EncodedAtomicStoreU32(&meta->valid, 1);
    EncodedAtomicStoreU32(&meta->writing, 0);
    EncodedAtomicStoreU32(&header_->write_index, index);
    EncodedAtomicStoreU32(&header_->payload_write_offset, offset + size);
    EncodedAtomicStoreU64(&header_->write_seq, seq);
    EncodedAtomicStoreU64(&header_->reserved0, (epoch & ~uint64_t(1)) + 2);
    result.ok = true;
    result.seq = seq;
    result.index = index;
    result.offset = offset;
    result.size = size;
    return result;
}

Status EncodedStreamShm::ReadPacket(uint64_t seq, EncodedPacket& out_packet) const {
    out_packet = EncodedPacket{};
    if (!IsValid() || seq == 0) {
        return Status::Error(StatusCode::Common_InvalidState, "encoded stream shm is not valid");
    }
    const uint64_t start_epoch = EncodedAtomicLoadU64(&header_->reserved0);
    if (start_epoch & 1U) {
        return Status::Error(StatusCode::Media_NoFrame, "encoded stream is being updated");
    }
    const uint32_t packet_capacity = EncodedAtomicLoadU32(&header_->packet_capacity);
    const uint32_t payload_capacity = EncodedAtomicLoadU32(&header_->payload_capacity);
    const uint32_t index = static_cast<uint32_t>(seq % packet_capacity);
    const EncodedPacketMeta* meta = packets_ + index;
    if (EncodedAtomicLoadU32(&meta->writing) != 0 ||
        EncodedAtomicLoadU32(&meta->valid) == 0) {
        return Status::Error(StatusCode::Media_NoFrame, "encoded packet is not ready");
    }
    EncodedAtomicThreadFenceAcquire();
    EncodedPacket snapshot;
    snapshot.seq = meta->seq;
    snapshot.pts_us = meta->pts_us;
    snapshot.dts_us = meta->dts_us;
    snapshot.timestamp_ns = meta->timestamp_ns;
    snapshot.flags = meta->flags;
    snapshot.nal_type = meta->nal_type;
    const uint32_t offset = meta->offset;
    const uint32_t size = meta->size;
    if (snapshot.seq != seq) {
        return Status::Error(StatusCode::Media_NoFrame, "encoded packet has been overwritten");
    }
    if (size == 0 || offset >= payload_capacity ||
        static_cast<uint64_t>(offset) + size > payload_capacity) {
        return Status::Error(StatusCode::SHM_OperationFailed, "encoded packet payload is invalid");
    }
    snapshot.data.resize(size);
    std::memcpy(snapshot.data.data(), payload_ + offset, size);
    EncodedAtomicThreadFenceAcquire();
    if (start_epoch != EncodedAtomicLoadU64(&header_->reserved0) ||
        EncodedAtomicLoadU32(&meta->writing) != 0 ||
        EncodedAtomicLoadU32(&meta->valid) == 0 ||
        meta->seq != seq) {
        return Status::Error(StatusCode::Media_NoFrame, "encoded packet changed while reading");
    }
    out_packet = std::move(snapshot);
    return Status::Ok();
}

uint64_t EncodedStreamShm::LatestSeq() const {
    if (!header_) {
        return 0;
    }
    return EncodedAtomicLoadU64(&header_->write_seq);
}


size_t EncodedStreamShm::MinimumHeaderSize(uint32_t packet_capacity) {
    return sizeof(EncodedStreamHeader) +
           static_cast<size_t>(packet_capacity) * sizeof(EncodedPacketMeta);
}

size_t EncodedStreamShm::ComputeTotalSize(uint32_t packet_capacity,
                                          uint32_t payload_capacity) {
    if (packet_capacity == 0 || payload_capacity == 0) {
        return 0;
    }
    return MinimumHeaderSize(packet_capacity) + payload_capacity;
}

bool EncodedStreamShm::CreateInternal(const CreateConfig& config,
                                      std::string* out_error) {
    if (config.name.empty() || config.session_id == 0 || config.stream_id == 0 ||
        config.width == 0 || config.height == 0 || config.fps == 0 ||
        config.bitrate_kbps == 0) {
        SetError(out_error, "invalid encoded stream shm arguments");
        return false;
    }

    const uint32_t packet_capacity = config.packet_capacity == 0
        ? kEncodedStreamDefaultPacketCapacity
        : config.packet_capacity;
    const uint32_t payload_capacity = config.payload_capacity == 0
        ? kEncodedStreamDefaultPayloadCapacity
        : config.payload_capacity;

    const size_t total_size = ComputeTotalSize(packet_capacity, payload_capacity);
    if (total_size == 0 ||
        total_size > static_cast<size_t>(std::numeric_limits<int32_t>::max()) * 8ULL) {
        SetError(out_error, "encoded stream shm size is invalid");
        return false;
    }

    if (!CreateMapping(config.name,
                       total_size,
                       CreatePolicy::Recreate,
                       true,
                       out_error)) {
        return false;
    }

    const auto fail_create = [this, &config]() {
        Close();
        (void)UnlinkByName(config.name, nullptr);
        return false;
    };

    std::memset(Address(), 0, Size());
    if (!AttachLayout(Address(), Size(), packet_capacity, out_error)) {
        return fail_create();
    }
    if (!InitializeLayout(config.session_id,
                          config.stream_id,
                          config.codec,
                          config.width,
                          config.height,
                          config.fps,
                          config.bitrate_kbps,
                          config.gop,
                          packet_capacity,
                          payload_capacity,
                          static_cast<uint32_t>(total_size),
                          out_error)) {
        return fail_create();
    }
    if (!ValidateLayout(out_error)) {
        return fail_create();
    }

    SetLayoutValid(true);
    return true;
}

bool EncodedStreamShm::OpenInternal(const OpenConfig& config,
                                    std::string* out_error) {
    if (config.name.empty()) {
        SetError(out_error, "invalid encoded stream open arguments");
        return false;
    }

    // 当前 EncodedStreamShm 的已有对象打开路径仅用于消费编码流。
    // Writer 必须通过 Create 建立并初始化 ring/payload 布局。
    if (config.access != ShmAccess::ReadOnly) {
        SetError(out_error, "encoded stream existing SHM only supports read-only open");
        return false;
    }

    const bool opened = config.size == 0
        ? OpenWholeMapping(config.name, sizeof(EncodedStreamHeader), config.access, out_error)
        : OpenMapping(config.name, config.size, config.access, out_error);
    if (!opened) {
        return false;
    }
    if (!AttachExistingLayout(Address(), Size(), out_error)) {
        Close();
        return false;
    }

    SetLayoutValid(true);
    return true;
}

bool EncodedStreamShm::AttachLayout(void* base,
                                    size_t size,
                                    uint32_t packet_capacity,
                                    std::string* out_error) {
    if (!base || packet_capacity == 0 || size < MinimumHeaderSize(packet_capacity)) {
        SetError(out_error, "invalid encoded stream shm layout");
        return false;
    }
    auto* base_u8 = static_cast<uint8_t*>(base);
    header_ = reinterpret_cast<EncodedStreamHeader*>(base_u8);
    packets_ = reinterpret_cast<EncodedPacketMeta*>(base_u8 + sizeof(EncodedStreamHeader));
    payload_ = base_u8 + MinimumHeaderSize(packet_capacity);
    return true;
}

bool EncodedStreamShm::AttachExistingLayout(void* base,
                                            size_t size,
                                            std::string* out_error) {
    if (!base || size < sizeof(EncodedStreamHeader)) {
        SetError(out_error, "invalid encoded stream readonly layout");
        return false;
    }

    auto* header = reinterpret_cast<EncodedStreamHeader*>(base);
    const uint32_t packet_capacity = EncodedAtomicLoadU32(&header->packet_capacity);
    if (!AttachLayout(base, size, packet_capacity, out_error)) {
        return false;
    }
    return ValidateLayout(out_error);
}

bool EncodedStreamShm::ValidateLayout(std::string* out_error) const {
    if (!IsOpen() || !header_ || !packets_ || !payload_) {
        SetError(out_error, "encoded stream layout is not attached");
        return false;
    }
    if (EncodedAtomicLoadU32(&header_->magic) != kEncodedStreamShmMagic) {
        SetError(out_error, "encoded stream shm magic mismatch");
        return false;
    }
    if (EncodedAtomicLoadU32(&header_->header_size) != sizeof(EncodedStreamHeader) ||
        EncodedAtomicLoadU32(&header_->packet_meta_size) != sizeof(EncodedPacketMeta)) {
        SetError(out_error, "encoded stream shm fixed layout size mismatch");
        return false;
    }
    const uint32_t packet_capacity = EncodedAtomicLoadU32(&header_->packet_capacity);
    const uint32_t payload_capacity = EncodedAtomicLoadU32(&header_->payload_capacity);
    const size_t expected_size = ComputeTotalSize(packet_capacity, payload_capacity);
    if (packet_capacity == 0 || payload_capacity == 0 || expected_size == 0 ||
        EncodedAtomicLoadU32(&header_->total_shm_size) != expected_size ||
        Size() < expected_size) {
        SetError(out_error, "encoded stream shm size/layout mismatch");
        return false;
    }
    return true;
}

bool EncodedStreamShm::InitializeLayout(uint32_t session_id,
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
                                        std::string* out_error) {
    if (!header_ || !packets_ || !payload_) {
        SetError(out_error, "encoded stream layout is not attached");
        return false;
    }

    header_->magic = kEncodedStreamShmMagic;
    header_->header_size = static_cast<uint32_t>(sizeof(EncodedStreamHeader));
    header_->packet_meta_size = static_cast<uint32_t>(sizeof(EncodedPacketMeta));
    header_->reserved_layout = 0;
    header_->stream_id = stream_id;
    header_->session_id = session_id;
    header_->codec = codec;
    header_->width = width;
    header_->height = height;
    header_->fps = fps;
    header_->bitrate_kbps = bitrate_kbps;
    header_->gop = gop;
    header_->packet_capacity = packet_capacity;
    header_->payload_capacity = payload_capacity;
    header_->total_shm_size = total_size;
    header_->write_seq = 0;
    header_->reserved0 = 0;
    header_->write_index = 0;
    header_->payload_write_offset = 0;
    return true;
}



}  // namespace kavio
