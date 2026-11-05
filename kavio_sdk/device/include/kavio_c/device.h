#ifndef KAVIO_C_DEVICE_H_
#define KAVIO_C_DEVICE_H_

/** C ABI for device I/O, image frames, and device state. */

#include <kavio_c/common.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct KavioDevice KavioDevice;
typedef struct KavioImageFrame KavioImageFrame;
typedef struct KavioTxSurface KavioTxSurface;
typedef struct KavioH264Packet KavioH264Packet;

typedef enum KavioRuntimeVideoStatus {
    KAVIO_RUNTIME_VIDEO_STATUS_UNKNOWN = 0,
    KAVIO_RUNTIME_VIDEO_STATUS_NOT_CONNECTED = 1,
    KAVIO_RUNTIME_VIDEO_STATUS_CONNECTED = 2,
    KAVIO_RUNTIME_VIDEO_STATUS_RUNNING = 3,
    KAVIO_RUNTIME_VIDEO_STATUS_STOPPED = 4,
    KAVIO_RUNTIME_VIDEO_STATUS_ERROR = 5
} KavioRuntimeVideoStatus;

typedef enum KavioInputMode {
    KAVIO_INPUT_MODE_BLE = 0,
    KAVIO_INPUT_MODE_RF24 = 1,
    KAVIO_INPUT_MODE_USB = 2
} KavioInputMode;

typedef enum KavioInputChannel {
    KAVIO_INPUT_CHANNEL_1 = 0,
    KAVIO_INPUT_CHANNEL_2 = 1,
    KAVIO_INPUT_CHANNEL_3 = 2
} KavioInputChannel;

typedef enum KavioInputConnectionStatus {
    KAVIO_INPUT_CONNECTION_STATUS_SYNCING = 0,
    KAVIO_INPUT_CONNECTION_STATUS_PAIRING = 1,
    KAVIO_INPUT_CONNECTION_STATUS_CONNECTED = 2,
    KAVIO_INPUT_CONNECTION_STATUS_DISCONNECTED = 3
} KavioInputConnectionStatus;

typedef enum KavioH264PacketFlag {
    KAVIO_H264_PACKET_KEY_FRAME = 1u << 0,
    KAVIO_H264_PACKET_IDR = 1u << 1,
    KAVIO_H264_PACKET_SPS = 1u << 2,
    KAVIO_H264_PACKET_PPS = 1u << 3,
    KAVIO_H264_PACKET_HEADER = 1u << 4
} KavioH264PacketFlag;

typedef struct KavioRuntimeVideoState {
    int32_t status;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t pixel_format;
} KavioRuntimeVideoState;

typedef struct KavioRuntimeInputState {
    int32_t mode;
    int32_t channel;
    int32_t connection_status;
} KavioRuntimeInputState;

typedef struct KavioRuntimeData {
    uint32_t struct_size;
    uint32_t abi_version;
    KavioRuntimeVideoState rx;
    KavioRuntimeVideoState tx;
    KavioRuntimeInputState input;
} KavioRuntimeData;

typedef struct KavioRxInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint8_t connected;
    uint8_t frame_available;
    uint8_t reserved[2];
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
} KavioRxInfo;

typedef struct KavioImageFrameInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t frame_id;
    uint64_t timestamp_ns;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
    int32_t dest_pos_x;
    int32_t dest_pos_y;
    KavioRect valid_rect;
    uint64_t data_size;
} KavioImageFrameInfo;

typedef struct KavioTxSurfaceInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t surface_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t pixel_format;
} KavioTxSurfaceInfo;

typedef struct KavioH264StreamInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t stream_id;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t bitrate_kbps;
    uint32_t gop;
    uint32_t packet_capacity;
    uint32_t payload_capacity;
} KavioH264StreamInfo;

typedef struct KavioH264PacketInfo {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t seq;
    uint64_t pts_us;
    uint64_t dts_us;
    uint64_t timestamp_ns;
    uint32_t flags;
    uint32_t nal_type;
    uint64_t data_size;
} KavioH264PacketInfo;

typedef struct KavioDeviceErrorEvent {
    uint32_t struct_size;
    uint32_t abi_version;
    uint64_t track_id;
    int32_t code;
    const char* message;
} KavioDeviceErrorEvent;

