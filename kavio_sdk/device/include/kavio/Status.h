#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace kavio {

enum StatusCode : int32_t {
    /* 通用状态：0~99 */
    OK = 0,
    Common_InvalidArgument = 1,
    Common_AlreadyExists = 2,
    Common_NotFound = 3,
    Common_InternalError = 4,
    Common_InvalidState = 5,
    Common_Cancelled = 6,
    Common_Unsupported = 7,
    Common_NotConnected = 8,
    Common_Timeout = 9,
    Common_ProtocolError = 10,

    /* 会话状态：100~149 */
    Session_TooManySessions = 100,

    /* 媒体状态：150~199 */
    Media_VideoNotReady = 150,
    Media_NoFrame = 151,

    /* SHM 状态：200~209 */
    SHM_CreateFailed = 200,
    SHM_OperationFailed = 201,

    /* IPC 状态：210~299 */
    IPC_SocketError = 210,

    /* USB SystemChannel 与 Device Component（设备组件）管理状态：300~399 */
    USB_BodyCaseNotSupported = 300,
    USB_AppVersionMismatch = 301,
    USB_AppSignatureInvalid = 302,
    USB_AppFileHashMismatch = 303,
    USB_AppDeveloperModeRequired = 304,
    USB_AppEnvironmentInstallFailed = 305,
    USB_AppUnsupportedRuntime = 306,
    USB_AppInstallInProgress = 307,

    /* H264 硬件编码状态：400~449 */
    H264_InvalidArgument = 400,
    H264_UnsupportedCodec = 401,
    H264_InvalidConfig = 402,
    H264_NotInitialized = 403,
    H264_StreamAlreadyExists = 404,
    H264_StreamNotFound = 405,
    H264_StreamShmCreateFailed = 406,
    H264_RuntimeBitrateUnsupported = 407,
    H264_RuntimeIDRUnsupported = 408,

    H264_VencCreateFailed = 409,
    H264_VencOpenFailed = 410,
    H264_VencInputConnectFailed = 411,
    H264_VencOutputConnectFailed = 412,
    H264_VencNotOpened = 413,
    H264_VencStartFailed = 414,

    H264_InputBDChainCreateFailed = 415,
    H264_InputBDAllocateFailed = 416,
    H264_InputShmAllocateFailed = 417,
    H264_InputBDTagAppendFailed = 418,
    H264_InputBDPushFailed = 419,

    H264_OutputBDChainCreateFailed = 420,
    H264_OutputBDAllocateFailed = 421,
    H264_OutputShmAllocateFailed = 422,
    H264_OutputBDTagAppendFailed = 423,
    H264_OutputBDPushFailed = 424,
    H264_OutputPathNotReady = 425,
    H264_OutputBDChainQueryFailed = 426,

    /* DRM OSD 显示状态：450~499 */
    DRM_DeviceOpenFailed = 450,
    DRM_DeviceOwnedByDisplayServer = 451,
    DRM_SetMasterFailed = 452,
    DRM_OutputTargetNotFound = 453,
    DRM_ArgbPlaneNotFound = 454,
    DRM_DumbBufferCreateFailed = 455,
    DRM_InitialCrtcSetFailed = 456,

    DRM_NotInitialized = 460,
    DRM_FrameAlreadyInProgress = 461,
    DRM_FrameNotStarted = 462,
    DRM_TopLayerUpdateDuringFrame = 463,

    DRM_BackBufferUnavailable = 470,
    DRM_ScanoutBufferUnavailable = 471,
    DRM_BackFramebufferUnavailable = 472,

    DRM_UnsupportedPixelFormat = 480,
    DRM_InvalidLayerBuffer = 481,
    DRM_InvalidLayerGeometry = 482,
    DRM_LayerPayloadTooSmall = 483,
    DRM_InvalidTopLayerBuffer = 484,
    DRM_TopLayerSizeMismatch = 485,
    DRM_InvalidTopLayerStride = 486,

    DRM_SetPlaneFailed = 490,
    DRM_UnsupportedPlaneType = 491,
};

