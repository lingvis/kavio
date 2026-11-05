#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

#include "ipc.pb.h"

namespace kavio::detail {

class EventRuntime {
public:
    struct Event {
        enum class Type {
            kMessage,
            kDisconnected,
        } type = Type::kMessage;
        kavio::ipc::proto::IPCPackagePb message;
    };

    bool PushMessage(const kavio::ipc::proto::IPCPackagePb& message);
    bool PushDisconnected();
    std::vector<Event> Drain();
    void Reset();
    bool Overflowed() const noexcept { return overflowed_.load(); }

private:
    static constexpr size_t kMaxEvents = 2048;
    mutable std::mutex mutex_;
    std::deque<Event> queue_;
    std::atomic<bool> overflowed_{false};
};

}  // namespace kavio::detail
