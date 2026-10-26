/**
 * @file Types.h
 * @brief Kavio Device SDK 通用数据类型、图像格式与 Session 配置。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <kavio/Status.h>

// Maximum serialized size of one IPC package, in bytes.
#define KAVIO_IPC_MAX_PACKAGE_BYTES (16u * 1024u * 1024u)

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

}  // namespace kavio
