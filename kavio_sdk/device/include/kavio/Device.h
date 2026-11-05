#pragma once

/** Device services and image-frame access for C++ applications. */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <kavio/AppApi.h>
#include <kavio/Status.h>
#include <kavio/Types.h>
#include <kavio/DeviceTypes.h>
#include <kavio/Keyboard.h>

namespace kavio {
namespace detail { class DeviceAccess; }

/**
 * @brief Device capabilities owned by a running KavioApp.
 *
 * The high-level interface uses only Kavio SDK types. Protobuf, JsonCpp and the
 * low-level KavioClient remain implementation details. Asynchronous handlers
 * execute on the App run thread.
 */
class KAVIO_APP_API Device {
public:
    using RuntimeData = DeviceRuntimeData;
    using RxInfo = DeviceRxInfo;
    using TxSurface = DeviceTxSurface;
    using H264Packet = DeviceH264Packet;
    using H264StreamInfo = DeviceH264StreamInfo;
    using H264InvalidatedEvent = DeviceH264InvalidatedEvent;

    using CommDataHandler = std::function<void(const std::vector<uint8_t>&)>;
    using TxSurfaceHandler = std::function<void(uint64_t, Status, TxSurface)>;
    using H264StreamHandler = std::function<void(uint64_t, Status, H264StreamInfo)>;
    using H264InvalidatedHandler = std::function<void(const H264InvalidatedEvent&)>;
    using ErrorHandler = std::function<void(const DeviceErrorEvent&)>;
    using ConfigChangedHandler = std::function<void()>;

    Device();
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool IsReady() const;
    RuntimeData GetRuntimeData() const;
    RxInfo GetRxInfo() const;
    bool IsUSBHostOnline() const;
    bool IsCommChannelReady() const;
    uint32_t GetCommChannelId() const;

    Status SetRxParams(bool enabled, uint32_t frame_sample_interval, uint64_t& request_id);
    Status SetTxParams(bool enabled, int32_t z_order, uint64_t& request_id);
    Status ReadLatestRxFrame(ImageFrame& out_frame);

    Status CreateTxSurface(uint32_t width, uint32_t height, uint64_t& request_id);
    Status DestroyTxSurface(uint32_t surface_id, uint64_t& request_id);

    /** Send a keyboard event using a USB HID Keyboard/Keypad usage code. */
    Status SendKeyboard(KeyOption option, KeyCode key_code, uint64_t timestamp_ns = 0);
    Status SendMouse(MouseOption option,
                     int32_t x = 0,
                     int32_t y = 0,
                     int32_t wheel = 0,
                     uint64_t timestamp_ns = 0);
    Status SendCommChannelData(const void* data, size_t size);
    Status SendCommChannelData(const std::vector<uint8_t>& data);

    /** Complete App Configuration encoded as compact UTF-8 JSON. */
    std::string GetAppConfigJson() const;
    Status UpdateAppConfigJson(const std::string& json);
    Status ResetAppConfigToDefaults();

    Status SubscribeH264Stream(uint64_t& request_id);
    Status UnsubscribeH264Stream(uint32_t stream_id, uint64_t& request_id);
    uint64_t GetLatestH264PacketSeq(uint32_t stream_id) const;
    Status ReadH264Packet(uint32_t stream_id, uint64_t seq, H264Packet& packet) const;

    void SetCommDataHandler(CommDataHandler handler);
    void SetTxSurfaceHandler(TxSurfaceHandler handler);
    void SetH264StreamHandler(H264StreamHandler handler);
    void SetH264InvalidatedHandler(H264InvalidatedHandler handler);
    void SetErrorHandler(ErrorHandler handler);
    void SetConfigChangedHandler(ConfigChangedHandler handler);

private:
    friend class detail::DeviceAccess;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace kavio
