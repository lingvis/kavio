#include <kavio/AppContext.h>

#include <kavio/presentation/Presentation.h>
#include <kavio/Device.h>

namespace kavio {

AppContext::AppContext(Presentation& presentation, Device& device) noexcept
    : presentation_(&presentation), device_(&device)
{
}

Presentation& AppContext::presentation() noexcept
{
    return *presentation_;
}

const Presentation& AppContext::presentation() const noexcept
{
    return *presentation_;
}

Device& AppContext::device() noexcept
{
    return *device_;
}

const Device& AppContext::device() const noexcept
{
    return *device_;
}

}  // namespace kavio
