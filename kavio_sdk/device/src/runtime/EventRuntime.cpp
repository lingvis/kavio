#include "runtime/EventRuntime.h"

namespace kavio::detail {

bool EventRuntime::PushMessage(const kavio::ipc::proto::IPCPackagePb& message)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxEvents) {
        overflowed_.store(true);
        return false;
    }
    Event event;
    event.type = Event::Type::kMessage;
    event.message.CopyFrom(message);
    queue_.push_back(std::move(event));
    return true;
}

bool EventRuntime::PushDisconnected()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.size() >= kMaxEvents) {
        overflowed_.store(true);
        return false;
    }
    Event event;
    event.type = Event::Type::kDisconnected;
    queue_.push_back(std::move(event));
    return true;
}

std::vector<EventRuntime::Event> EventRuntime::Drain()
{
    std::vector<Event> events;
    std::lock_guard<std::mutex> lock(mutex_);
    events.reserve(queue_.size());
    while (!queue_.empty()) {
        events.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return events;
}

void EventRuntime::Reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
    overflowed_.store(false);
}

}  // namespace kavio::detail
