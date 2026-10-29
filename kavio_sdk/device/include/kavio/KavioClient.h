/**
 * @file KavioClient.h
 * @brief Kavio Device SDK：设备会话、异步 IPC 控制、RX/TX 与 H.264 共享内存接口。
 *
 * 业务请求成功返回仅表示本地 IPC 消息已提交；Platform 处理结果由
 * IClientMessageHandler::OnMessage() 异步交付，凭 track_id 关联。
 * SDK 不替业务请求计时。OnMessage 在单独的单线程派发循环中顺序执行，
 * 耗时处理会拖延后续消息派发，但不会直接阻塞 IPC 接收线程。
 */
#pragma once

#include <kavio/Types.h>
#include <kavio/AppViewSurface.h>
#include <json/json.h>
#include <functional>
#include "input.pb.h"
#include "ipc.pb.h"
#include "runtime_data.pb.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kavio {

/** @brief RX 当前状态快照；仅表示当前接收和帧缓冲状态，不表示编码流已启用。 */
struct RxInfo {
    bool connected = false;        ///< Platform Runtime 判断 RX 已连接且处于可运行状态。
    bool frame_available = false;  ///< SDK 已打开 RX SHM，可调用 ReadLatestRxFrame 尝试读取。
    uint32_t width = 0;           ///< RX 宽度（像素）。
    uint32_t height = 0;          ///< RX 高度（像素）。
    uint32_t stride = 0;          ///< RX 一行的字节数。
    PixelFormat pixel_format = PixelFormat::kUnknown; ///< RX 帧的像素格式。
};

inline bool operator==(const RxInfo& lhs, const RxInfo& rhs) {
    return lhs.connected == rhs.connected &&
           lhs.frame_available == rhs.frame_available &&
           lhs.width == rhs.width &&
           lhs.height == rhs.height &&
           lhs.stride == rhs.stride &&
           lhs.pixel_format == rhs.pixel_format;
}

inline bool operator!=(const RxInfo& lhs, const RxInfo& rhs) {
    return !(lhs == rhs);
}

/**
 * @brief 本地 TX 共享内存图层句柄，可移动、不可复制。
 * @note 远端资源由 Platform 持有，销毁需调用 Destroy() 异步提交并观察对应回复。
 */
class TxSurface {
public:
    TxSurface();
    ~TxSurface();

    TxSurface(TxSurface&& other) noexcept;
    TxSurface& operator=(TxSurface&& other) noexcept;

    TxSurface(const TxSurface&) = delete;
    TxSurface& operator=(const TxSurface&) = delete;

    /** @return 图层是否仍持有有效的本地共享内存映射。 */
    bool IsValid() const;

    /** @return 当前图层 ID；没有有效图层时的值不应用于 Platform 操作。 */
    uint32_t id() const;
    /** @return 图层宽度，单位为像素。 */
    uint32_t width() const;
    /** @return 图层高度，单位为像素。 */
    uint32_t height() const;
    /** @return 图像一行占用的字节数。 */
    uint32_t stride() const;
    /** @return 图层像素格式，当前 TX 图层使用 BGRA8888。 */
    PixelFormat pixel_format() const;

    /**
     * @brief 将一帧图像写入 TX 图层。
     * @param[in] frame 包含像素数据及尺寸/格式信息的图像帧。
     * @return OK 或无效尺寸、图像格式、共享内存操作等错误。
     * @note 数据复制到图层缓冲区；不会保留 frame.data 指针。
     */
    Status WriteFrame(const ImageFrame& frame);

    /** @brief 清空当前图层。@return OK 或共享内存/图层状态错误。 */
    Status Clear();

    /**
     * @brief 异步提交 TX 图层销毁请求。
     * @param[out] request_id 本地发送成功后输出的 track_id。
     * @return 本地 IPC 发送状态；OK 不代表 Platform 已完成销毁。
     * @note 收到对应 MSG_P2A_RSP_STATUS 成功时，SDK 自动释放本地图层映射。
     */
    Status Destroy(uint64_t& request_id);

private:
    friend class KavioClient;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};


