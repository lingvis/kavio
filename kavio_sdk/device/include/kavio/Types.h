#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <kavio/Status.h>

namespace kavio {

inline constexpr char kDefaultIpcSocketPath[] = "/tmp/kavio.sock";

inline constexpr const char* DefaultIpcSocketPath() {
    return kDefaultIpcSocketPath;
}

enum class PixelFormat {
    kUnknown = 0,
    kUYVY,
    kYUYV,
    kBGRA8888,
};

struct Rect {
    int32_t pos_x = 0;
    int32_t pos_y = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    bool empty() const {
        return width == 0 || height == 0;
    }
};

struct ImageFrame {
    uint64_t frame_id = 0;
    uint64_t timestamp_ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;
    int32_t dest_pos_x = 0;
    int32_t dest_pos_y = 0;
    Rect valid_rect;
    std::vector<uint8_t> data;

    bool empty() const { return data.empty(); }
};

struct RxParams {
    bool enabled = true;
    uint32_t frame_sample_interval = 1;
};

struct TxParams {
    bool enabled = false;
    int32_t z_order = 0;
};

struct SessionOptions {
    std::string client_name = "kavio_sdk_client";
    int32_t client_pid = 0;
    RxParams rx_params;
    TxParams tx_params;
    bool auto_query_runtime_on_connect = true;
    bool auto_query_user_config_on_connect = true;
};

}  // namespace kavio
