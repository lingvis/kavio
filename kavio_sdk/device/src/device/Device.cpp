#include <kavio/Device.h>

#include <kavio/KavioClient.h>

#include "device/DeviceInternal.h"

#include "input.pb.h"
#include "ipc.pb.h"
#include "runtime_data.pb.h"

#include <json/json.h>

#include <utility>

namespace kavio {

struct DeviceTxSurface::Impl {
    TxSurface surface;
};

DeviceTxSurface::DeviceTxSurface() : impl_(std::make_unique<Impl>()) {}
DeviceTxSurface::~DeviceTxSurface() = default;
DeviceTxSurface::DeviceTxSurface(DeviceTxSurface&& other) noexcept = default;
DeviceTxSurface& DeviceTxSurface::operator=(DeviceTxSurface&& other) noexcept = default;

bool DeviceTxSurface::IsValid() const { return impl_ && impl_->surface.IsValid(); }
uint32_t DeviceTxSurface::id() const { return impl_ ? impl_->surface.id() : 0u; }
uint32_t DeviceTxSurface::width() const { return impl_ ? impl_->surface.width() : 0u; }
uint32_t DeviceTxSurface::height() const { return impl_ ? impl_->surface.height() : 0u; }
uint32_t DeviceTxSurface::stride() const { return impl_ ? impl_->surface.stride() : 0u; }
PixelFormat DeviceTxSurface::pixel_format() const {
    return impl_ ? impl_->surface.pixel_format() : PixelFormat::kUnknown;
}
Status DeviceTxSurface::WriteFrame(const ImageFrame& frame) {
    return impl_ ? impl_->surface.WriteFrame(frame)
                 : Status::Error(StatusCode::Common_InvalidState, "TX Surface is not initialized");
}
Status DeviceTxSurface::Clear() {
    return impl_ ? impl_->surface.Clear()
                 : Status::Error(StatusCode::Common_InvalidState, "TX Surface is not initialized");
}
Status DeviceTxSurface::Destroy(uint64_t& request_id) {
    return impl_ ? impl_->surface.Destroy(request_id)
                 : Status::Error(StatusCode::Common_InvalidState, "TX Surface is not initialized");
}

struct Device::Impl {
    KavioClient* client = nullptr;
    CommDataHandler comm_data_handler;
    TxSurfaceHandler tx_surface_handler;
    H264StreamHandler h264_stream_handler;
    H264InvalidatedHandler h264_invalidated_handler;
    ErrorHandler error_handler;
    ConfigChangedHandler config_changed_handler;
};

Device::Device() : impl_(std::make_unique<Impl>()) {}
Device::~Device() = default;

namespace {
Status NotBound()
{
    return Status::Error(StatusCode::Common_InvalidState,
                         "Kavio Device is not bound to a running App");
}

PixelFormat ToPixelFormat(runtime::proto::RuntimePixelFormatPb value)
{
    switch (value) {
    case runtime::proto::RUNTIME_PIXEL_FORMAT_UYVY: return PixelFormat::kUYVY;
    case runtime::proto::RUNTIME_PIXEL_FORMAT_YUYV: return PixelFormat::kYUYV;
    case runtime::proto::RUNTIME_PIXEL_FORMAT_BGRA8888: return PixelFormat::kBGRA8888;
    default: return PixelFormat::kUnknown;
    }
}

DeviceRuntimeVideoState ToRuntimeVideoState(const runtime::proto::RuntimeVideoChannelStatePb& value)
{
    DeviceRuntimeVideoState out;
    out.status = static_cast<RuntimeVideoStatus>(value.status());
    out.width = value.width();
    out.height = value.height();
    out.fps = value.fps();
    out.pixel_format = ToPixelFormat(value.pixel_format());
    return out;
}

bool ValidKeyOption(KeyOption option)
{
    return option == KeyOption::kDown || option == KeyOption::kUp;
}

bool ValidMouseOption(MouseOption option)
{
    const auto value = static_cast<int32_t>(option);
    return value >= static_cast<int32_t>(MouseOption::kMove) &&
           value <= static_cast<int32_t>(MouseOption::kWheel);
}
}  // namespace

void detail::DeviceAccess::Bind(Device& device, KavioClient& client) noexcept
{
    device.impl_->client = &client;
}

void detail::DeviceAccess::Unbind(Device& device) noexcept
{
    device.impl_->client = nullptr;
}

bool Device::IsReady() const { return impl_->client && impl_->client->IsClientReady(); }

Device::RuntimeData Device::GetRuntimeData() const
{
    RuntimeData out;
    if (!impl_->client) return out;
    const auto source = impl_->client->GetRuntimeData();
    out.rx = ToRuntimeVideoState(source.rx_info());
    out.tx = ToRuntimeVideoState(source.tx_info());
    out.input.mode = static_cast<InputMode>(source.input().mode());
    out.input.channel = static_cast<InputChannel>(source.input().channel());
    out.input.connection_status = static_cast<InputConnectionStatus>(source.input().connection_status());
    return out;
}

Device::RxInfo Device::GetRxInfo() const
{
    RxInfo out;
    if (!impl_->client) return out;
    const auto source = impl_->client->GetRxInfo();
    out.connected = source.connected;
    out.frame_available = source.frame_available;
    out.width = source.width;
    out.height = source.height;
    out.stride = source.stride;
    out.pixel_format = source.pixel_format;
    return out;
}

bool Device::IsUSBHostOnline() const { return impl_->client && impl_->client->IsUSBHostOnline(); }
bool Device::IsCommChannelReady() const { return impl_->client && impl_->client->IsCommChannelReady(); }
uint32_t Device::GetCommChannelId() const { return impl_->client ? impl_->client->GetCommChannelId() : 0; }

Status Device::SetRxParams(bool enabled, uint32_t frame_sample_interval, uint64_t& request_id)
{
    if (!impl_->client) return NotBound();
    ipc::proto::RxParamsPb params;
    params.set_enabled(enabled);
    params.set_frame_sample_interval(frame_sample_interval == 0 ? 1u : frame_sample_interval);
    return impl_->client->SetRxParams(params, request_id);
}

Status Device::SetTxParams(bool enabled, int32_t z_order, uint64_t& request_id)
{
    if (!impl_->client) return NotBound();
    ipc::proto::TxParamsPb params;
    params.set_enabled(enabled);
    params.set_z_order(z_order);
    return impl_->client->SetTxParams(params, request_id);
}

Status Device::ReadLatestRxFrame(ImageFrame& out_frame)
{ return impl_->client ? impl_->client->ReadLatestRxFrame(out_frame) : NotBound(); }
Status Device::CreateTxSurface(uint32_t width, uint32_t height, uint64_t& request_id)
{ return impl_->client ? impl_->client->CreateTxSurface(width, height, request_id) : NotBound(); }
Status Device::DestroyTxSurface(uint32_t surface_id, uint64_t& request_id)
{ return impl_->client ? impl_->client->DestroyTxSurface(surface_id, request_id) : NotBound(); }

Status Device::SendKeyboard(KeyOption option, KeyCode key_code, uint64_t timestamp_ns)
{
    if (!impl_->client) return NotBound();
    if (!ValidKeyOption(option)) {
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid keyboard option");
    }
    input::proto::InputEventPb event;
    event.set_timestamp_ns(timestamp_ns);
    auto* keyboard = event.mutable_keyboard();
    keyboard->set_key_option(static_cast<input::proto::KeyboardEventPb::KeyOptionPb>(option));
    keyboard->set_key_code(KeyCodeValue(key_code));
    return impl_->client->SendInputEvent(event);
}

Status Device::SendMouse(MouseOption option, int32_t x, int32_t y, int32_t wheel, uint64_t timestamp_ns)
{
    if (!impl_->client) return NotBound();
    if (!ValidMouseOption(option)) {
        return Status::Error(StatusCode::Common_InvalidArgument, "invalid mouse option");
    }
    input::proto::InputEventPb event;
    event.set_timestamp_ns(timestamp_ns);
    auto* mouse = event.mutable_mouse();
    mouse->set_mouse_option(static_cast<input::proto::MouseEventPb::MouseOptionPb>(option));
    mouse->set_x(x);
    mouse->set_y(y);
    mouse->set_wheel(wheel);
    return impl_->client->SendInputEvent(event);
}

Status Device::SendCommChannelData(const void* data, size_t size)
{ return impl_->client ? impl_->client->SendCommChannelData(data, size) : NotBound(); }
Status Device::SendCommChannelData(const std::vector<uint8_t>& data)
{ return impl_->client ? impl_->client->SendCommChannelData(data) : NotBound(); }

std::string Device::GetAppConfigJson() const
{
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const Json::Value value = impl_->client
        ? impl_->client->GetAppConfig()
        : Json::Value(Json::objectValue);
    return Json::writeString(builder, value);
}

Status Device::UpdateAppConfigJson(const std::string& json)
{
    if (!impl_->client) return NotBound();
    Json::Value value;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(json.data(), json.data() + json.size(), &value, &errors)) {
        return Status::Error(StatusCode::Common_InvalidArgument, errors);
    }
    return impl_->client->UpdateAppConfig(value);
}

