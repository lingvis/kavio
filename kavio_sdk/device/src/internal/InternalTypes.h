#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <kavio/Types.h>

namespace kavio {

enum class ShmType {
    kUnknown = 0,
    kRx,
    kTx,
};

struct ShmRegionInfo {
    ShmType type = ShmType::kUnknown;
    uint32_t id = 0;
    std::string name;
    int fd = -1;
    void* addr = nullptr;
    size_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;

    bool valid() const {
        return fd >= 0 && addr != nullptr && size > 0;
    }

    void Reset() {
        type = ShmType::kUnknown;
        id = 0;
        name.clear();
        fd = -1;
        addr = nullptr;
        size = 0;
        width = 0;
        height = 0;
        stride = 0;
        pixel_format = PixelFormat::kUnknown;
    }
};

struct ShmInfo {
    uint32_t id = 0;
    std::string name;
    uint64_t size = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;
};

struct SessionInfo {
    uint32_t session_id = 0;
    uint32_t max_sessions = 0;
    uint32_t max_rx_shms_per_session = 1;
    uint32_t max_tx_shms_per_session = 10;
    ShmInfo rx_shm;
    std::vector<ShmInfo> tx_shms;
};

}  // namespace kavio
