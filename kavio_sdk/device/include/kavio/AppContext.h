#pragma once

/** Shared application context providing access to current device and presentation. */

#include <kavio/AppApi.h>

namespace kavio {
namespace detail {
class AppRuntime;
}

class Presentation;
class Device;

/** Services owned by the running KavioApp. */
class KAVIO_APP_API AppContext {
public:
    Presentation& presentation() noexcept;
    const Presentation& presentation() const noexcept;
    Device& device() noexcept;
    const Device& device() const noexcept;

private:
    friend class detail::AppRuntime;
    explicit AppContext(Presentation& presentation, Device& device) noexcept;
    Presentation* presentation_ = nullptr;
    Device* device_ = nullptr;
};

}  // namespace kavio
