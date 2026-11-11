#include <kavio_c/app.h>
#include <kavio_c/device.h>
#include <kavio_c/presentation.h>

#include "c_api/KavioCInternal.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>


#include <kavio/Status.h>

namespace {

static_assert(KAVIO_STATUS_OK == static_cast<int32_t>(kavio::StatusCode::OK), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_INVALID_ARGUMENT == static_cast<int32_t>(kavio::StatusCode::Common_InvalidArgument), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_ALREADY_EXISTS == static_cast<int32_t>(kavio::StatusCode::Common_AlreadyExists), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_NOT_FOUND == static_cast<int32_t>(kavio::StatusCode::Common_NotFound), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_INTERNAL_ERROR == static_cast<int32_t>(kavio::StatusCode::Common_InternalError), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_INVALID_STATE == static_cast<int32_t>(kavio::StatusCode::Common_InvalidState), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_CANCELLED == static_cast<int32_t>(kavio::StatusCode::Common_Cancelled), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_UNSUPPORTED == static_cast<int32_t>(kavio::StatusCode::Common_Unsupported), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_NOT_CONNECTED == static_cast<int32_t>(kavio::StatusCode::Common_NotConnected), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_TIMEOUT == static_cast<int32_t>(kavio::StatusCode::Common_Timeout), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_COMMON_PROTOCOL_ERROR == static_cast<int32_t>(kavio::StatusCode::Common_ProtocolError), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_SESSION_TOO_MANY_SESSIONS == static_cast<int32_t>(kavio::StatusCode::Session_TooManySessions), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_MEDIA_VIDEO_NOT_READY == static_cast<int32_t>(kavio::StatusCode::Media_VideoNotReady), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_MEDIA_NO_FRAME == static_cast<int32_t>(kavio::StatusCode::Media_NoFrame), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_SHM_CREATE_FAILED == static_cast<int32_t>(kavio::StatusCode::SHM_CreateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_SHM_OPERATION_FAILED == static_cast<int32_t>(kavio::StatusCode::SHM_OperationFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_IPC_SOCKET_ERROR == static_cast<int32_t>(kavio::StatusCode::IPC_SocketError), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_BODY_CASE_NOT_SUPPORTED == static_cast<int32_t>(kavio::StatusCode::USB_BodyCaseNotSupported), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_VERSION_MISMATCH == static_cast<int32_t>(kavio::StatusCode::USB_AppVersionMismatch), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_SIGNATURE_INVALID == static_cast<int32_t>(kavio::StatusCode::USB_AppSignatureInvalid), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_FILE_HASH_MISMATCH == static_cast<int32_t>(kavio::StatusCode::USB_AppFileHashMismatch), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_UNVERIFIED_APP_NOT_ALLOWED == static_cast<int32_t>(kavio::StatusCode::USB_AppUnverifiedAppNotAllowed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_ENVIRONMENT_INSTALL_FAILED == static_cast<int32_t>(kavio::StatusCode::USB_AppEnvironmentInstallFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_UNSUPPORTED_RUNTIME == static_cast<int32_t>(kavio::StatusCode::USB_AppUnsupportedRuntime), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_USB_APP_INSTALL_IN_PROGRESS == static_cast<int32_t>(kavio::StatusCode::USB_AppInstallInProgress), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INVALID_ARGUMENT == static_cast<int32_t>(kavio::StatusCode::H264_InvalidArgument), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_UNSUPPORTED_CODEC == static_cast<int32_t>(kavio::StatusCode::H264_UnsupportedCodec), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INVALID_CONFIG == static_cast<int32_t>(kavio::StatusCode::H264_InvalidConfig), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_NOT_INITIALIZED == static_cast<int32_t>(kavio::StatusCode::H264_NotInitialized), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_STREAM_ALREADY_EXISTS == static_cast<int32_t>(kavio::StatusCode::H264_StreamAlreadyExists), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_STREAM_NOT_FOUND == static_cast<int32_t>(kavio::StatusCode::H264_StreamNotFound), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_STREAM_SHM_CREATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_StreamShmCreateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_RUNTIME_BITRATE_UNSUPPORTED == static_cast<int32_t>(kavio::StatusCode::H264_RuntimeBitrateUnsupported), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_RUNTIME_IDR_UNSUPPORTED == static_cast<int32_t>(kavio::StatusCode::H264_RuntimeIDRUnsupported), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_VENC_CREATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_VencCreateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_VENC_OPEN_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_VencOpenFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_VENC_INPUT_CONNECT_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_VencInputConnectFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_VENC_OUTPUT_CONNECT_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_VencOutputConnectFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_VENC_NOT_OPENED == static_cast<int32_t>(kavio::StatusCode::H264_VencNotOpened), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_VENC_START_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_VencStartFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INPUT_BD_CHAIN_CREATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_InputBDChainCreateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INPUT_BD_ALLOCATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_InputBDAllocateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INPUT_SHM_ALLOCATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_InputShmAllocateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INPUT_BD_TAG_APPEND_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_InputBDTagAppendFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_INPUT_BD_PUSH_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_InputBDPushFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_BD_CHAIN_CREATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_OutputBDChainCreateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_BD_ALLOCATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_OutputBDAllocateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_SHM_ALLOCATE_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_OutputShmAllocateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_BD_TAG_APPEND_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_OutputBDTagAppendFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_BD_PUSH_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_OutputBDPushFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_PATH_NOT_READY == static_cast<int32_t>(kavio::StatusCode::H264_OutputPathNotReady), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_H264_OUTPUT_BD_CHAIN_QUERY_FAILED == static_cast<int32_t>(kavio::StatusCode::H264_OutputBDChainQueryFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_DEVICE_OPEN_FAILED == static_cast<int32_t>(kavio::StatusCode::DRM_DeviceOpenFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_DEVICE_OWNED_BY_DISPLAY_SERVER == static_cast<int32_t>(kavio::StatusCode::DRM_DeviceOwnedByDisplayServer), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_SET_MASTER_FAILED == static_cast<int32_t>(kavio::StatusCode::DRM_SetMasterFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_OUTPUT_TARGET_NOT_FOUND == static_cast<int32_t>(kavio::StatusCode::DRM_OutputTargetNotFound), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_ARGB_PLANE_NOT_FOUND == static_cast<int32_t>(kavio::StatusCode::DRM_ArgbPlaneNotFound), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_DUMB_BUFFER_CREATE_FAILED == static_cast<int32_t>(kavio::StatusCode::DRM_DumbBufferCreateFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_INITIAL_CRTC_SET_FAILED == static_cast<int32_t>(kavio::StatusCode::DRM_InitialCrtcSetFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_NOT_INITIALIZED == static_cast<int32_t>(kavio::StatusCode::DRM_NotInitialized), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_FRAME_ALREADY_IN_PROGRESS == static_cast<int32_t>(kavio::StatusCode::DRM_FrameAlreadyInProgress), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_FRAME_NOT_STARTED == static_cast<int32_t>(kavio::StatusCode::DRM_FrameNotStarted), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_TOP_LAYER_UPDATE_DURING_FRAME == static_cast<int32_t>(kavio::StatusCode::DRM_TopLayerUpdateDuringFrame), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_BACK_BUFFER_UNAVAILABLE == static_cast<int32_t>(kavio::StatusCode::DRM_BackBufferUnavailable), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_SCANOUT_BUFFER_UNAVAILABLE == static_cast<int32_t>(kavio::StatusCode::DRM_ScanoutBufferUnavailable), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_BACK_FRAMEBUFFER_UNAVAILABLE == static_cast<int32_t>(kavio::StatusCode::DRM_BackFramebufferUnavailable), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_UNSUPPORTED_PIXEL_FORMAT == static_cast<int32_t>(kavio::StatusCode::DRM_UnsupportedPixelFormat), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_INVALID_LAYER_BUFFER == static_cast<int32_t>(kavio::StatusCode::DRM_InvalidLayerBuffer), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_INVALID_LAYER_GEOMETRY == static_cast<int32_t>(kavio::StatusCode::DRM_InvalidLayerGeometry), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_LAYER_PAYLOAD_TOO_SMALL == static_cast<int32_t>(kavio::StatusCode::DRM_LayerPayloadTooSmall), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_INVALID_TOP_LAYER_BUFFER == static_cast<int32_t>(kavio::StatusCode::DRM_InvalidTopLayerBuffer), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_TOP_LAYER_SIZE_MISMATCH == static_cast<int32_t>(kavio::StatusCode::DRM_TopLayerSizeMismatch), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_INVALID_TOP_LAYER_STRIDE == static_cast<int32_t>(kavio::StatusCode::DRM_InvalidTopLayerStride), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_SET_PLANE_FAILED == static_cast<int32_t>(kavio::StatusCode::DRM_SetPlaneFailed), "C ABI status code mismatch");
static_assert(KAVIO_STATUS_DRM_UNSUPPORTED_PLANE_TYPE == static_cast<int32_t>(kavio::StatusCode::DRM_UnsupportedPlaneType), "C ABI status code mismatch");
static_assert(KAVIO_PIXEL_FORMAT_UNKNOWN == static_cast<int>(kavio::PixelFormat::kUnknown), "C ABI pixel format mismatch");
static_assert(KAVIO_PIXEL_FORMAT_UYVY == static_cast<int>(kavio::PixelFormat::kUYVY), "C ABI pixel format mismatch");
static_assert(KAVIO_PIXEL_FORMAT_YUYV == static_cast<int>(kavio::PixelFormat::kYUYV), "C ABI pixel format mismatch");
static_assert(KAVIO_PIXEL_FORMAT_BGRA8888 == static_cast<int>(kavio::PixelFormat::kBGRA8888), "C ABI pixel format mismatch");
static_assert(KAVIO_RUNTIME_VIDEO_STATUS_UNKNOWN == static_cast<int32_t>(kavio::RuntimeVideoStatus::kUnknown), "C ABI runtime status mismatch");
static_assert(KAVIO_RUNTIME_VIDEO_STATUS_NOT_CONNECTED == static_cast<int32_t>(kavio::RuntimeVideoStatus::kNotConnected), "C ABI runtime status mismatch");
static_assert(KAVIO_RUNTIME_VIDEO_STATUS_CONNECTED == static_cast<int32_t>(kavio::RuntimeVideoStatus::kConnected), "C ABI runtime status mismatch");
static_assert(KAVIO_RUNTIME_VIDEO_STATUS_RUNNING == static_cast<int32_t>(kavio::RuntimeVideoStatus::kRunning), "C ABI runtime status mismatch");
static_assert(KAVIO_RUNTIME_VIDEO_STATUS_STOPPED == static_cast<int32_t>(kavio::RuntimeVideoStatus::kStopped), "C ABI runtime status mismatch");
static_assert(KAVIO_RUNTIME_VIDEO_STATUS_ERROR == static_cast<int32_t>(kavio::RuntimeVideoStatus::kError), "C ABI runtime status mismatch");
static_assert(KAVIO_INPUT_MODE_BLE == static_cast<int32_t>(kavio::InputMode::kBle), "C ABI input mode mismatch");
static_assert(KAVIO_INPUT_MODE_RF24 == static_cast<int32_t>(kavio::InputMode::kRf24), "C ABI input mode mismatch");
static_assert(KAVIO_INPUT_MODE_USB == static_cast<int32_t>(kavio::InputMode::kUsb), "C ABI input mode mismatch");
static_assert(KAVIO_INPUT_CHANNEL_1 == static_cast<int32_t>(kavio::InputChannel::kChannel1), "C ABI input channel mismatch");
static_assert(KAVIO_INPUT_CHANNEL_2 == static_cast<int32_t>(kavio::InputChannel::kChannel2), "C ABI input channel mismatch");
static_assert(KAVIO_INPUT_CHANNEL_3 == static_cast<int32_t>(kavio::InputChannel::kChannel3), "C ABI input channel mismatch");
static_assert(KAVIO_INPUT_CONNECTION_STATUS_SYNCING == static_cast<int32_t>(kavio::InputConnectionStatus::kSyncing), "C ABI input connection mismatch");
static_assert(KAVIO_INPUT_CONNECTION_STATUS_PAIRING == static_cast<int32_t>(kavio::InputConnectionStatus::kPairing), "C ABI input connection mismatch");
static_assert(KAVIO_INPUT_CONNECTION_STATUS_CONNECTED == static_cast<int32_t>(kavio::InputConnectionStatus::kConnected), "C ABI input connection mismatch");
static_assert(KAVIO_INPUT_CONNECTION_STATUS_DISCONNECTED == static_cast<int32_t>(kavio::InputConnectionStatus::kDisconnected), "C ABI input connection mismatch");
static_assert(KAVIO_H264_PACKET_KEY_FRAME == static_cast<uint32_t>(kavio::kDeviceH264PacketKeyFrame), "C ABI H264 flag mismatch");
static_assert(KAVIO_H264_PACKET_IDR == static_cast<uint32_t>(kavio::kDeviceH264PacketIDR), "C ABI H264 flag mismatch");
static_assert(KAVIO_H264_PACKET_SPS == static_cast<uint32_t>(kavio::kDeviceH264PacketSPS), "C ABI H264 flag mismatch");
static_assert(KAVIO_H264_PACKET_PPS == static_cast<uint32_t>(kavio::kDeviceH264PacketPPS), "C ABI H264 flag mismatch");
static_assert(KAVIO_H264_PACKET_HEADER == static_cast<uint32_t>(kavio::kDeviceH264PacketHeader), "C ABI H264 flag mismatch");
static_assert(KAVIO_KEY_OPTION_DOWN == static_cast<int32_t>(kavio::KeyOption::kDown), "C ABI keyboard option mismatch");
static_assert(KAVIO_KEY_OPTION_UP == static_cast<int32_t>(kavio::KeyOption::kUp), "C ABI keyboard option mismatch");
static_assert(KAVIO_MOUSE_OPTION_MOVE == static_cast<int32_t>(kavio::MouseOption::kMove), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_LBUTTON_DOWN == static_cast<int32_t>(kavio::MouseOption::kLeftDown), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_LBUTTON_UP == static_cast<int32_t>(kavio::MouseOption::kLeftUp), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_RBUTTON_DOWN == static_cast<int32_t>(kavio::MouseOption::kRightDown), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_RBUTTON_UP == static_cast<int32_t>(kavio::MouseOption::kRightUp), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_MBUTTON_DOWN == static_cast<int32_t>(kavio::MouseOption::kMiddleDown), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_MBUTTON_UP == static_cast<int32_t>(kavio::MouseOption::kMiddleUp), "C ABI mouse option mismatch");
static_assert(KAVIO_MOUSE_OPTION_WHEEL == static_cast<int32_t>(kavio::MouseOption::kWheel), "C ABI mouse option mismatch");

thread_local std::string g_last_error;

void ClearLastError()
{
    g_last_error.clear();
}

KavioStatusCode SetError(KavioStatusCode code, const std::string& message)
{
    g_last_error = message;
    return code;
}

KavioStatusCode ReturnStatus(const kavio::Status& status)
{
    if (status.ok()) {
        ClearLastError();
        return KAVIO_STATUS_OK;
    }
    return SetError(static_cast<KavioStatusCode>(status.code), status.message);
}

KavioStatusCode InvalidArgument(const char* message)
{
    return SetError(KAVIO_STATUS_COMMON_INVALID_ARGUMENT, message ? message : "invalid argument");
}

KavioStatusCode InternalException(const char* message)
{
    return SetError(KAVIO_STATUS_COMMON_INTERNAL_ERROR,
                    message ? message : "unexpected C++ exception");
}

bool ValidStructHeader(uint32_t struct_size, uint32_t abi_version, size_t required_size)
{
    return struct_size >= required_size && abi_version == KAVIO_ABI_VERSION;
}

KavioPixelFormat ToCPixelFormat(kavio::PixelFormat format)
{
    switch (format) {
        case kavio::PixelFormat::kUYVY:
            return KAVIO_PIXEL_FORMAT_UYVY;
        case kavio::PixelFormat::kYUYV:
            return KAVIO_PIXEL_FORMAT_YUYV;
        case kavio::PixelFormat::kBGRA8888:
            return KAVIO_PIXEL_FORMAT_BGRA8888;
        case kavio::PixelFormat::kUnknown:
        default:
            return KAVIO_PIXEL_FORMAT_UNKNOWN;
    }
}

kavio::PixelFormat ToCppPixelFormat(uint32_t format)
{
    switch (format) {
        case KAVIO_PIXEL_FORMAT_UYVY: return kavio::PixelFormat::kUYVY;
        case KAVIO_PIXEL_FORMAT_YUYV: return kavio::PixelFormat::kYUYV;
        case KAVIO_PIXEL_FORMAT_BGRA8888: return kavio::PixelFormat::kBGRA8888;
        default: return kavio::PixelFormat::kUnknown;
    }
}

void FillFrameInfo(const kavio::FrameBufferInfo& source, KavioFrameInfo& target)
{
    target.width = source.width;
    target.height = source.height;
    target.stride = source.stride;
    target.pixel_format = static_cast<uint32_t>(ToCPixelFormat(source.pixel_format));
    target.buffer_size = source.buffer_size;
    target.surface_id = source.surface_id;
    target.reserved0 = 0;
    target.generation = source.generation;
}

KavioPresentation* EnsurePresentationHandle(KavioApp* app)
{
    if (!app) return nullptr;
    if (!app->presentation) {
        auto wrapper = std::make_unique<KavioPresentation>();
        wrapper->owner = app;
        wrapper->presentation = &app->app.context().presentation();
        app->presentation = std::move(wrapper);
    }
    return app->presentation.get();
}

KavioDevice* EnsureDeviceHandle(KavioApp* app)
{
    if (!app) return nullptr;
    if (!app->device) {
        auto wrapper = std::make_unique<KavioDevice>();
        wrapper->owner = app;
        wrapper->device = &app->app.context().device();
        app->device = std::move(wrapper);
    }
    return app->device.get();
}

void InstallAppBridges(KavioApp* app)
{
    app->app.on_ready([app](kavio::AppContext&) {
        KavioAppReadyCallback callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(app->callback_mutex);
            callback = app->ready_callback;
            user_data = app->ready_user_data;
        }
        if (callback) callback(app, user_data);
    });
    app->app.on_tick([app](kavio::AppContext&) {
        KavioAppTickCallback callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(app->callback_mutex);
            callback = app->tick_callback;
            user_data = app->tick_user_data;
        }
        if (callback) callback(app, user_data);
    });
    app->app.on_stopping([app](kavio::AppContext&) {
        KavioAppStoppingCallback callback = nullptr;
        void* user_data = nullptr;
        {
            std::lock_guard<std::mutex> lock(app->callback_mutex);
            callback = app->stopping_callback;
            user_data = app->stopping_user_data;
        }
        if (callback) callback(app, user_data);
    });
}

void InstallTouchBridge(KavioPresentation* presentation)
{
    presentation->presentation->SetTouchHandler(
        [presentation](const kavio::TouchFrame& source) {
            KavioTouchCallback callback = nullptr;
            void* user_data = nullptr;
            {
                std::lock_guard<std::mutex> lock(presentation->callback_mutex);
                callback = presentation->touch_callback;
                user_data = presentation->touch_user_data;
            }
            if (!callback) return;

            std::vector<KavioTouchPoint> points;
            points.reserve(source.points.size());
            for (const auto& point : source.points) {
                KavioTouchPoint converted{};
                converted.slot = point.slot;
                converted.tracking_id = point.tracking_id;
                converted.x = point.x;
                converted.y = point.y;
                converted.pressure = point.pressure;
                converted.touch_major = point.touch_major;
                converted.active = point.active ? 1u : 0u;
                converted.changed = point.changed ? 1u : 0u;
                points.push_back(converted);
            }

            KavioTouchFrame frame{};
            frame.struct_size = sizeof(frame);
            frame.abi_version = KAVIO_ABI_VERSION;
            frame.surface_id = source.surface_id;
            frame.point_count = static_cast<uint32_t>(points.size());
            frame.sequence = source.sequence;
            frame.points = points.empty() ? nullptr : points.data();
            frame.cancelled = source.cancelled ? 1u : 0u;
            callback(presentation, &frame, user_data);
        });
}

void InstallVisibilityBridge(KavioPresentation* presentation)
{
    presentation->presentation->SetVisibilityHandler(
        [presentation](bool visible) {
            KavioVisibilityCallback callback = nullptr;
            void* user_data = nullptr;
            {
                std::lock_guard<std::mutex> lock(presentation->callback_mutex);
                callback = presentation->visibility_callback;
                user_data = presentation->visibility_user_data;
            }
            if (callback) callback(presentation, visible ? 1u : 0u, user_data);
        });
}

}  // namespace

extern "C" {

uint32_t kavio_get_abi_version(void)
{
    return KAVIO_ABI_VERSION;
}

const char* kavio_status_code_name(KavioStatusCode code)
{
    return kavio::StatusCodeToString(code);
}

const char* kavio_last_error_message(void)
{
    return g_last_error.c_str();
}

void kavio_free(void* pointer)
{
    ClearLastError();
    std::free(pointer);
}

void kavio_app_options_init(KavioAppOptions* options)
{
    ClearLastError();
    if (!options) return;
    *options = KavioAppOptions KAVIO_APP_OPTIONS_INIT;
}

KavioStatusCode kavio_app_create(const KavioAppOptions* options, KavioApp** out_app)
{
    ClearLastError();
    if (!out_app) return InvalidArgument("out_app is null");
    *out_app = nullptr;
    try {
        kavio::AppOptions cpp_options;
        if (options) {
            if (!ValidStructHeader(options->struct_size, options->abi_version,
                                   KAVIO_APP_OPTIONS_V1_SIZE)) {
                return InvalidArgument("KavioAppOptions struct_size or abi_version is invalid");
            }
            if (options->client_name) cpp_options.client_name = options->client_name;
            cpp_options.idle_sleep_ms = options->idle_sleep_ms == 0 ? 1u : options->idle_sleep_ms;
            cpp_options.install_signal_handlers = options->install_signal_handlers != 0;
            if (options->struct_size >= KAVIO_APP_OPTIONS_DEVICE_V1_SIZE) {
                cpp_options.rx_enabled = options->rx_enabled != 0;
                cpp_options.tx_enabled = options->tx_enabled != 0;
                cpp_options.rx_frame_sample_interval = options->rx_frame_sample_interval == 0
                    ? 1u : options->rx_frame_sample_interval;
                cpp_options.tx_z_order = options->tx_z_order;
            }
        }

        auto app = std::make_unique<KavioApp>(std::move(cpp_options));
        (void)EnsurePresentationHandle(app.get());
        (void)EnsureDeviceHandle(app.get());
        InstallAppBridges(app.get());
        *out_app = app.release();
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_app_create failed with an unknown exception");
    }
}

KavioStatusCode kavio_app_set_ready_callback(
    KavioApp* app,
    KavioAppReadyCallback callback,
    void* user_data)
{
    ClearLastError();
    if (!app) return InvalidArgument("app is null");
    try {
        std::lock_guard<std::mutex> lock(app->callback_mutex);
        app->ready_callback = callback;
        app->ready_user_data = user_data;
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_app_set_ready_callback failed with an unknown exception");
    }
}

KavioStatusCode kavio_app_set_tick_callback(
    KavioApp* app, KavioAppTickCallback callback, void* user_data)
{
    ClearLastError();
    if (!app) return InvalidArgument("app is null");
    std::lock_guard<std::mutex> lock(app->callback_mutex);
    app->tick_callback = callback;
    app->tick_user_data = user_data;
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_app_set_stopping_callback(
    KavioApp* app, KavioAppStoppingCallback callback, void* user_data)
{
    ClearLastError();
    if (!app) return InvalidArgument("app is null");
    std::lock_guard<std::mutex> lock(app->callback_mutex);
    app->stopping_callback = callback;
    app->stopping_user_data = user_data;
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_app_get_presentation(KavioApp* app, KavioPresentation** out_presentation)
{
    ClearLastError();
    if (!app || !out_presentation) return InvalidArgument("app or out_presentation is null");
    try {
        *out_presentation = EnsurePresentationHandle(app);
        if (!*out_presentation) return InternalException("Presentation handle is unavailable");
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_app_get_presentation failed with an unknown exception");
    }
}

KavioStatusCode kavio_app_get_device(KavioApp* app, KavioDevice** out_device)
{
    ClearLastError();
    if (!app || !out_device) return InvalidArgument("app or out_device is null");
    *out_device = EnsureDeviceHandle(app);
    return *out_device ? KAVIO_STATUS_OK : InternalException("Device handle is unavailable");
}

KavioStatusCode kavio_app_run(KavioApp* app, int32_t* out_exit_code)
{
    ClearLastError();
    if (!app) return InvalidArgument("app is null");
    if (app->running.exchange(true)) {
        return SetError(KAVIO_STATUS_COMMON_ALREADY_EXISTS, "KavioApp is already running");
    }
    try {
        const int exit_code = app->app.run();
        app->running.store(false);
        if (out_exit_code) *out_exit_code = static_cast<int32_t>(exit_code);
        const kavio::Status status = app->app.last_status();
        if (!status.ok()) return ReturnStatus(status);
        return exit_code == 0
            ? KAVIO_STATUS_OK
            : SetError(KAVIO_STATUS_COMMON_INTERNAL_ERROR,
                       "KavioApp returned a non-zero exit code without an SDK error status");
    } catch (const std::exception& exc) {
        app->running.store(false);
        return InternalException(exc.what());
    } catch (...) {
        app->running.store(false);
        return InternalException("kavio_app_run failed with an unknown exception");
    }
}

KavioStatusCode kavio_app_request_stop(KavioApp* app)
{
    ClearLastError();
    if (!app) return InvalidArgument("app is null");
    app->app.request_stop();
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_app_last_status(KavioApp* app)
{
    ClearLastError();
    if (!app) return InvalidArgument("app is null");
    return ReturnStatus(app->app.last_status());
}

KavioStatusCode kavio_app_destroy(KavioApp* app)
{
    ClearLastError();
    if (!app) return KAVIO_STATUS_OK;
    if (app->running.load()) {
        return SetError(KAVIO_STATUS_COMMON_INVALID_STATE,
                        "kavio_app_destroy must be called after kavio_app_run returns");
    }
    delete app;
    return KAVIO_STATUS_OK;
}

uint8_t kavio_presentation_is_ready(KavioPresentation* presentation)
{
    ClearLastError();
    return presentation && presentation->presentation && presentation->presentation->IsReady() ? 1u : 0u;
}

uint8_t kavio_presentation_is_visible(KavioPresentation* presentation)
{
    ClearLastError();
    return presentation && presentation->presentation && presentation->presentation->IsVisible() ? 1u : 0u;
}

uint64_t kavio_presentation_generation(KavioPresentation* presentation)
{
    ClearLastError();
    return presentation && presentation->presentation ? presentation->presentation->Generation() : 0u;
}

KavioStatusCode kavio_presentation_get_frame_info(
    KavioPresentation* presentation,
    KavioFrameInfo* out_info)
{
    ClearLastError();
    if (!presentation || !presentation->presentation || !out_info) {
        return InvalidArgument("presentation or out_info is null");
    }
    if (!ValidStructHeader(out_info->struct_size, out_info->abi_version, KAVIO_FRAME_INFO_V1_SIZE)) {
        return InvalidArgument("KavioFrameInfo struct_size or abi_version is invalid");
    }
    if (!presentation->presentation->IsReady()) {
        return SetError(KAVIO_STATUS_COMMON_INVALID_STATE, "Presentation is not ready");
    }
    FillFrameInfo(presentation->presentation->FrameInfo(), *out_info);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_presentation_acquire_frame(
    KavioPresentation* presentation,
    KavioFrameLease** out_frame)
{
    ClearLastError();
    if (!presentation || !presentation->presentation || !out_frame) {
        return InvalidArgument("presentation or out_frame is null");
    }
    *out_frame = nullptr;
    try {
        kavio::FrameLease lease;
        const kavio::Status status = presentation->presentation->AcquireFrame(lease);
        if (!status.ok()) return ReturnStatus(status);
        *out_frame = new KavioFrameLease(std::move(lease));
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_presentation_acquire_frame failed with an unknown exception");
    }
}

KavioStatusCode kavio_presentation_clear(
    KavioPresentation* presentation,
    uint8_t blue,
    uint8_t green,
    uint8_t red,
    uint8_t alpha)
{
    ClearLastError();
    if (!presentation || !presentation->presentation) return InvalidArgument("presentation is null");
    try {
        return ReturnStatus(presentation->presentation->Clear(blue, green, red, alpha));
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_presentation_clear failed with an unknown exception");
    }
}

KavioStatusCode kavio_presentation_set_touch_callback(
    KavioPresentation* presentation,
    KavioTouchCallback callback,
    void* user_data)
{
    ClearLastError();
    if (!presentation || !presentation->presentation) return InvalidArgument("presentation is null");
    try {
        {
            std::lock_guard<std::mutex> lock(presentation->callback_mutex);
            presentation->touch_callback = callback;
            presentation->touch_user_data = user_data;
        }
        InstallTouchBridge(presentation);
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_presentation_set_touch_callback failed with an unknown exception");
    }
}

KavioStatusCode kavio_presentation_set_visibility_callback(
    KavioPresentation* presentation,
    KavioVisibilityCallback callback,
    void* user_data)
{
    ClearLastError();
    if (!presentation || !presentation->presentation) return InvalidArgument("presentation is null");
    try {
        {
            std::lock_guard<std::mutex> lock(presentation->callback_mutex);
            presentation->visibility_callback = callback;
            presentation->visibility_user_data = user_data;
        }
        InstallVisibilityBridge(presentation);
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_presentation_set_visibility_callback failed with an unknown exception");
    }
}

void kavio_frame_info_init(KavioFrameInfo* info)
{
    ClearLastError();
    if (!info) return;
    *info = KavioFrameInfo KAVIO_FRAME_INFO_INIT;
}

KavioStatusCode kavio_frame_get_info(KavioFrameLease* frame, KavioFrameInfo* out_info)
{
    ClearLastError();
    if (!frame || !out_info) return InvalidArgument("frame or out_info is null");
    if (!ValidStructHeader(out_info->struct_size, out_info->abi_version, KAVIO_FRAME_INFO_V1_SIZE)) {
        return InvalidArgument("KavioFrameInfo struct_size or abi_version is invalid");
    }
    FillFrameInfo(frame->lease.Info(), *out_info);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_frame_get_pixels(
    KavioFrameLease* frame,
    void** out_pixels,
    uint64_t* out_buffer_size)
{
    ClearLastError();
    if (!frame || !out_pixels) return InvalidArgument("frame or out_pixels is null");
    uint8_t* pixels = frame->lease.Data();
    if (!pixels) return SetError(KAVIO_STATUS_COMMON_INVALID_STATE, "FrameLease is not writable");
    *out_pixels = pixels;
    if (out_buffer_size) *out_buffer_size = frame->lease.Info().buffer_size;
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_frame_present(
    KavioFrameLease* frame,
    const KavioRect* dirty_rects,
    uint32_t dirty_rect_count)
{
    ClearLastError();
    if (!frame) return InvalidArgument("frame is null");
    if (dirty_rect_count != 0 && !dirty_rects) {
        return InvalidArgument("dirty_rects is null while dirty_rect_count is non-zero");
    }
    try {
        std::vector<kavio::Rect> converted;
        converted.reserve(dirty_rect_count);
        for (uint32_t i = 0; i < dirty_rect_count; ++i) {
            converted.push_back(kavio::Rect{
                dirty_rects[i].pos_x,
                dirty_rects[i].pos_y,
                dirty_rects[i].width,
                dirty_rects[i].height,
            });
        }
        const kavio::Status status = frame->lease.Present(
            converted.empty() ? nullptr : converted.data(), converted.size());
        frame->completed = frame->lease.Data() == nullptr;
        return ReturnStatus(status);
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_frame_present failed with an unknown exception");
    }
}

KavioStatusCode kavio_frame_discard(KavioFrameLease* frame)
{
    ClearLastError();
    if (!frame) return InvalidArgument("frame is null");
    try {
        const kavio::Status status = frame->lease.Discard();
        frame->completed = frame->lease.Data() == nullptr;
        return ReturnStatus(status);
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_frame_discard failed with an unknown exception");
    }
}

KavioStatusCode kavio_frame_destroy(KavioFrameLease* frame)
{
    ClearLastError();
    if (!frame) return KAVIO_STATUS_OK;
    if (!frame->completed) {
        if (frame->owner_thread != std::this_thread::get_id()) {
            return SetError(KAVIO_STATUS_COMMON_INVALID_STATE,
                            "FrameLease must be destroyed on the acquiring thread");
        }
        const kavio::Status discard_status = frame->lease.Discard();
        frame->completed = frame->lease.Data() == nullptr;
        if (!frame->completed) return ReturnStatus(discard_status);
    }
    delete frame;
    return KAVIO_STATUS_OK;
}


void kavio_runtime_data_init(KavioRuntimeData* data)
{
    ClearLastError();
    if (!data) return;
    std::memset(data, 0, sizeof(*data));
    data->struct_size = sizeof(*data);
    data->abi_version = KAVIO_ABI_VERSION;
}

void kavio_rx_info_init(KavioRxInfo* info)
{
    ClearLastError();
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->abi_version = KAVIO_ABI_VERSION;
}

void kavio_image_frame_info_init(KavioImageFrameInfo* info)
{
    ClearLastError();
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->abi_version = KAVIO_ABI_VERSION;
}

void kavio_tx_surface_info_init(KavioTxSurfaceInfo* info)
{
    ClearLastError();
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->abi_version = KAVIO_ABI_VERSION;
}

void kavio_h264_stream_info_init(KavioH264StreamInfo* info)
{
    ClearLastError();
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->abi_version = KAVIO_ABI_VERSION;
}

void kavio_h264_packet_info_init(KavioH264PacketInfo* info)
{
    ClearLastError();
    if (!info) return;
    std::memset(info, 0, sizeof(*info));
    info->struct_size = sizeof(*info);
    info->abi_version = KAVIO_ABI_VERSION;
}

uint8_t kavio_device_is_ready(KavioDevice* device)
{
    ClearLastError();
    return device && device->device && device->device->IsReady() ? 1u : 0u;
}

uint8_t kavio_device_is_usb_host_online(KavioDevice* device)
{
    ClearLastError();
    return device && device->device && device->device->IsUSBHostOnline() ? 1u : 0u;
}

uint8_t kavio_device_is_comm_channel_ready(KavioDevice* device)
{
    ClearLastError();
    return device && device->device && device->device->IsCommChannelReady() ? 1u : 0u;
}

uint32_t kavio_device_get_comm_channel_id(KavioDevice* device)
{
    ClearLastError();
    return device && device->device ? device->device->GetCommChannelId() : 0u;
}

KavioStatusCode kavio_device_get_runtime_data(KavioDevice* device, KavioRuntimeData* out_data)
{
    ClearLastError();
    if (!device || !device->device || !out_data) return InvalidArgument("device or out_data is null");
    if (!ValidStructHeader(out_data->struct_size, out_data->abi_version, sizeof(KavioRuntimeData))) {
        return InvalidArgument("KavioRuntimeData struct_size or abi_version is invalid");
    }
    const auto source = device->device->GetRuntimeData();
    out_data->rx.status = static_cast<int32_t>(source.rx.status);
    out_data->rx.width = source.rx.width;
    out_data->rx.height = source.rx.height;
    out_data->rx.fps = source.rx.fps;
    out_data->rx.pixel_format = static_cast<uint32_t>(ToCPixelFormat(source.rx.pixel_format));
    out_data->tx.status = static_cast<int32_t>(source.tx.status);
    out_data->tx.width = source.tx.width;
    out_data->tx.height = source.tx.height;
    out_data->tx.fps = source.tx.fps;
    out_data->tx.pixel_format = static_cast<uint32_t>(ToCPixelFormat(source.tx.pixel_format));
    out_data->input.mode = static_cast<int32_t>(source.input.mode);
    out_data->input.channel = static_cast<int32_t>(source.input.channel);
    out_data->input.connection_status = static_cast<int32_t>(source.input.connection_status);
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_get_rx_info(KavioDevice* device, KavioRxInfo* out_info)
{
    ClearLastError();
    if (!device || !device->device || !out_info) return InvalidArgument("device or out_info is null");
    if (!ValidStructHeader(out_info->struct_size, out_info->abi_version, sizeof(KavioRxInfo))) {
        return InvalidArgument("KavioRxInfo struct_size or abi_version is invalid");
    }
    const auto source = device->device->GetRxInfo();
    out_info->connected = source.connected ? 1u : 0u;
    out_info->frame_available = source.frame_available ? 1u : 0u;
    out_info->width = source.width;
    out_info->height = source.height;
    out_info->stride = source.stride;
    out_info->pixel_format = static_cast<uint32_t>(ToCPixelFormat(source.pixel_format));
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_set_rx_params(
    KavioDevice* device, uint8_t enabled, uint32_t frame_sample_interval, uint64_t* out_request_id)
{
    ClearLastError();
    if (!device || !device->device || !out_request_id) return InvalidArgument("device or out_request_id is null");
    return ReturnStatus(device->device->SetRxParams(
        enabled != 0, frame_sample_interval, *out_request_id));
}

KavioStatusCode kavio_device_set_tx_params(
    KavioDevice* device, uint8_t enabled, int32_t z_order, uint64_t* out_request_id)
{
    ClearLastError();
    if (!device || !device->device || !out_request_id) return InvalidArgument("device or out_request_id is null");
    return ReturnStatus(device->device->SetTxParams(enabled != 0, z_order, *out_request_id));
}

KavioStatusCode kavio_device_read_latest_rx_frame(KavioDevice* device, KavioImageFrame** out_frame)
{
    ClearLastError();
    if (!device || !device->device || !out_frame) return InvalidArgument("device or out_frame is null");
    *out_frame = nullptr;
    try {
        auto frame = std::make_unique<KavioImageFrame>();
        const auto status = device->device->ReadLatestRxFrame(frame->frame);
        if (!status.ok()) return ReturnStatus(status);
        *out_frame = frame.release();
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_device_read_latest_rx_frame failed");
    }
}

KavioStatusCode kavio_image_frame_get_info(KavioImageFrame* frame, KavioImageFrameInfo* out_info)
{
    ClearLastError();
    if (!frame || !out_info) return InvalidArgument("frame or out_info is null");
    if (!ValidStructHeader(out_info->struct_size, out_info->abi_version, sizeof(KavioImageFrameInfo))) {
        return InvalidArgument("KavioImageFrameInfo struct_size or abi_version is invalid");
    }
    const auto& source = frame->frame;
    out_info->frame_id = source.frame_id;
    out_info->timestamp_ns = source.timestamp_ns;
    out_info->width = source.width;
    out_info->height = source.height;
    out_info->stride = source.stride;
    out_info->pixel_format = static_cast<uint32_t>(ToCPixelFormat(source.pixel_format));
    out_info->dest_pos_x = source.dest_pos_x;
    out_info->dest_pos_y = source.dest_pos_y;
    out_info->valid_rect = KavioRect{source.valid_rect.pos_x, source.valid_rect.pos_y,
                                    source.valid_rect.width, source.valid_rect.height};
    out_info->data_size = source.data.size();
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_image_frame_get_data(
    KavioImageFrame* frame, const uint8_t** out_data, uint64_t* out_size)
{
    ClearLastError();
    if (!frame || !out_data) return InvalidArgument("frame or out_data is null");
    *out_data = frame->frame.data.empty() ? nullptr : frame->frame.data.data();
    if (out_size) *out_size = frame->frame.data.size();
    return KAVIO_STATUS_OK;
}

void kavio_image_frame_destroy(KavioImageFrame* frame)
{
    ClearLastError();
    delete frame;
}

KavioStatusCode kavio_device_create_tx_surface(
    KavioDevice* device, uint32_t width, uint32_t height, uint64_t* out_request_id)
{
    ClearLastError();
    if (!device || !device->device || !out_request_id) return InvalidArgument("device or out_request_id is null");
    return ReturnStatus(device->device->CreateTxSurface(width, height, *out_request_id));
}

KavioStatusCode kavio_device_destroy_tx_surface(
    KavioDevice* device, uint32_t surface_id, uint64_t* out_request_id)
{
    ClearLastError();
    if (!device || !device->device || !out_request_id) return InvalidArgument("device or out_request_id is null");
    return ReturnStatus(device->device->DestroyTxSurface(surface_id, *out_request_id));
}

KavioStatusCode kavio_tx_surface_get_info(KavioTxSurface* surface, KavioTxSurfaceInfo* out_info)
{
    ClearLastError();
    if (!surface || !out_info) return InvalidArgument("surface or out_info is null");
    if (!ValidStructHeader(out_info->struct_size, out_info->abi_version, sizeof(KavioTxSurfaceInfo))) {
        return InvalidArgument("KavioTxSurfaceInfo struct_size or abi_version is invalid");
    }
    if (!surface->surface.IsValid()) return SetError(KAVIO_STATUS_COMMON_INVALID_STATE, "TX Surface is invalid");
    out_info->surface_id = surface->surface.id();
    out_info->width = surface->surface.width();
    out_info->height = surface->surface.height();
    out_info->stride = surface->surface.stride();
    out_info->pixel_format = static_cast<uint32_t>(ToCPixelFormat(surface->surface.pixel_format()));
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_tx_surface_write(
    KavioTxSurface* surface, const void* data, uint64_t data_size,
    uint32_t width, uint32_t height, uint32_t stride, uint32_t pixel_format,
    int32_t dest_pos_x, int32_t dest_pos_y, const KavioRect* valid_rect)
{
    ClearLastError();
    if (!surface || !data) return InvalidArgument("surface or data is null");
    if (data_size > static_cast<uint64_t>(SIZE_MAX)) return InvalidArgument("data_size is too large");
    kavio::ImageFrame frame;
    frame.width = width;
    frame.height = height;
    frame.stride = stride;
    frame.pixel_format = ToCppPixelFormat(pixel_format);
    frame.dest_pos_x = dest_pos_x;
    frame.dest_pos_y = dest_pos_y;
    frame.valid_rect = valid_rect
        ? kavio::Rect{valid_rect->pos_x, valid_rect->pos_y, valid_rect->width, valid_rect->height}
        : kavio::Rect{0, 0, width, height};
    const auto* bytes = static_cast<const uint8_t*>(data);
    frame.data.assign(bytes, bytes + static_cast<size_t>(data_size));
    return ReturnStatus(surface->surface.WriteFrame(frame));
}

KavioStatusCode kavio_tx_surface_clear(KavioTxSurface* surface)
{
    ClearLastError();
    if (!surface) return InvalidArgument("surface is null");
    return ReturnStatus(surface->surface.Clear());
}

KavioStatusCode kavio_tx_surface_request_destroy(KavioTxSurface* surface, uint64_t* out_request_id)
{
    ClearLastError();
    if (!surface || !out_request_id) return InvalidArgument("surface or out_request_id is null");
    return ReturnStatus(surface->surface.Destroy(*out_request_id));
}

void kavio_tx_surface_destroy(KavioTxSurface* surface)
{
    ClearLastError();
    delete surface;
}

KavioStatusCode kavio_device_send_keyboard(
    KavioDevice* device, int32_t key_option, int32_t key_code, uint64_t timestamp_ns)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    return ReturnStatus(device->device->SendKeyboard(
        static_cast<kavio::KeyOption>(key_option),
        static_cast<kavio::KeyCode>(key_code), timestamp_ns));
}

KavioStatusCode kavio_device_send_mouse(
    KavioDevice* device, int32_t mouse_option, int32_t x, int32_t y,
    int32_t wheel, uint64_t timestamp_ns)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    return ReturnStatus(device->device->SendMouse(
        static_cast<kavio::MouseOption>(mouse_option), x, y, wheel, timestamp_ns));
}

KavioStatusCode kavio_device_send_comm_data(KavioDevice* device, const void* data, uint64_t size)
{
    ClearLastError();
    if (!device || !device->device || !data) return InvalidArgument("device or data is null");
    if (size > static_cast<uint64_t>(SIZE_MAX)) return InvalidArgument("size is too large");
    return ReturnStatus(device->device->SendCommChannelData(data, static_cast<size_t>(size)));
}

KavioStatusCode kavio_device_get_app_config_json(
    KavioDevice* device, char** out_json, uint64_t* out_size)
{
    ClearLastError();
    if (!device || !device->device || !out_json) return InvalidArgument("device or out_json is null");
    *out_json = nullptr;
    try {
        const std::string encoded = device->device->GetAppConfigJson();
        char* copy = static_cast<char*>(std::malloc(encoded.size() + 1));
        if (!copy) return SetError(KAVIO_STATUS_COMMON_INTERNAL_ERROR, "memory allocation failed");
        std::memcpy(copy, encoded.data(), encoded.size());
        copy[encoded.size()] = '\0';
        *out_json = copy;
        if (out_size) *out_size = encoded.size();
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_device_get_app_config_json failed");
    }
}

KavioStatusCode kavio_device_update_app_config_json(
    KavioDevice* device, const char* json, uint64_t size)
{
    ClearLastError();
    if (!device || !device->device || (!json && size != 0)) return InvalidArgument("device or json is invalid");
    if (size > static_cast<uint64_t>(SIZE_MAX)) return InvalidArgument("size is too large");
    const char* begin = json ? json : "";
    return ReturnStatus(device->device->UpdateAppConfigJson(
        std::string(begin, begin + static_cast<size_t>(size))));
}

KavioStatusCode kavio_device_reset_app_config(KavioDevice* device)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    return ReturnStatus(device->device->ResetAppConfigToDefaults());
}

KavioStatusCode kavio_device_subscribe_h264(KavioDevice* device, uint64_t* out_request_id)
{
    ClearLastError();
    if (!device || !device->device || !out_request_id) return InvalidArgument("device or out_request_id is null");
    return ReturnStatus(device->device->SubscribeH264Stream(*out_request_id));
}

KavioStatusCode kavio_device_unsubscribe_h264(
    KavioDevice* device, uint32_t stream_id, uint64_t* out_request_id)
{
    ClearLastError();
    if (!device || !device->device || !out_request_id) return InvalidArgument("device or out_request_id is null");
    return ReturnStatus(device->device->UnsubscribeH264Stream(stream_id, *out_request_id));
}

uint64_t kavio_device_get_latest_h264_packet_seq(KavioDevice* device, uint32_t stream_id)
{
    ClearLastError();
    return device && device->device ? device->device->GetLatestH264PacketSeq(stream_id) : 0u;
}

KavioStatusCode kavio_device_read_h264_packet(
    KavioDevice* device, uint32_t stream_id, uint64_t seq, KavioH264Packet** out_packet)
{
    ClearLastError();
    if (!device || !device->device || !out_packet) return InvalidArgument("device or out_packet is null");
    *out_packet = nullptr;
    try {
        auto packet = std::make_unique<KavioH264Packet>();
        const auto status = device->device->ReadH264Packet(stream_id, seq, packet->packet);
        if (!status.ok()) return ReturnStatus(status);
        *out_packet = packet.release();
        return KAVIO_STATUS_OK;
    } catch (const std::exception& exc) {
        return InternalException(exc.what());
    } catch (...) {
        return InternalException("kavio_device_read_h264_packet failed");
    }
}

KavioStatusCode kavio_h264_packet_get_info(KavioH264Packet* packet, KavioH264PacketInfo* out_info)
{
    ClearLastError();
    if (!packet || !out_info) return InvalidArgument("packet or out_info is null");
    if (!ValidStructHeader(out_info->struct_size, out_info->abi_version, sizeof(KavioH264PacketInfo))) {
        return InvalidArgument("KavioH264PacketInfo struct_size or abi_version is invalid");
    }
    const auto& source = packet->packet;
    out_info->seq = source.seq;
    out_info->pts_us = source.pts_us;
    out_info->dts_us = source.dts_us;
    out_info->timestamp_ns = source.timestamp_ns;
    out_info->flags = source.flags;
    out_info->nal_type = source.nal_type;
    out_info->data_size = source.data.size();
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_h264_packet_get_data(
    KavioH264Packet* packet, const uint8_t** out_data, uint64_t* out_size)
{
    ClearLastError();
    if (!packet || !out_data) return InvalidArgument("packet or out_data is null");
    *out_data = packet->packet.data.empty() ? nullptr : packet->packet.data.data();
    if (out_size) *out_size = packet->packet.data.size();
    return KAVIO_STATUS_OK;
}

void kavio_h264_packet_destroy(KavioH264Packet* packet)
{
    ClearLastError();
    delete packet;
}

KavioStatusCode kavio_device_set_comm_data_callback(
    KavioDevice* device, KavioCommDataCallback callback, void* user_data)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    {
        std::lock_guard<std::mutex> lock(device->callback_mutex);
        device->comm_data_callback = callback;
        device->comm_data_user_data = user_data;
    }
    if (!callback) {
        device->device->SetCommDataHandler({});
        return KAVIO_STATUS_OK;
    }
    device->device->SetCommDataHandler([device](const std::vector<uint8_t>& data) {
        KavioCommDataCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(device->callback_mutex);
            cb = device->comm_data_callback;
            ud = device->comm_data_user_data;
        }
        if (cb) cb(device, data.empty() ? nullptr : data.data(), data.size(), ud);
    });
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_set_tx_surface_callback(
    KavioDevice* device, KavioTxSurfaceCallback callback, void* user_data)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    {
        std::lock_guard<std::mutex> lock(device->callback_mutex);
        device->tx_surface_callback = callback;
        device->tx_surface_user_data = user_data;
    }
    if (!callback) {
        device->device->SetTxSurfaceHandler({});
        return KAVIO_STATUS_OK;
    }
    device->device->SetTxSurfaceHandler([device](uint64_t request_id, kavio::Status status, kavio::DeviceTxSurface surface) mutable {
        KavioTxSurfaceCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(device->callback_mutex);
            cb = device->tx_surface_callback;
            ud = device->tx_surface_user_data;
        }
        if (!cb) {
            if (surface.IsValid()) { uint64_t ignored = 0; (void)surface.Destroy(ignored); }
            return;
        }
        KavioTxSurface* wrapper = nullptr;
        if (status.ok() && surface.IsValid()) {
            wrapper = new (std::nothrow) KavioTxSurface{std::move(surface)};
            if (!wrapper) status = kavio::Status::Error(kavio::StatusCode::Common_InternalError, "memory allocation failed");
        }
        cb(device, request_id, static_cast<KavioStatusCode>(status.code), wrapper, ud);
    });
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_set_h264_stream_callback(
    KavioDevice* device, KavioH264StreamCallback callback, void* user_data)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    {
        std::lock_guard<std::mutex> lock(device->callback_mutex);
        device->h264_stream_callback = callback;
        device->h264_stream_user_data = user_data;
    }
    if (!callback) {
        device->device->SetH264StreamHandler({});
        return KAVIO_STATUS_OK;
    }
    device->device->SetH264StreamHandler([device](uint64_t request_id, kavio::Status status, kavio::DeviceH264StreamInfo source) {
        KavioH264StreamCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(device->callback_mutex);
            cb = device->h264_stream_callback;
            ud = device->h264_stream_user_data;
        }
        if (!cb) return;
        KavioH264StreamInfo info{};
        info.struct_size = sizeof(info);
        info.abi_version = KAVIO_ABI_VERSION;
        info.stream_id = source.stream_id;
        info.width = source.width;
        info.height = source.height;
        info.fps = source.fps;
        info.bitrate_kbps = source.bitrate_kbps;
        info.gop = source.gop;
        info.packet_capacity = source.packet_capacity;
        info.payload_capacity = source.payload_capacity;
        cb(device, request_id, static_cast<KavioStatusCode>(status.code), &info, ud);
    });
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_set_h264_invalidated_callback(
    KavioDevice* device, KavioH264InvalidatedCallback callback, void* user_data)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    {
        std::lock_guard<std::mutex> lock(device->callback_mutex);
        device->h264_invalidated_callback = callback;
        device->h264_invalidated_user_data = user_data;
    }
    if (!callback) {
        device->device->SetH264InvalidatedHandler({});
        return KAVIO_STATUS_OK;
    }
    device->device->SetH264InvalidatedHandler([device](const kavio::DeviceH264InvalidatedEvent& source) {
        KavioH264InvalidatedCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(device->callback_mutex);
            cb = device->h264_invalidated_callback;
            ud = device->h264_invalidated_user_data;
        }
        if (!cb) return;
        KavioH264InvalidatedEvent event{sizeof(KavioH264InvalidatedEvent), KAVIO_ABI_VERSION,
                                        source.stream_id, source.reason};
        cb(device, &event, ud);
    });
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_set_error_callback(
    KavioDevice* device, KavioDeviceErrorCallback callback, void* user_data)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    {
        std::lock_guard<std::mutex> lock(device->callback_mutex);
        device->error_callback = callback;
        device->error_user_data = user_data;
    }
    if (!callback) {
        device->device->SetErrorHandler({});
        return KAVIO_STATUS_OK;
    }
    device->device->SetErrorHandler([device](const kavio::DeviceErrorEvent& source) {
        KavioDeviceErrorCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(device->callback_mutex);
            cb = device->error_callback;
            ud = device->error_user_data;
        }
        if (!cb) return;
        KavioDeviceErrorEvent event{sizeof(KavioDeviceErrorEvent), KAVIO_ABI_VERSION,
                                    source.track_id, source.code, source.message.c_str()};
        cb(device, &event, ud);
    });
    return KAVIO_STATUS_OK;
}

KavioStatusCode kavio_device_set_config_changed_callback(
    KavioDevice* device, KavioConfigChangedCallback callback, void* user_data)
{
    ClearLastError();
    if (!device || !device->device) return InvalidArgument("device is null");
    {
        std::lock_guard<std::mutex> lock(device->callback_mutex);
        device->config_changed_callback = callback;
        device->config_changed_user_data = user_data;
    }
    if (!callback) {
        device->device->SetConfigChangedHandler({});
        return KAVIO_STATUS_OK;
    }
    device->device->SetConfigChangedHandler([device]() {
        KavioConfigChangedCallback cb = nullptr;
        void* ud = nullptr;
        {
            std::lock_guard<std::mutex> lock(device->callback_mutex);
            cb = device->config_changed_callback;
            ud = device->config_changed_user_data;
        }
        if (cb) cb(device, ud);
    });
    return KAVIO_STATUS_OK;
}

}  // extern "C"
