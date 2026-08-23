#pragma once

#include <kavio/Types.h>
#include "input.pb.h"
#include "ipc.pb.h"
#include "runtime_data.pb.h"
#include "user_config.pb.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kavio {

// 对 App 不暴露内部状态枚举。
// App 只通过 on_client_ready / on_client_invalid 感知生命周期，
// 并通过 IsClientReady() 查询当前 client 是否可用。

struct RxInfo {
    bool connected = false;         // runtime 层面 RX 是否连接/运行
    bool frame_available = false;   // SDK 是否已打开有效 RX SHM，可尝试读帧

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    PixelFormat pixel_format = PixelFormat::kUnknown;
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

class TxSurface {
public:
    TxSurface();
    ~TxSurface();

    TxSurface(TxSurface&& other) noexcept;
    TxSurface& operator=(TxSurface&& other) noexcept;

    TxSurface(const TxSurface&) = delete;
    TxSurface& operator=(const TxSurface&) = delete;

    bool IsValid() const;

    uint32_t id() const;
    uint32_t width() const;
    uint32_t height() const;
    uint32_t stride() const;
    PixelFormat pixel_format() const;

    Status WriteFrame(const ImageFrame& frame);
    Status Clear();

    // 阻塞等待 platform ACK。
    // 析构函数不执行阻塞销毁；调用方需要确认 platform 已释放 TX SHM 时，必须显式调用 Destroy()。
    Status Destroy(std::chrono::milliseconds timeout);

private:
    friend class KavioClient;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};


class KavioClient {
public:
    using RuntimeData = kavio::runtime::proto::RuntimeDataPb;
    using UserConfig = kavio::config::proto::UserConfigPb;
    using StatsReply = kavio::ipc::proto::NtfStatsReplyPb;
    using AppConfigRequest = kavio::ipc::proto::NtfAppConfigRequestPb;
    using AppConfigResponse = kavio::ipc::proto::NtfAppConfigResponsePb;

    struct Callbacks {
        // IPC 连接成功且 session 创建成功后触发。
        // 每次断线重连并重新创建 session 成功后，都会再次触发。
        std::function<void()> on_client_ready;

        // IPC 断开、session 丢失、Stop 或不可恢复错误时触发。
        // 进入 invalid 后，App 必须认为旧 TX Surface、旧 RX 帧、旧 session 全部失效。
        std::function<void()> on_client_invalid;

        // runtime cache 更新后触发。SDK 内部会保存最新 runtime，App 也可以主动 GetRuntimeData() 查询。
        std::function<void(const RuntimeData& runtime_data)> on_runtime_changed;

        // user config cache 更新后触发。SDK 内部会保存最新 user config，App 也可以主动 GetUserConfig() 查询。
        std::function<void(const UserConfig& user_config)> on_user_config_changed;

        // RX 运行状态或 frame_available 变化后触发。
        // 每帧实际宽高、stride 和 pixel_format 以 ReadLatestRxFrame 返回的 ImageFrame 为准。
        std::function<void(const RxInfo& prev, const RxInfo& next)> on_rx_changed;

        // USB Host 在线状态变化。reason 为 Platform 提供的诊断信息。
        std::function<void(bool online, const std::string& reason)> on_usb_host_online_changed;

        // 当前 Kavio App 的通信 Channel 是否具备双向通信条件（USB Host 与对端 Host Component 均在线）。
        std::function<void(bool ready)> on_comm_channel_ready_changed;

        // 收到 Host Component（主机组件）通过自身 Channel 发来的一条完整原始数据报。
        std::function<void(const std::vector<uint8_t>& payload)> on_comm_channel_data;

        // 收到由 Windows 端通过 SystemChannel 发起的 App 配置读取/保存请求。
        // App 必须使用相同 request_id 和 operation 调用 SendAppConfigResponse。
        std::function<void(const AppConfigRequest& request)> on_app_config_request;

    };

    KavioClient();
    ~KavioClient();

    KavioClient(const KavioClient&) = delete;
    KavioClient& operator=(const KavioClient&) = delete;

    // 非阻塞。
    // 返回 kOk 只表示后台启动流程已提交，不表示 IPC 已连接或 session 已创建。
    // App 应等待 on_client_ready() 后再创建 TX 或发送 session 级命令。
    Status Start(const SessionOptions& options, Callbacks callbacks = {});

    // 非阻塞。
    // 返回后后台线程继续退出和清理；Client 会立即变为不可用。
    // 析构函数会阻塞等待后台线程退出。
    void Stop();

    // 返回 true 表示 IPC 已连接且 session 已创建成功。
    // 返回 false 表示当前 client 不可用。
    // 注意：true 不代表 RX 图像一定可读。
    bool IsClientReady() const;

    RuntimeData GetRuntimeData() const;
    UserConfig GetUserConfig() const;
    RxInfo GetRxInfo() const;
    bool IsUSBHostOnline() const;
    bool IsCommChannelReady() const;

    // 阻塞等待 platform ACK。RX 参数按完整值设置。
    // enabled=true 时确保 RX SHM 已创建；enabled=false 时停止写入并删除 RX SHM。
    Status SetRxParams(const RxParams& params,
                       std::chrono::milliseconds timeout);

    // 阻塞等待 platform ACK。TX 参数按完整值设置。
    Status SetTxParams(const TxParams& params,
                       std::chrono::milliseconds timeout);

    // 非阻塞读取当前最新 RX 帧。该接口会拷贝图像数据到 out_frame.data。
    // RX 未 ready 返回 StatusCode::Media_VideoNotReady；暂时无帧返回 StatusCode::Media_NoFrame；client 不可用返回 StatusCode::Common_NotConnected。
    Status ReadLatestRxFrame(ImageFrame& out_frame);

    // 阻塞等待 platform 创建 TX SHM，并等待 SDK open 成功。
    // 返回 kOk 后 out_surface 可直接 WriteFrame/Clear。
    Status CreateTxSurface(uint32_t width,
                           uint32_t height,
                           TxSurface& out_surface,
                           std::chrono::milliseconds timeout);

    // 阻塞等待 platform ACK。
    // 可用于关闭任意 session；若关闭的是当前 client 自己绑定的 session，连接随后可能失效。
    Status CloseSession(uint32_t session_id,
                        std::chrono::milliseconds timeout);

    // 阻塞等待 stats reply。
    Status QueryStats(StatsReply& out_stats,
                      std::chrono::milliseconds timeout);

    // 非阻塞输入事件发送。该接口只保证尝试写入 IPC，不等待 ACK。
    Status SendInputEvent(const kavio::input::proto::InputEventPb& event);

    // 通过当前 Kavio App 绑定的唯一通信 Channel 向 Host Component（主机组件）发送一条非空完整原始数据报，大小为 1 字节至 1 MiB。
    // Channel 身份由 Platform 根据 SO_PEERCRED 的真实进程 PID 校验。
    // 返回当前 App 由 Platform 绑定的稳定 ChannelID。0 表示 session 尚未建立。
    uint32_t GetCommChannelId() const;

    Status SendCommChannelData(const void* data, size_t size);
    Status SendCommChannelData(const std::vector<uint8_t>& data);

    // 返回 SystemChannel App 配置请求的处理结果。该消息通过 Device Component IPC 发送，
    // 不依赖 Host Component 业务 Channel 是否注册或可用。
    Status SendAppConfigResponse(const AppConfigResponse& response);


private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace kavio
