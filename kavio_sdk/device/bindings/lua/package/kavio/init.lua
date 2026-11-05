local App = require("kavio.app")
local native = require("kavio.native")

return {
    App = App,
    -- Monotonic seconds for measuring elapsed time; not calendar time.
    monotonic_seconds = native.monotonic_seconds,
    ABI_VERSION = native.ABI_VERSION,
    PixelFormat = {
        UNKNOWN = native.PIXEL_FORMAT_UNKNOWN,
        UYVY = native.PIXEL_FORMAT_UYVY,
        YUYV = native.PIXEL_FORMAT_YUYV,
        BGRA8888 = native.PIXEL_FORMAT_BGRA8888,
    },
    RuntimeVideoStatus = {
        UNKNOWN = native.RUNTIME_VIDEO_STATUS_UNKNOWN,
        NOT_CONNECTED = native.RUNTIME_VIDEO_STATUS_NOT_CONNECTED,
        CONNECTED = native.RUNTIME_VIDEO_STATUS_CONNECTED,
        RUNNING = native.RUNTIME_VIDEO_STATUS_RUNNING,
        STOPPED = native.RUNTIME_VIDEO_STATUS_STOPPED,
        ERROR = native.RUNTIME_VIDEO_STATUS_ERROR,
    },
    InputMode = {
        BLE = native.INPUT_MODE_BLE,
        RF24 = native.INPUT_MODE_RF24,
        USB = native.INPUT_MODE_USB,
    },
    InputChannel = {
        CHANNEL_1 = native.INPUT_CHANNEL_1,
        CHANNEL_2 = native.INPUT_CHANNEL_2,
        CHANNEL_3 = native.INPUT_CHANNEL_3,
    },
    InputConnectionStatus = {
        SYNCING = native.INPUT_CONNECTION_STATUS_SYNCING,
        PAIRING = native.INPUT_CONNECTION_STATUS_PAIRING,
        CONNECTED = native.INPUT_CONNECTION_STATUS_CONNECTED,
        DISCONNECTED = native.INPUT_CONNECTION_STATUS_DISCONNECTED,
    },
    H264PacketFlag = {
        KEY_FRAME = native.H264_PACKET_KEY_FRAME,
        IDR = native.H264_PACKET_IDR,
        SPS = native.H264_PACKET_SPS,
        PPS = native.H264_PACKET_PPS,
        HEADER = native.H264_PACKET_HEADER,
    },
    KeyCode = native.KEY_CODES,
    KeyOption = {
        DOWN = native.KEY_DOWN,
        UP = native.KEY_UP,
    },
    MouseOption = {
        MOVE = native.MOUSE_MOVE,
        LEFT_DOWN = native.MOUSE_LBUTTON_DOWN,
        LEFT_UP = native.MOUSE_LBUTTON_UP,
        RIGHT_DOWN = native.MOUSE_RBUTTON_DOWN,
        RIGHT_UP = native.MOUSE_RBUTTON_UP,
        MIDDLE_DOWN = native.MOUSE_MBUTTON_DOWN,
        MIDDLE_UP = native.MOUSE_MBUTTON_UP,
        WHEEL = native.MOUSE_WHEEL,
    },
}
