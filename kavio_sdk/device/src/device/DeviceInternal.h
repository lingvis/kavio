#pragma once

#include <kavio/Status.h>

namespace kavio {
class Device;
class KavioClient;
namespace ipc { namespace proto { class IPCPackagePb; } }

namespace detail {

/** Runtime-only bridge to Device private state. Never exposed by the public SDK. */
class DeviceAccess final {
public:
    static void Bind(Device& device, KavioClient& client) noexcept;
    static void Unbind(Device& device) noexcept;
    static void HandleMessage(Device& device, const ipc::proto::IPCPackagePb& message);
};

}  // namespace detail
}  // namespace kavio