Status Device::ResetAppConfigToDefaults()
{ return impl_->client ? impl_->client->ResetAppConfigToDefaults() : NotBound(); }
Status Device::SubscribeH264Stream(uint64_t& request_id)
{ return impl_->client ? impl_->client->SubscribeH264Stream(request_id) : NotBound(); }
Status Device::UnsubscribeH264Stream(uint32_t stream_id, uint64_t& request_id)
{ return impl_->client ? impl_->client->UnsubscribeH264Stream(stream_id, request_id) : NotBound(); }
uint64_t Device::GetLatestH264PacketSeq(uint32_t stream_id) const
{ return impl_->client ? impl_->client->GetLatestH264PacketSeq(stream_id) : 0; }

Status Device::ReadH264Packet(uint32_t stream_id, uint64_t seq, H264Packet& packet) const
{
    if (!impl_->client) return NotBound();
    kavio::H264Packet low_level;
    const Status status = impl_->client->ReadH264Packet(stream_id, seq, low_level);
    if (!status.ok()) return status;
    packet.seq = low_level.seq;
    packet.pts_us = low_level.pts_us;
    packet.dts_us = low_level.dts_us;
    packet.timestamp_ns = low_level.timestamp_ns;
    packet.flags = low_level.flags;
    packet.nal_type = low_level.nal_type;
    packet.data = std::move(low_level.data);
    return Status::Ok();
}