inline const char* StatusCodeToString(StatusCode code)
{
    switch (code) {
    case StatusCode::OK:
        return "StatusCode::OK";
    case StatusCode::Common_InvalidArgument:
        return "StatusCode::Common_InvalidArgument";
    case StatusCode::Common_AlreadyExists:
        return "StatusCode::Common_AlreadyExists";
    case StatusCode::Common_NotFound:
        return "StatusCode::Common_NotFound";
    case StatusCode::Common_InternalError:
        return "StatusCode::Common_InternalError";
    case StatusCode::Common_InvalidState:
        return "StatusCode::Common_InvalidState";
    case StatusCode::Common_Cancelled:
        return "StatusCode::Common_Cancelled";
    case StatusCode::Common_Unsupported:
        return "StatusCode::Common_Unsupported";
    case StatusCode::Common_NotConnected:
        return "StatusCode::Common_NotConnected";
    case StatusCode::Common_Timeout:
        return "StatusCode::Common_Timeout";
    case StatusCode::Common_ProtocolError:
        return "StatusCode::Common_ProtocolError";
    case StatusCode::Session_TooManySessions:
        return "StatusCode::Session_TooManySessions";
    case StatusCode::Media_VideoNotReady:
        return "StatusCode::Media_VideoNotReady";
    case StatusCode::Media_NoFrame:
        return "StatusCode::Media_NoFrame";
    case StatusCode::SHM_CreateFailed:
        return "StatusCode::SHM_CreateFailed";
    case StatusCode::SHM_OperationFailed:
        return "StatusCode::SHM_OperationFailed";
    case StatusCode::IPC_SocketError:
        return "StatusCode::IPC_SocketError";
    case StatusCode::USB_BodyCaseNotSupported:
        return "StatusCode::USB_BodyCaseNotSupported";
    case StatusCode::USB_AppVersionMismatch:
        return "StatusCode::USB_AppVersionMismatch";
    case StatusCode::USB_AppSignatureInvalid:
        return "StatusCode::USB_AppSignatureInvalid";
    case StatusCode::USB_AppFileHashMismatch:
        return "StatusCode::USB_AppFileHashMismatch";
    case StatusCode::USB_AppDeveloperModeRequired:
        return "StatusCode::USB_AppDeveloperModeRequired";
    case StatusCode::USB_AppEnvironmentInstallFailed:
        return "StatusCode::USB_AppEnvironmentInstallFailed";
    case StatusCode::USB_AppUnsupportedRuntime:
        return "StatusCode::USB_AppUnsupportedRuntime";
    case StatusCode::USB_AppInstallInProgress:
        return "StatusCode::USB_AppInstallInProgress";
    case StatusCode::H264_InvalidArgument:
        return "StatusCode::H264_InvalidArgument";
    case StatusCode::H264_UnsupportedCodec:
        return "StatusCode::H264_UnsupportedCodec";
    case StatusCode::H264_InvalidConfig:
        return "StatusCode::H264_InvalidConfig";
    case StatusCode::H264_NotInitialized:
        return "StatusCode::H264_NotInitialized";
    case StatusCode::H264_StreamAlreadyExists:
        return "StatusCode::H264_StreamAlreadyExists";
    case StatusCode::H264_StreamNotFound:
        return "StatusCode::H264_StreamNotFound";
    case StatusCode::H264_StreamShmCreateFailed:
        return "StatusCode::H264_StreamShmCreateFailed";
    case StatusCode::H264_RuntimeBitrateUnsupported:
        return "StatusCode::H264_RuntimeBitrateUnsupported";
    case StatusCode::H264_RuntimeIDRUnsupported:
        return "StatusCode::H264_RuntimeIDRUnsupported";
    case StatusCode::H264_VencCreateFailed:
        return "StatusCode::H264_VencCreateFailed";
    case StatusCode::H264_VencOpenFailed:
        return "StatusCode::H264_VencOpenFailed";
    case StatusCode::H264_VencInputConnectFailed:
        return "StatusCode::H264_VencInputConnectFailed";
    case StatusCode::H264_VencOutputConnectFailed:
        return "StatusCode::H264_VencOutputConnectFailed";
    case StatusCode::H264_VencNotOpened:
        return "StatusCode::H264_VencNotOpened";
    case StatusCode::H264_VencStartFailed:
        return "StatusCode::H264_VencStartFailed";
    case StatusCode::H264_InputBDChainCreateFailed:
        return "StatusCode::H264_InputBDChainCreateFailed";
    case StatusCode::H264_InputBDAllocateFailed:
        return "StatusCode::H264_InputBDAllocateFailed";
    case StatusCode::H264_InputShmAllocateFailed:
        return "StatusCode::H264_InputShmAllocateFailed";
    case StatusCode::H264_InputBDTagAppendFailed:
        return "StatusCode::H264_InputBDTagAppendFailed";
    case StatusCode::H264_InputBDPushFailed:
        return "StatusCode::H264_InputBDPushFailed";
    case StatusCode::H264_OutputBDChainCreateFailed:
        return "StatusCode::H264_OutputBDChainCreateFailed";
    case StatusCode::H264_OutputBDAllocateFailed:
        return "StatusCode::H264_OutputBDAllocateFailed";
    case StatusCode::H264_OutputShmAllocateFailed:
        return "StatusCode::H264_OutputShmAllocateFailed";
    case StatusCode::H264_OutputBDTagAppendFailed:
        return "StatusCode::H264_OutputBDTagAppendFailed";
    case StatusCode::H264_OutputBDPushFailed:
        return "StatusCode::H264_OutputBDPushFailed";
    case StatusCode::H264_OutputPathNotReady:
        return "StatusCode::H264_OutputPathNotReady";
    case StatusCode::H264_OutputBDChainQueryFailed:
        return "StatusCode::H264_OutputBDChainQueryFailed";
    case StatusCode::DRM_DeviceOpenFailed:
        return "StatusCode::DRM_DeviceOpenFailed";
    case StatusCode::DRM_DeviceOwnedByDisplayServer:
        return "StatusCode::DRM_DeviceOwnedByDisplayServer";
    case StatusCode::DRM_SetMasterFailed:
        return "StatusCode::DRM_SetMasterFailed";
    case StatusCode::DRM_OutputTargetNotFound:
        return "StatusCode::DRM_OutputTargetNotFound";
    case StatusCode::DRM_ArgbPlaneNotFound:
        return "StatusCode::DRM_ArgbPlaneNotFound";
    case StatusCode::DRM_DumbBufferCreateFailed:
        return "StatusCode::DRM_DumbBufferCreateFailed";
    case StatusCode::DRM_InitialCrtcSetFailed:
        return "StatusCode::DRM_InitialCrtcSetFailed";
    case StatusCode::DRM_NotInitialized:
        return "StatusCode::DRM_NotInitialized";
    case StatusCode::DRM_FrameAlreadyInProgress:
        return "StatusCode::DRM_FrameAlreadyInProgress";
    case StatusCode::DRM_FrameNotStarted:
        return "StatusCode::DRM_FrameNotStarted";
    case StatusCode::DRM_TopLayerUpdateDuringFrame:
        return "StatusCode::DRM_TopLayerUpdateDuringFrame";
    case StatusCode::DRM_BackBufferUnavailable:
        return "StatusCode::DRM_BackBufferUnavailable";
    case StatusCode::DRM_ScanoutBufferUnavailable:
        return "StatusCode::DRM_ScanoutBufferUnavailable";
    case StatusCode::DRM_BackFramebufferUnavailable:
        return "StatusCode::DRM_BackFramebufferUnavailable";
    case StatusCode::DRM_UnsupportedPixelFormat:
        return "StatusCode::DRM_UnsupportedPixelFormat";
    case StatusCode::DRM_InvalidLayerBuffer:
        return "StatusCode::DRM_InvalidLayerBuffer";
    case StatusCode::DRM_InvalidLayerGeometry:
        return "StatusCode::DRM_InvalidLayerGeometry";
    case StatusCode::DRM_LayerPayloadTooSmall:
        return "StatusCode::DRM_LayerPayloadTooSmall";
    case StatusCode::DRM_InvalidTopLayerBuffer:
        return "StatusCode::DRM_InvalidTopLayerBuffer";
    case StatusCode::DRM_TopLayerSizeMismatch:
        return "StatusCode::DRM_TopLayerSizeMismatch";
    case StatusCode::DRM_InvalidTopLayerStride:
        return "StatusCode::DRM_InvalidTopLayerStride";
    case StatusCode::DRM_SetPlaneFailed:
        return "StatusCode::DRM_SetPlaneFailed";
    case StatusCode::DRM_UnsupportedPlaneType:
        return "StatusCode::DRM_UnsupportedPlaneType";
    default:
        return "StatusCode::Unknown";
    }
}

inline const char* StatusCodeToString(int32_t code)
{
    return StatusCodeToString(static_cast<StatusCode>(code));
}

struct Status {
    StatusCode code;
    std::string message;

    Status()
        : code(StatusCode::OK)
    {
    }

    Status(StatusCode status_code, std::string status_message)
        : code(status_code), message(std::move(status_message))
    {
    }

    bool ok() const { return code == StatusCode::OK; }
    const char* name() const { return StatusCodeToString(code); }

    static Status Ok() { return Status(); }
    static Status Error(StatusCode code, std::string message)
    {
        return Status(code, std::move(message));
    }
};

}  // namespace kavio
