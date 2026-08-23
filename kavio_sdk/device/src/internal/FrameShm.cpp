#include "internal/FrameShm.h"

#include <new>

/*
 * File: FrameShm.cpp
 * Brief: Implementation of FrameShm lifecycle, layout attach/init,
 *        and latest-only read/write synchronization.
 */

namespace kavio {

FrameShm::FrameShm()
    : ShmBase(ShmKind::Frame) {
}

FrameShm::~FrameShm() {
    Close();
}

FrameShm::FrameShm(FrameShm&& other) noexcept
    : ShmBase(std::move(other)),
      header_(other.header_) {
    meta_[0] = other.meta_[0];
    meta_[1] = other.meta_[1];
    other.ResetLayout();
}

FrameShm& FrameShm::operator=(FrameShm&& other) noexcept {
    if (this != &other) {
        Close();
        ShmBase::operator=(std::move(other));
        header_ = other.header_;
        meta_[0] = other.meta_[0];
        meta_[1] = other.meta_[1];
        other.ResetLayout();
    }
    return *this;
}

std::unique_ptr<FrameShm> FrameShm::Create(
    const CreateConfig& config,
    std::string* out_error) {
    auto shm = std::make_unique<FrameShm>();
    if (!shm->CreateInternal(config, out_error)) {
        return nullptr;
    }
    return shm;
}

std::unique_ptr<FrameShm> FrameShm::Open(
    const OpenConfig& config,
    std::string* out_error) {
    auto shm = std::make_unique<FrameShm>();
    if (!shm->OpenInternal(config, out_error)) {
        return nullptr;
    }
    return shm;
}

ShmBufferMeta* FrameShm::Meta(uint32_t index) {
    if (index >= kShmBufferCount) {
        return nullptr;
    }
    return meta_[index];
}

const ShmBufferMeta* FrameShm::Meta(uint32_t index) const {
    if (index >= kShmBufferCount) {
        return nullptr;
    }
    return meta_[index];
}

uint8_t* FrameShm::Payload(uint32_t index) {
    if (!IsValid() || index >= kShmBufferCount) {
        return nullptr;
    }

    uint8_t* payload0 = reinterpret_cast<uint8_t*>(Address()) + MinimumHeaderSize();
    return payload0 + static_cast<size_t>(index) * header_->payload_capacity;
}

const uint8_t* FrameShm::Payload(uint32_t index) const {
    if (!IsValid() || index >= kShmBufferCount) {
        return nullptr;
    }

    const uint8_t* payload0 = reinterpret_cast<const uint8_t*>(Address()) + MinimumHeaderSize();
    return payload0 + static_cast<size_t>(index) * header_->payload_capacity;
}

void FrameShm::ResetLayout() noexcept {
    header_ = nullptr;
    meta_[0] = nullptr;
    meta_[1] = nullptr;
}

bool FrameShm::CreateInternal(const CreateConfig& config,
                                     std::string* out_error) {
    if (config.name.empty()) {
        SetError(out_error, "shm name is empty");
        return false;
    }
    if (config.width == 0 || config.height == 0 || config.stride == 0) {
        SetError(out_error, "invalid width/height/stride");
        return false;
    }

    const size_t payload_capacity = ComputePayloadCapacity(config.height, config.stride);
    if (payload_capacity == 0 || payload_capacity > UINT32_MAX) {
        SetError(out_error, "payload_capacity overflow");
        return false;
    }

    const size_t total_size = ComputeTotalSize(static_cast<uint32_t>(payload_capacity));
    if (total_size == 0 || total_size > static_cast<size_t>(INT32_MAX) * 8ULL) {
        SetError(out_error, "total shm size is too large");
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

    if (!AttachLayout(Address(), Size(), out_error)) {
        return fail_create();
    }
    if (!InitializeLayout(config.shm_type,
                          config.pixel_format,
                          config.session_id,
                          config.shm_id,
                          config.width,
                          config.height,
                          config.stride,
                          static_cast<uint32_t>(payload_capacity),
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

bool FrameShm::OpenInternal(const OpenConfig& config,
                                   std::string* out_error) {
    if (config.name.empty()) {
        SetError(out_error, "shm name is empty");
        return false;
    }

    // FrameShm 的读侧会更新 read_in_progress/consumer_index，
    // 因此打开已有对象必须使用读写映射。
    if (!OpenWholeMapping(config.name,
                          MinimumHeaderSize(),
                          ShmAccess::ReadWrite,
                          out_error)) {
        return false;
    }

    if (!AttachLayout(Address(), Size(), out_error) ||
        !ValidateLayout(out_error)) {
        Close();
        return false;
    }

    SetLayoutValid(true);
    return true;
}

bool FrameShm::AttachLayout(void* base,
                                   size_t size,
                                   std::string* out_error) {
    if (base == nullptr || size < MinimumHeaderSize()) {
        SetError(out_error, "invalid base/size for layout attach");
        return false;
    }

    auto* base_u8 = reinterpret_cast<uint8_t*>(base);
    header_ = reinterpret_cast<SharedMemoryHeader*>(base_u8);

    meta_[0] = reinterpret_cast<ShmBufferMeta*>(base_u8 + sizeof(SharedMemoryHeader));
    meta_[1] = meta_[0] + 1;

    return true;
}

bool FrameShm::ValidateLayout(std::string* out_error) const {
    if (header_ == nullptr || !IsOpen() || meta_[0] == nullptr || meta_[1] == nullptr) {
        SetError(out_error, "shm layout is not attached");
        return false;
    }
    if (header_->magic != kShmMagic) {
        SetError(out_error, "shm magic mismatch");
        return false;
    }
    if (header_->header_size != sizeof(SharedMemoryHeader) ||
        header_->buffer_meta_size != sizeof(ShmBufferMeta)) {
        SetError(out_error, "shm fixed layout size mismatch");
        return false;
    }
    if (header_->buffer_count != kShmBufferCount || header_->payload_capacity == 0) {
        SetError(out_error, "shm buffer layout is invalid");
        return false;
    }
    const size_t expected_size = ComputeTotalSize(header_->payload_capacity);
    if (expected_size == 0 || header_->total_shm_size != expected_size || Size() < expected_size) {
        SetError(out_error, "shm total size/layout mismatch");
        return false;
    }
    return true;
}

bool FrameShm::InitializeLayout(ShmType shm_type,
                                       PixelFormat pixel_format,
                                       uint32_t session_id,
                                       uint32_t shm_id,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t stride,
                                       uint32_t payload_capacity,
                                       uint32_t total_shm_size,
                                       std::string* out_error) {
    if (!IsOpen() || header_ == nullptr) {
        SetError(out_error, "layout is not attached");
        return false;
    }

    if (Size() < ComputeTotalSize(payload_capacity)) {
        SetError(out_error, "mapped size is smaller than required layout size");
        return false;
    }

    new (header_) SharedMemoryHeader{};

    header_->magic = kShmMagic;
    header_->header_size = static_cast<uint32_t>(sizeof(SharedMemoryHeader));
    header_->buffer_meta_size = static_cast<uint32_t>(sizeof(ShmBufferMeta));
    header_->reserved_layout = 0;
    header_->shm_type = static_cast<uint32_t>(shm_type);
    header_->pixel_format = static_cast<uint32_t>(pixel_format);
    header_->session_id = session_id;
    header_->shm_id = shm_id;
    header_->buffer_count = kShmBufferCount;
    header_->width = width;
    header_->height = height;
    header_->stride = stride;
    header_->payload_capacity = payload_capacity;
    header_->total_shm_size = total_shm_size;
    header_->published_index = 0;
    header_->consumer_index = 0;

    for (uint32_t index = 0; index < kShmBufferCount; ++index) {
        new (meta_[index]) ShmBufferMeta{};
        meta_[index]->width = 0;
        meta_[index]->height = 0;
        meta_[index]->stride = 0;
        meta_[index]->pixel_format = static_cast<uint32_t>(PixelFormat::kUnknown);
        meta_[index]->valid_rect_pos_x = 0;
        meta_[index]->valid_rect_pos_y = 0;
        meta_[index]->valid_rect_width = 0;
        meta_[index]->valid_rect_height = 0;
        meta_[index]->valid = 0;
        meta_[index]->read_in_progress = 0;
        meta_[index]->dest_pos_x = 0;
        meta_[index]->dest_pos_y = 0;
    }

    return true;
}

int FrameShm::ChooseWritableBuffer() const {
    if (!IsValid()) {
        return -1;
    }

    const uint32_t published = AtomicLoadU32(&header_->published_index);
    const uint32_t writable = (published == 0) ? 1u : 0u;

    if (AtomicLoadU32(&meta_[writable]->read_in_progress) != 0) {
        return -1;
    }

    return static_cast<int>(writable);
}

bool FrameShm::WriteLatest(const void* src,
                                  uint32_t src_size,
                                  uint64_t frame_id,
                                  uint64_t timestamp_ns,
                                  uint32_t width,
                                  uint32_t height,
                                  uint32_t stride,
                                  PixelFormat pixel_format,
                                  int32_t dest_pos_x,
                                  int32_t dest_pos_y,
                                  const Rect& valid_rect) {
    if (!IsValid()) {
        return false;
    }

    uint32_t bytes_per_pixel = 0;
    switch (pixel_format) {
    case PixelFormat::kUYVY:
    case PixelFormat::kYUYV:
        bytes_per_pixel = 2;
        break;
    case PixelFormat::kBGRA8888:
        bytes_per_pixel = 4;
        break;
    default:
        return false;
    }

    if (src_size > header_->payload_capacity ||
        width == 0 || height == 0 || stride == 0 ||
        width > header_->width || height > header_->height ||
        stride > header_->stride) {
        return false;
    }

    const uint64_t minimum_stride = static_cast<uint64_t>(width) * bytes_per_pixel;
    const uint64_t minimum_size = static_cast<uint64_t>(height) * stride;
    if (minimum_stride > stride || minimum_size > src_size) {
        return false;
    }

    if (src_size > 0 && src == nullptr) {
        return false;
    }

    const int writable = ChooseWritableBuffer();
    if (writable < 0) {
        return false;
    }

    const uint32_t index = static_cast<uint32_t>(writable);
    uint8_t* dst = Payload(index);
    if (dst == nullptr) {
        return false;
    }

    if (src != nullptr && src_size > 0) {
        std::memcpy(dst, src, src_size);
    }

    ShmBufferMeta* meta = meta_[index];
    meta->frame_id = frame_id;
    meta->timestamp_ns = timestamp_ns;
    meta->width = width;
    meta->height = height;
    meta->stride = stride;
    meta->pixel_format = static_cast<uint32_t>(pixel_format);
    meta->payload_size = src_size;
    meta->valid_rect_pos_x = valid_rect.pos_x;
    meta->valid_rect_pos_y = valid_rect.pos_y;
    meta->valid_rect_width = valid_rect.width;
    meta->valid_rect_height = valid_rect.height;
    meta->dest_pos_x = dest_pos_x;
    meta->dest_pos_y = dest_pos_y;
    meta->valid = 1;

    AtomicFetchAddU32(&meta->write_seq, 1);
    AtomicThreadFenceRelease();
    AtomicStoreU32(&header_->published_index, index);

    return true;
}

bool FrameShm::BeginReadLatest(ReadView* out_view) {
    if (!IsValid() || out_view == nullptr) {
        return false;
    }

    for (int retry = 0; retry < 4; ++retry) {
        const uint32_t index = AtomicLoadU32(&header_->published_index);
        if (index >= kShmBufferCount) {
            return false;
        }

        ShmBufferMeta* meta = meta_[index];
        if (AtomicLoadU32(&meta->valid) == 0) {
            return false;
        }

        AtomicFetchAddU32(&meta->read_in_progress, 1);
        AtomicThreadFenceAcquire();

        const uint32_t index_after_ref = AtomicLoadU32(&header_->published_index);
        if (index_after_ref != index || AtomicLoadU32(&meta->valid) == 0) {
            AtomicFetchSubU32(&meta->read_in_progress, 1);
            continue;
        }

        AtomicStoreU32(&header_->consumer_index, index);

        out_view->index = index;
        out_view->payload = Payload(index);
        out_view->width = AtomicLoadU32(&meta->width);
        out_view->height = AtomicLoadU32(&meta->height);
        out_view->stride = AtomicLoadU32(&meta->stride);
        out_view->pixel_format = static_cast<PixelFormat>(
            AtomicLoadU32(&meta->pixel_format));
        out_view->payload_size = AtomicLoadU32(&meta->payload_size);
        out_view->frame_id = AtomicLoadU64(&meta->frame_id);
        out_view->timestamp_ns = AtomicLoadU64(&meta->timestamp_ns);
        out_view->dest_pos_x = meta->dest_pos_x;
        out_view->dest_pos_y = meta->dest_pos_y;
        out_view->valid_rect.pos_x = meta->valid_rect_pos_x;
        out_view->valid_rect.pos_y = meta->valid_rect_pos_y;
        out_view->valid_rect.width = AtomicLoadU32(&meta->valid_rect_width);
        out_view->valid_rect.height = AtomicLoadU32(&meta->valid_rect_height);
        out_view->meta = meta;
        out_view->header = header_;
        return true;
    }

    return false;
}

void FrameShm::EndRead(uint32_t index) {
    if (!IsValid() || index >= kShmBufferCount) {
        return;
    }

    ShmBufferMeta* meta = meta_[index];
    if (meta == nullptr) {
        return;
    }

    const uint32_t old = AtomicLoadU32(&meta->read_in_progress);
    if (old == 0) {
        return;
    }

    AtomicFetchSubU32(&meta->read_in_progress, 1);
}

void FrameShm::ClearAllBuffers() {
    if (!IsValid()) {
        return;
    }

    for (uint32_t index = 0; index < kShmBufferCount; ++index) {
        meta_[index]->valid = 0;
        meta_[index]->width = 0;
        meta_[index]->height = 0;
        meta_[index]->stride = 0;
        meta_[index]->pixel_format = static_cast<uint32_t>(PixelFormat::kUnknown);
        meta_[index]->payload_size = 0;
        meta_[index]->valid_rect_pos_x = 0;
        meta_[index]->valid_rect_pos_y = 0;
        meta_[index]->valid_rect_width = 0;
        meta_[index]->valid_rect_height = 0;
        meta_[index]->dest_pos_x = 0;
        meta_[index]->dest_pos_y = 0;
        meta_[index]->frame_id = 0;
        meta_[index]->timestamp_ns = 0;
        meta_[index]->write_seq = 0;
    }

    AtomicStoreU32(&header_->published_index, 0);
    AtomicStoreU32(&header_->consumer_index, 0);
}

}  // namespace kavio