void Device::SetCommDataHandler(CommDataHandler handler) { impl_->comm_data_handler = std::move(handler); }
void Device::SetTxSurfaceHandler(TxSurfaceHandler handler) { impl_->tx_surface_handler = std::move(handler); }
void Device::SetH264StreamHandler(H264StreamHandler handler) { impl_->h264_stream_handler = std::move(handler); }
void Device::SetH264InvalidatedHandler(H264InvalidatedHandler handler) { impl_->h264_invalidated_handler = std::move(handler); }
void Device::SetErrorHandler(ErrorHandler handler) { impl_->error_handler = std::move(handler); }
void Device::SetConfigChangedHandler(ConfigChangedHandler handler) { impl_->config_changed_handler = std::move(handler); }

void detail::DeviceAccess::HandleMessage(Device& device, const ipc::proto::IPCPackagePb& message)
{
    Device::Impl* impl = device.impl_.get();
    if (!impl || !impl->client) return;
    const auto type = message.header().msg_type();

    switch (type) {
    case ipc::proto::MSG_P2A_DATA_COMM_CHANNEL:
        if (message.payload().has_p2a_data_comm_channel() && impl->comm_data_handler) {
            const auto& body = message.payload().p2a_data_comm_channel();
            const auto& bytes = body.payload();
            impl->comm_data_handler(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        }
        break;

    case ipc::proto::MSG_P2A_RSP_CREATE_TX_SHM:
        if (message.payload().has_p2a_rsp_create_tx_shm()) {
            kavio::TxSurface low_level_surface;
            const auto& info = message.payload().p2a_rsp_create_tx_shm().tx_shm();
            Status status = impl->client->OpenTxSurface(info, low_level_surface);
            if (!impl->tx_surface_handler) {
                uint64_t ignored = 0;
                (void)impl->client->DestroyTxSurface(info.id(), ignored);
                return;
            }
            if (!status.ok()) {
                uint64_t ignored = 0;
                (void)impl->client->DestroyTxSurface(info.id(), ignored);
            }
            DeviceTxSurface surface;
            if (status.ok()) surface.impl_->surface = std::move(low_level_surface);
            impl->tx_surface_handler(message.header().track_id(), status, std::move(surface));
        }
        break;

    case ipc::proto::MSG_P2A_RSP_CREATE_ENCODE_STREAM:
        if (message.payload().has_p2a_rsp_create_encode_stream()) {
            const auto& info = message.payload().p2a_rsp_create_encode_stream().stream();
            Status status = impl->client->OpenH264Stream(info);
            DeviceH264StreamInfo out;
            out.stream_id = info.stream_id();
            out.width = info.width();
            out.height = info.height();
            out.fps = info.fps();
            out.bitrate_kbps = info.bitrate_kbps();
            out.gop = info.gop();
            out.packet_capacity = info.packet_capacity();
            out.payload_capacity = info.payload_capacity();
            if (!impl->h264_stream_handler) {
                uint64_t ignored = 0;
                (void)impl->client->UnsubscribeH264Stream(info.stream_id(), ignored);
                return;
            }
            if (!status.ok()) {
                uint64_t ignored = 0;
                (void)impl->client->UnsubscribeH264Stream(info.stream_id(), ignored);
            }
            impl->h264_stream_handler(message.header().track_id(), status, out);
        }
        break;

    case ipc::proto::MSG_P2A_NTF_ENCODE_STREAM_INVALIDATED:
        if (message.payload().has_p2a_ntf_encode_stream_invalidated() && impl->h264_invalidated_handler) {
            const auto& body = message.payload().p2a_ntf_encode_stream_invalidated();
            impl->h264_invalidated_handler(DeviceH264InvalidatedEvent{
                body.stream_id(), static_cast<int32_t>(body.reason())});
        }
        break;

    case ipc::proto::MSG_P2A_RSP_ERROR:
        if (message.payload().has_p2a_rsp_error() && impl->error_handler) {
            const auto& body = message.payload().p2a_rsp_error();
            impl->error_handler(DeviceErrorEvent{
                body.track_id() != 0 ? body.track_id() : message.header().track_id(),
                static_cast<int32_t>(body.code()), body.message()});
        }
        break;

    case ipc::proto::MSG_P2A_REQ_SET_APP_CONFIG:
        if (impl->config_changed_handler) impl->config_changed_handler();
        break;

    default:
        break;
    }
}

}  // namespace kavio