/** @brief H264Packet::flags 的位标记，可按位组合。 */
enum H264PacketFlag : uint32_t {
    kH264PacketKeyFrame = 1u << 0, ///< 包含关键帧信息。
    kH264PacketIDR      = 1u << 1, ///< 包含 IDR NAL。
    kH264PacketSPS      = 1u << 2, ///< 包含 SPS 参数集。
    kH264PacketPPS      = 1u << 3, ///< 包含 PPS 参数集。
    kH264PacketHeader   = 1u << 4, ///< 编码头部信息。
};

/**
 * @brief 一份独立的 H.264 Annex-B 数据包及元数据。
 * @note 一个 packet 不保证恰好是一帧；可能单独包含 SPS/PPS 等参数集。
 *       data 属于当前对象，不依赖 SHM 的读取生命周期。
 */
struct H264Packet {
    uint64_t seq = 0;          ///< 共享流中单调增加的数据包序号；与帧号不同。
    uint64_t pts_us = 0;       ///< 显示时间戳（微秒），由编码路径提供。
    uint64_t dts_us = 0;       ///< 解码时间戳（微秒），由编码路径提供。
    uint64_t timestamp_ns = 0; ///< 发布时的时间戳（纳秒）。
    uint32_t flags = 0;        ///< H264PacketFlag 位标记。
    uint32_t nal_type = 0;     ///< 数据包第一个 H.264 NAL 的类型；0 表示无法识别。
    std::vector<uint8_t> data; ///< Annex-B 原始字节，包含 NAL 起始码。
};

/**
 * @brief 应用消息处理器；消息直接使用当前公开的 protobuf IPCPackagePb。
 * @note OnMessage 在 SDK 专用派发线程中顺序执行。不得从中销毁正在派发的
 *       KavioClient。处理耗时会延迟下一条消息；不会直接阻塞 IPC 接收线程。
 *       handler 对象必须存活至 KavioClient 完成析构。
 */
class IClientMessageHandler {
public:
    virtual ~IClientMessageHandler() = default;
    virtual void OnMessage(const kavio::ipc::proto::IPCPackagePb& message) = 0;
    /** @brief 本地 IPC 断开事件；不是由 Platform 产生的 protobuf 消息。 */
    virtual void OnDisconnected() {}
};

/** @brief Platform IPC Session 客户端，管理连接、重连和异步消息队列。 */
class KavioClient {
public:
    using RuntimeData = kavio::runtime::proto::RuntimeDataPb;
    using StatsReply = kavio::ipc::proto::P2ARspQueryStatsPb;

    KavioClient();
    ~KavioClient();

    KavioClient(const KavioClient&) = delete;
    KavioClient& operator=(const KavioClient&) = delete;

    /**
     * @brief 启动 IPC 接收及 App 消息派发线程（非阻塞）。
     * @param[in] options 直接使用 A2PReqCreateSessionPb；client_name / client_pid 可留空（SDK 自动识别）；
     *                    rx_params / tx_params 可省略，此时使用 SDK 默认值。
     * @param[in] handler 不转移所有权，须保持到 KavioClient 析构结束。
     * @param[in] auto_query_runtime_on_connect 创建 Session 后是否自动查询 Runtime。
     * @return OK 表示后台启动成功；Session 建立以 MSG_P2A_RSP_CREATE_SESSION 为准。
     */
    Status Start(const kavio::ipc::proto::A2PReqCreateSessionPb& options,
                 IClientMessageHandler& handler,
                 bool auto_query_runtime_on_connect = true);

    /**
     * @brief 请求停止客户端及后台通信（非阻塞）。
     * @note 返回时清理可能仍在进行；析构函数负责等待后台线程退出。
     */
    void Stop();

    /** @brief 查询 Session 是否可用。@return true 表示 IPC 已连接且已创建 Session；不保证 RX 有图像。 */
    bool IsClientReady() const;

    /** @brief 获取最近缓存的 Platform Runtime 快照。@return 最近一次接收的 RuntimeData，可能尚未初始化。 */
    RuntimeData GetRuntimeData() const;
    /** @brief 获取最近缓存的 RX 状态。@return RxInfo 快照；frame_available 表示可尝试读帧。 */
    RxInfo GetRxInfo() const;
    /** @brief 查询 USB Host 是否在线。@return 当前缓存的在线状态。 */
    bool IsUSBHostOnline() const;
    /** @brief 查询当前 App 的 USB 通信 Channel 是否具备双向通信条件。 */
    bool IsCommChannelReady() const;

