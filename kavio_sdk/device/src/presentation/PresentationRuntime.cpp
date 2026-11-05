#include "presentation/PresentationRuntime.h"

#include <kavio/KavioClient.h>

#include "presentation/PresentationInternal.h"

namespace kavio::detail {

Status PresentationRuntime::Initialize(KavioClient& client)
{
    return PresentationAccess::Initialize(*presentation_, client);
}

Status PresentationRuntime::HandleMessage(const ipc::proto::IPCPackagePb& message,
                                          KavioClient& client,
                                          bool& consumed,
                                          bool& became_ready)
{
    return PresentationAccess::HandleMessage(*presentation_, message, client, consumed, became_ready);
}

void PresentationRuntime::Shutdown(KavioClient& client) noexcept
{
    PresentationAccess::Shutdown(*presentation_, client);
}

}  // namespace kavio::detail