typedef struct KavioH264InvalidatedEvent {
    uint32_t struct_size;
    uint32_t abi_version;
    uint32_t stream_id;
    int32_t reason;
} KavioH264InvalidatedEvent;

enum {
    KAVIO_KEY_OPTION_DOWN = 1,
    KAVIO_KEY_OPTION_UP = 2
};

enum {
    KAVIO_MOUSE_OPTION_MOVE = 1,
    KAVIO_MOUSE_OPTION_LBUTTON_DOWN = 2,
    KAVIO_MOUSE_OPTION_LBUTTON_UP = 3,
    KAVIO_MOUSE_OPTION_RBUTTON_DOWN = 4,
    KAVIO_MOUSE_OPTION_RBUTTON_UP = 5,
    KAVIO_MOUSE_OPTION_MBUTTON_DOWN = 6,
    KAVIO_MOUSE_OPTION_MBUTTON_UP = 7,
    KAVIO_MOUSE_OPTION_WHEEL = 8
};

typedef void (*KavioCommDataCallback)(
    KavioDevice* device, const uint8_t* data, uint64_t size, void* user_data);
typedef void (*KavioTxSurfaceCallback)(
    KavioDevice* device, uint64_t request_id, KavioStatusCode status,
    KavioTxSurface* surface, void* user_data);
typedef void (*KavioH264StreamCallback)(
    KavioDevice* device, uint64_t request_id, KavioStatusCode status,
    const KavioH264StreamInfo* info, void* user_data);
typedef void (*KavioH264InvalidatedCallback)(
    KavioDevice* device, const KavioH264InvalidatedEvent* event, void* user_data);
typedef void (*KavioDeviceErrorCallback)(
    KavioDevice* device, const KavioDeviceErrorEvent* event, void* user_data);
typedef void (*KavioConfigChangedCallback)(KavioDevice* device, void* user_data);

KAVIO_APP_API void kavio_runtime_data_init(KavioRuntimeData* data);
KAVIO_APP_API void kavio_rx_info_init(KavioRxInfo* info);
KAVIO_APP_API void kavio_image_frame_info_init(KavioImageFrameInfo* info);
KAVIO_APP_API void kavio_tx_surface_info_init(KavioTxSurfaceInfo* info);
KAVIO_APP_API void kavio_h264_stream_info_init(KavioH264StreamInfo* info);
KAVIO_APP_API void kavio_h264_packet_info_init(KavioH264PacketInfo* info);

KAVIO_APP_API uint8_t kavio_device_is_ready(KavioDevice* device);
KAVIO_APP_API uint8_t kavio_device_is_usb_host_online(KavioDevice* device);
KAVIO_APP_API uint8_t kavio_device_is_comm_channel_ready(KavioDevice* device);
KAVIO_APP_API uint32_t kavio_device_get_comm_channel_id(KavioDevice* device);
KAVIO_APP_API KavioStatusCode kavio_device_get_runtime_data(
    KavioDevice* device, KavioRuntimeData* out_data);
KAVIO_APP_API KavioStatusCode kavio_device_get_rx_info(
    KavioDevice* device, KavioRxInfo* out_info);
KAVIO_APP_API KavioStatusCode kavio_device_set_rx_params(
    KavioDevice* device, uint8_t enabled, uint32_t frame_sample_interval,
    uint64_t* out_request_id);
KAVIO_APP_API KavioStatusCode kavio_device_set_tx_params(
    KavioDevice* device, uint8_t enabled, int32_t z_order,
    uint64_t* out_request_id);

KAVIO_APP_API KavioStatusCode kavio_device_read_latest_rx_frame(
    KavioDevice* device, KavioImageFrame** out_frame);
KAVIO_APP_API KavioStatusCode kavio_image_frame_get_info(
    KavioImageFrame* frame, KavioImageFrameInfo* out_info);
KAVIO_APP_API KavioStatusCode kavio_image_frame_get_data(
    KavioImageFrame* frame, const uint8_t** out_data, uint64_t* out_size);
KAVIO_APP_API void kavio_image_frame_destroy(KavioImageFrame* frame);

KAVIO_APP_API KavioStatusCode kavio_device_create_tx_surface(
    KavioDevice* device, uint32_t width, uint32_t height, uint64_t* out_request_id);
