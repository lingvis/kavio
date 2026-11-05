#pragma once

#include <memory>
#include <thread>

#include <kavio/AppViewSurface.h>
#include <kavio/presentation/FrameBuffer.h>

namespace kavio {

struct FrameLease::Impl {
    ~Impl()
    {
        if (active && surface && owner_thread == std::this_thread::get_id()) {
            (void)surface->CancelWrite();
        }
        active = false;
    }

    std::shared_ptr<AppViewSurface> surface;
    FrameBufferInfo info;
    bool active = false;
    std::thread::id owner_thread;
};

}  // namespace kavio
