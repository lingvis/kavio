#pragma once

#include <cstdint>

namespace kavio {

// SHM 内部帧缓冲角色：不是 IPC 协议对象，用于本地布局初始化。
enum class ShmType {
    kUnknown = 0,
    kRx,
    kTx,
};

}  // namespace kavio