KAVIO_APP_API KavioStatusCode kavio_device_destroy_tx_surface(
    KavioDevice* device, uint32_t surface_id, uint64_t* out_request_id);
KAVIO_APP_API KavioStatusCode kavio_tx_surface_get_info(
    KavioTxSurface* surface, KavioTxSurfaceInfo* out_info);
KAVIO_APP_API KavioStatusCode kavio_tx_surface_write(
    KavioTxSurface* surface,
    const void* data, uint64_t data_size,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t pixel_format,
    int32_t dest_pos_x, int32_t dest_pos_y,
    const KavioRect* valid_rect);
KAVIO_APP_API KavioStatusCode kavio_tx_surface_clear(KavioTxSurface* surface);
KAVIO_APP_API KavioStatusCode kavio_tx_surface_request_destroy(
    KavioTxSurface* surface, uint64_t* out_request_id);
KAVIO_APP_API void kavio_tx_surface_destroy(KavioTxSurface* surface);

/* key_code is a USB HID Keyboard/Keypad usage ID (Usage Page 0x07). */
KAVIO_APP_API KavioStatusCode kavio_device_send_keyboard(
    KavioDevice* device, int32_t key_option, int32_t key_code, uint64_t timestamp_ns);
KAVIO_APP_API KavioStatusCode kavio_device_send_mouse(
    KavioDevice* device, int32_t mouse_option, int32_t x, int32_t y,
    int32_t wheel, uint64_t timestamp_ns);
KAVIO_APP_API KavioStatusCode kavio_device_send_comm_data(
    KavioDevice* device, const void* data, uint64_t size);

KAVIO_APP_API KavioStatusCode kavio_device_get_app_config_json(
    KavioDevice* device, char** out_json, uint64_t* out_size);
KAVIO_APP_API KavioStatusCode kavio_device_update_app_config_json(
    KavioDevice* device, const char* json, uint64_t size);
KAVIO_APP_API KavioStatusCode kavio_device_reset_app_config(KavioDevice* device);

KAVIO_APP_API KavioStatusCode kavio_device_subscribe_h264(
    KavioDevice* device, uint64_t* out_request_id);
KAVIO_APP_API KavioStatusCode kavio_device_unsubscribe_h264(
    KavioDevice* device, uint32_t stream_id, uint64_t* out_request_id);
KAVIO_APP_API uint64_t kavio_device_get_latest_h264_packet_seq(
    KavioDevice* device, uint32_t stream_id);
KAVIO_APP_API KavioStatusCode kavio_device_read_h264_packet(
    KavioDevice* device, uint32_t stream_id, uint64_t seq, KavioH264Packet** out_packet);
KAVIO_APP_API KavioStatusCode kavio_h264_packet_get_info(
    KavioH264Packet* packet, KavioH264PacketInfo* out_info);
KAVIO_APP_API KavioStatusCode kavio_h264_packet_get_data(
    KavioH264Packet* packet, const uint8_t** out_data, uint64_t* out_size);
KAVIO_APP_API void kavio_h264_packet_destroy(KavioH264Packet* packet);

KAVIO_APP_API KavioStatusCode kavio_device_set_comm_data_callback(
    KavioDevice* device, KavioCommDataCallback callback, void* user_data);
KAVIO_APP_API KavioStatusCode kavio_device_set_tx_surface_callback(
    KavioDevice* device, KavioTxSurfaceCallback callback, void* user_data);
KAVIO_APP_API KavioStatusCode kavio_device_set_h264_stream_callback(
    KavioDevice* device, KavioH264StreamCallback callback, void* user_data);
KAVIO_APP_API KavioStatusCode kavio_device_set_h264_invalidated_callback(
    KavioDevice* device, KavioH264InvalidatedCallback callback, void* user_data);
KAVIO_APP_API KavioStatusCode kavio_device_set_error_callback(
    KavioDevice* device, KavioDeviceErrorCallback callback, void* user_data);
KAVIO_APP_API KavioStatusCode kavio_device_set_config_changed_callback(
    KavioDevice* device, KavioConfigChangedCallback callback, void* user_data);

#ifdef __cplusplus
}
#endif

#endif  // KAVIO_C_DEVICE_H_