    /**
     * @brief 异步提交 RX 参数。@param[in] params RxParamsPb，frame_sample_interval 必须 >= 1。
     * @param[out] request_id IPC track_id。@return 本地发送结果。
     * @note 用同一 track_id 的 MSG_P2A_RSP_STATUS / MSG_P2A_RSP_ERROR 判断远端结果。
     */
    Status SetRxParams(const kavio::ipc::proto::RxParamsPb& params, uint64_t& request_id);

    /**
     * @brief 异步提交 TX 参数。@param[in] params TxParamsPb，包含输出开关与 z_order。
     * @param[out] request_id IPC track_id。@return 本地发送结果。
     */
    Status SetTxParams(const kavio::ipc::proto::TxParamsPb& params, uint64_t& request_id);

    /**
     * @brief 获取目前可读取的最新 HDMI RX 原始图像帧（非阻塞）。
     * @param[out] out_frame 包括尺寸、stride、格式及独立复制的像素数据。
     * @return OK；Media_VideoNotReady 表示 RX 尚未就绪；Media_NoFrame 表示暂时无可用帧；
     *         Common_NotConnected 表示客户端不可用，其他值表示具体错误。
     */
    Status ReadLatestRxFrame(ImageFrame& out_frame);

    /**
     * @brief 异步创建 BGRA8888 图层，不自动打开共享内存。
     * @param[in] width 图层宽度（像素，>0）。@param[in] height 图层高度（像素，>0）。
     * @param[out] request_id IPC track_id；MSG_P2A_RSP_CREATE_TX_SHM 表示远端创建成功。
     * @return 本地发送结果。@note App 收到 OnMessage 后按需调用 OpenTxSurface()。
     */
    Status CreateTxSurface(uint32_t width, uint32_t height, uint64_t& request_id);

    /**
     * @brief 根据远端创建回复打开本地图层映射；不发送 IPC。
     * @param[in] info MSG_P2A_RSP_CREATE_TX_SHM.tx_shm。
     * @param[out] surface 成功时得到的可写图层句柄。
     * @return 本地 SHM 打开状态。失败时 App 应异步销毁远端资源。
     */
    Status OpenTxSurface(const kavio::ipc::proto::ShmInfoPb& info, TxSurface& surface);

    /**
     * @brief 根据图层 ID 异步请求销毁远端 TX 资源，适用于 OpenTxSurface 失败后的清理。
     * @param[in] shm_id MSG_P2A_RSP_CREATE_TX_SHM 返回的图层 ID。
     * @param[out] request_id 本地发送成功时输出的 IPC track_id。
     * @return 本地发送状态；结果经 OnMessage 返回。
     */
    Status DestroyTxSurface(uint32_t shm_id, uint64_t& request_id);

    /**
     * @brief 异步创建独立的全屏 AppView BGRA8888 画布，不属于 TX Overlay。
     * @param[out] request_id 本地发送成功后输出的 track_id。
     * @note 收到 MSG_P2A_RSP_CREATE_APP_VIEW_SURFACE 后调用 OpenAppViewSurface。
     */
    Status CreateAppViewSurface(uint64_t& request_id);

    /**
     * @brief 根据 Platform 的创建回复映射 AppView 共享画布，不发送 IPC。
     */
    Status OpenAppViewSurface(
        const kavio::ipc::proto::AppViewSurfaceInfoPb& info,
        AppViewSurface& surface);

    /**
     * @brief 异步请求销毁独立 AppView 画布。
     * @param[out] request_id 本地发送成功后输出的 track_id。
     */
    Status DestroyAppViewSurface(uint32_t surface_id, uint64_t& request_id);

    /**
     * @brief 异步关闭指定 Session。
     * @param[in] session_id 要关闭的 Session ID（非零）。
     * @param[out] request_id IPC track_id。@return 本地发送结果。
     * @note 关闭自身 Session 可能直接导致连接断开，不保证能收到远端回复。
     */
    Status CloseSession(uint32_t session_id, uint64_t& request_id);

