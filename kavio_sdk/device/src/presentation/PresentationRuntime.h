#pragma once

#include <kavio/Status.h>
#include <kavio/presentation/Presentation.h>

namespace kavio {
class KavioClient;
namespace ipc { namespace proto { class IPCPackagePb; } }
}

namespace kavio::detail {

/** Internal owner of Presentation lifecycle operations used by AppRuntime. */
class PresentationRuntime {
public:
    explicit PresentationRuntime(Presentation& presentation) noexcept
        : presentation_(&presentation) {}

    Status Initialize(KavioClient& client);
    Status HandleMessage(const ipc::proto::IPCPackagePb& message,
                         KavioClient& client,
                         bool& consumed,
                         bool& became_ready);
    void Shutdown(KavioClient& client) noexcept;

private:
    Presentation* presentation_ = nullptr;
};

}  // namespace kavio::detail
