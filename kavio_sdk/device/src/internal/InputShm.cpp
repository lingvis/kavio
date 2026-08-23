#include "internal/InputShm.h"

#include <cstring>
#include <ctime>

namespace kavio {

InputShm::InputShm()
    : ShmBase(ShmKind::Input) {
}

InputShm::~InputShm() {
    Close();
}

std::unique_ptr<InputShm> InputShm::Create(
    const CreateConfig& config,
    std::string* error) {
    auto shm = std::make_unique<InputShm>();
    if (!shm->CreateInternal(config, error)) {
        return nullptr;
    }
    return shm;
}

std::unique_ptr<InputShm> InputShm::Open(
    const OpenConfig& config,
    std::string* error) {
    auto shm = std::make_unique<InputShm>();
    if (!shm->OpenInternal(config, error)) {
        return nullptr;
    }
    return shm;
}

bool InputShm::CreateInternal(const CreateConfig& config, std::string* error) {
    if (!ValidateShmName(config.name, error)) {
        return false;
    }

    if (!CreateMapping(config.name,
                       sizeof(SharedState),
                       CreatePolicy::ResizeExisting,
                       false,
                       error)) {
        return false;
    }

    state_ = static_cast<SharedState*>(Address());

    // 创建阶段允许整块初始化，因为此时结构刚创建或明确重置。
    std::memset(state_, 0, sizeof(SharedState));

    state_->magic = kMagic;
    state_->struct_size = static_cast<uint32_t>(sizeof(SharedState));
    state_->reserved_layout = 0;
    state_->timestamp_us = NowMonotonicUs();

    __atomic_add_fetch(&state_->update_counter, 1u, __ATOMIC_RELAXED);
    SetLayoutValid(true);
    return true;
}

bool InputShm::OpenInternal(const OpenConfig& config, std::string* error) {
    if (!ValidateShmName(config.name, error)) {
        return false;
    }

    if (!OpenMapping(config.name, sizeof(SharedState), config.access, error)) {
        return false;
    }

    state_ = static_cast<SharedState*>(Address());
    if (!ValidateHeader(error)) {
        Close();
        return false;
    }

    SetLayoutValid(true);
    return true;
}

void InputShm::ResetLayout() noexcept {
    state_ = nullptr;
}

bool InputShm::ResetState(std::string* error) {
    if (!RequireWritable(error)) {
        return false;
    }

    for (uint32_t i = 0; i < kKeyWords; ++i) {
        __atomic_store_n(&state_->key_bitmap[i], 0u, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&state_->mouse.x, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state_->mouse.y, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state_->mouse.buttons, 0u, __ATOMIC_RELAXED);

    __atomic_store_n(&state_->timestamp_us, NowMonotonicUs(), __ATOMIC_RELAXED);
    __atomic_add_fetch(&state_->update_counter, 1u, __ATOMIC_RELAXED);

    return true;
}

bool InputShm::SetKey(uint32_t code, bool pressed) {
    if (!RequireWritable(nullptr)) {
        return false;
    }

    if (!IsValidKeyCode(code)) {
        return false;
    }

    const uint32_t word_index = code / 32u;
    const uint32_t bit_index = code % 32u;
    const uint32_t mask = 1u << bit_index;

    if (pressed) {
        __atomic_fetch_or(&state_->key_bitmap[word_index], mask, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_and(&state_->key_bitmap[word_index], ~mask, __ATOMIC_RELAXED);
    }

    __atomic_store_n(&state_->timestamp_us, NowMonotonicUs(), __ATOMIC_RELAXED);
    __atomic_add_fetch(&state_->update_counter, 1u, __ATOMIC_RELAXED);

    return true;
}

bool InputShm::IsKeyDown(uint32_t code) const {
    if (!state_ || !IsValidKeyCode(code)) {
        return false;
    }

    const uint32_t word_index = code / 32u;
    const uint32_t bit_index = code % 32u;
    const uint32_t mask = 1u << bit_index;

    const uint32_t word = __atomic_load_n(&state_->key_bitmap[word_index], __ATOMIC_RELAXED);
    return (word & mask) != 0;
}

uint32_t InputShm::GetKeyWord(uint32_t word_index) const {
    if (!state_ || word_index >= kKeyWords) {
        return 0;
    }
    return __atomic_load_n(&state_->key_bitmap[word_index], __ATOMIC_RELAXED);
}

bool InputShm::SetMouseState(int32_t x, int32_t y, uint32_t buttons) {
    if (!RequireWritable(nullptr)) {
        return false;
    }

    __atomic_store_n(&state_->mouse.x, x, __ATOMIC_RELAXED);
    __atomic_store_n(&state_->mouse.y, y, __ATOMIC_RELAXED);
    __atomic_store_n(&state_->mouse.buttons, buttons, __ATOMIC_RELAXED);

    __atomic_store_n(&state_->timestamp_us, NowMonotonicUs(), __ATOMIC_RELAXED);
    __atomic_add_fetch(&state_->update_counter, 1u, __ATOMIC_RELAXED);
    return true;
}

InputShm::MouseState InputShm::GetMouseState() const {
    MouseState out;
    if (!state_) {
        return out;
    }

    out.x = __atomic_load_n(&state_->mouse.x, __ATOMIC_RELAXED);
    out.y = __atomic_load_n(&state_->mouse.y, __ATOMIC_RELAXED);
    out.buttons = __atomic_load_n(&state_->mouse.buttons, __ATOMIC_RELAXED);
    return out;
}

uint32_t InputShm::GetUpdateCounter() const {
    if (!state_) {
        return 0;
    }
    return __atomic_load_n(&state_->update_counter, __ATOMIC_RELAXED);
}

uint64_t InputShm::GetTimestampUs() const {
    if (!state_) {
        return 0;
    }
    return __atomic_load_n(&state_->timestamp_us, __ATOMIC_RELAXED);
}

bool InputShm::Remove(const std::string& shm_name, std::string* error) {
    if (!ValidateShmName(shm_name, error)) {
        return false;
    }
    return UnlinkByName(shm_name, error);
}

bool InputShm::ValidateShmName(const std::string& shm_name, std::string* error) {
    if (shm_name.empty()) {
        SetError(error, "input shm name is empty");
        return false;
    }
    if (shm_name[0] != '/') {
        SetError(error, "input shm name must start with '/': " + shm_name);
        return false;
    }
    return true;
}

uint64_t InputShm::NowMonotonicUs() {
    timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return static_cast<uint64_t>(ts.tv_sec) * 1000000ull
        + static_cast<uint64_t>(ts.tv_nsec) / 1000ull;
}

bool InputShm::ValidateHeader(std::string* error) const {
    if (!state_) {
        SetError(error, "input shm is not mapped");
        return false;
    }

    const uint32_t magic = __atomic_load_n(&state_->magic, __ATOMIC_RELAXED);
    const uint32_t struct_size = __atomic_load_n(&state_->struct_size, __ATOMIC_RELAXED);

    if (magic != kMagic) {
        SetError(error,
            "input shm magic mismatch, expected=" + std::to_string(kMagic)
            + ", actual=" + std::to_string(magic));
        return false;
    }

    if (struct_size != sizeof(SharedState)) {
        SetError(error,
            "input shm struct_size mismatch, expected="
            + std::to_string(sizeof(SharedState))
            + ", actual=" + std::to_string(struct_size));
        return false;
    }

    return true;
}

bool InputShm::RequireOpen(std::string* error) const {
    if (!IsValid() || !state_) {
        SetError(error, "input shm is not open");
        return false;
    }
    return true;
}

bool InputShm::RequireWritable(std::string* error) const {
    if (!RequireOpen(error)) {
        return false;
    }
    if (!IsWritable()) {
        SetError(error, "input shm is not writable");
        return false;
    }
    return true;
}

} // namespace kavio
