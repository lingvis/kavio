#pragma once

#include <kavio/Status.h>

namespace kavio {
class KavioClient;
class Presentation;
namespace ipc { namespace proto { class IPCPackagePb; } }

namespace detail {

/** Runtime-only bridge to Presentation private state. */
class PresentationAccess final {
public:
    static Status Initialize(Presentation& presentation, KavioClient& client);
    static Status HandleMessage(Presentation& presentation,
                                const ipc::proto::IPCPackagePb& message,
                                KavioClient& client,
                                bool& consumed,
                                bool& became_ready);
    static void Shutdown(Presentation& presentation, KavioClient& client) noexcept;
};

}  // namespace detail
}  // namespace kavio