    /**
     * @brief 异步查询 Platform 运行统计。
     * @param[out] request_id IPC track_id。@return 本地发送结果。
     * @note MSG_P2A_RSP_QUERY_STATS 携带统计结果。
     */
    Status QueryStats(uint64_t& request_id);

    /**
     * @brief 发送键盘或鼠标输入事件（不等待确认）。
     * @param[in] event 一个完整的输入事件 protobuf。
     * @return 本地 IPC 发送尝试的结果；OK 不保证远端已经处理。
     */
    Status SendInputEvent(const kavio::input::proto::InputEventPb& event);

    /**
     * @brief 查询当前 App 绑定的 USB 通信 Channel 标识。
     * @return ChannelID；0 表示 Session 尚未建立或没有可用标识。
     */
    uint32_t GetCommChannelId() const;

    /**
     * @brief 经当前 App 的通信 Channel 发送一份原始数据报。
     * @param[in] data 数据首地址，不得为 nullptr。
     * @param[in] size 数据大小（字节），范围 1 到 1 MiB。
     * @return 本地提交结果；不等待 Host Component 的业务应答。
     * @note App Channel 需要 Host 在线且对端 Host Component 已建立对应通道。
     */
    Status SendCommChannelData(const void* data, size_t size);
    /** @brief 发送一份原始字节数组。@param[in] data 非空且不超过 1 MiB。@return 发送结果。 */
    Status SendCommChannelData(const std::vector<uint8_t>& data);

    // SDK-managed App configuration. Snapshot is independently owned by the caller.
    Json::Value GetAppConfig() const;
    Status UpdateAppConfig(const Json::Value& value);
    Status ResetAppConfigToDefaults();

    /**
     * @brief 异步订阅公共 H.264 码流；参数全部由 Platform 决定。
     * @param[out] request_id IPC track_id。@return 本地发送结果。
     * @note MSG_P2A_RSP_CREATE_ENCODE_STREAM 表示远端订阅成功；App 收到通知后
     *       调用 OpenH264Stream()，才可通过 ReadH264Packet() 消费数据。
     */
    Status SubscribeH264Stream(uint64_t& request_id);

    /**
     * @brief 打开订阅回复所指定的只读 H.264 SHM；不发送 IPC。
     * @param[in] info MSG_P2A_RSP_CREATE_ENCODE_STREAM.stream。
     * @return 本地 SHM 映射状态。失败后 App 应请求 UnsubscribeH264Stream()。
     */
    Status OpenH264Stream(const kavio::ipc::proto::EncodedStreamShmInfoPb& info);

    /**
     * @brief 异步取消本 Session 的编码流订阅。
     * @param[in] stream_id 公共 H.264 流 ID。
     * @param[out] request_id IPC track_id。@return 本地发送结果。
     * @note 收到 MSG_P2A_RSP_STATUS 成功后 SDK 清理本地编码流映射。
     */
    Status UnsubscribeH264Stream(uint32_t stream_id, uint64_t& request_id);

    /**
     * @brief 查询 H.264 公共流最新数据包的发布序号。
     * @param[in] stream_id 当前客户端订阅的流 ID。
     * @return 最新包序号；0 表示当前没有可用的有效订阅。
     * @note 序号不是视频帧号；查询时不复制数据。缓冲区可能已覆盖较早的数据包。
     */
    uint64_t GetLatestH264PacketSeq(uint32_t stream_id) const;

    /**
     * @brief 根据序号从 SHM 读取并复制一个 Annex-B H.264 数据包（非阻塞）。
     * @param[in] stream_id 当前客户端订阅的流 ID。
     * @param[in] seq 要读取的数据包序号（必须大于 0）。
     * @param[out] packet 成功时返回独立复制的元数据和 data 字节缓冲。
     * @return OK；Media_NoFrame 表示包尚未就绪、读取过程中正在更新或已被覆盖；
     *         其他 Status 表示连接状态、SHM 或参数错误。
     * @note SHM 容量有限，慢消费者需要容忍覆盖丢包；新订阅或发生丢包后，
     *       应等待 SPS/PPS 与后续 IDR 再开始解码。此函数不会等待未来数据到达。
     */
    Status ReadH264Packet(uint32_t stream_id, uint64_t seq, H264Packet& packet) const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace kavio
