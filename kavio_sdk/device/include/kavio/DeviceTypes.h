#pragma once

/** Portable value types returned by device services. */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <kavio/AppApi.h>
#include <kavio/Status.h>
#include <kavio/Types.h>

namespace kavio {

class Device;
namespace detail { class DeviceAccess; }

/** Stable runtime video states used by the high-level Device API. */
enum class RuntimeVideoStatus : int32_t {
    kUnknown = 0,
    kNotConnected = 1,
    kConnected = 2,
    kRunning = 3,
    kStopped = 4,
    kError = 5,
};

enum class InputMode : int32_t {
    kBle = 0,
    kRf24 = 1,
    kUsb = 2,
};

enum class InputChannel : int32_t {
    kChannel1 = 0,
    kChannel2 = 1,
    kChannel3 = 2,
};

enum class InputConnectionStatus : int32_t {
    kSyncing = 0,
    kPairing = 1,
    kConnected = 2,
    kDisconnected = 3,
};

enum class KeyOption : int32_t {
    kDown = 1,
    kUp = 2,
};

enum class MouseOption : int32_t {
    kMove = 1,
    kLeftDown = 2,
    kLeftUp = 3,
    kRightDown = 4,
    kRightUp = 5,
    kMiddleDown = 6,
    kMiddleUp = 7,
    kWheel = 8,
};

struct DeviceRuntimeVideoState {
    RuntimeVideoStatus status = RuntimeVideoStatus::kUnknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;
};

struct DeviceRuntimeInputState {
    InputMode mode = InputMode::kBle;
    InputChannel channel = InputChannel::kChannel1;
    InputConnectionStatus connection_status = InputConnectionStatus::kSyncing;
};

struct DeviceRuntimeData {
    DeviceRuntimeVideoState rx;
    DeviceRuntimeVideoState tx;
    DeviceRuntimeInputState input;
};

/** RX state snapshot owned by the high-level Device API. */
struct DeviceRxInfo {
    bool connected = false;
    bool frame_available = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;
};

/** H.264 packet flags. */
enum DeviceH264PacketFlag : uint32_t {
    kDeviceH264PacketKeyFrame = 1u << 0,
    kDeviceH264PacketIDR = 1u << 1,
    kDeviceH264PacketSPS = 1u << 2,
    kDeviceH264PacketPPS = 1u << 3,
    kDeviceH264PacketHeader = 1u << 4,
};

struct DeviceH264Packet {
    uint64_t seq = 0;
    uint64_t pts_us = 0;
    uint64_t dts_us = 0;
    uint64_t timestamp_ns = 0;
    uint32_t flags = 0;
    uint32_t nal_type = 0;
    std::vector<uint8_t> data;
};

struct DeviceH264StreamInfo {
    uint32_t stream_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t gop = 0;
    uint32_t packet_capacity = 0;
    uint32_t payload_capacity = 0;
};

struct DeviceErrorEvent {
    uint64_t track_id = 0;
    int32_t code = 0;
    std::string message;
};

struct DeviceH264InvalidatedEvent {
    uint32_t stream_id = 0;
    int32_t reason = 0;
};

/**
 * High-level TX Surface owned by AppRuntime.
 *
 * The implementation is kept inside libkavio_app_runtime.so so ordinary C++
 * Apps do not need to link the low-level KavioClient static library.
 */
class KAVIO_APP_API DeviceTxSurface {
public:
    DeviceTxSurface();
    ~DeviceTxSurface();

    DeviceTxSurface(DeviceTxSurface&& other) noexcept;
    DeviceTxSurface& operator=(DeviceTxSurface&& other) noexcept;

    DeviceTxSurface(const DeviceTxSurface&) = delete;
    DeviceTxSurface& operator=(const DeviceTxSurface&) = delete;

    bool IsValid() const;
    uint32_t id() const;
    uint32_t width() const;
    uint32_t height() const;
    uint32_t stride() const;
    PixelFormat pixel_format() const;

    Status WriteFrame(const ImageFrame& frame);
    Status Clear();
    Status Destroy(uint64_t& request_id);

private:
    friend class Device;
    friend class detail::DeviceAccess;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kavio
