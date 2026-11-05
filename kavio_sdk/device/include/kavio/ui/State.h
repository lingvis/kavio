#pragma once

#include <functional>
#include <utility>
#include <vector>

namespace kavio::ui {

/**
 * Lightweight retained state value. Callbacks run synchronously on the thread
 * that calls Set(); UI state should therefore be changed on the App run thread.
 */
template <typename T>
class State {
public:
    using Observer = std::function<void(const T&)>;

    State() = default;
    explicit State(T value) : value_(std::move(value)) {}

    const T& Get() const noexcept { return value_; }

    void Set(T value)
    {
        value_ = std::move(value);
        for (auto& observer : observers_) {
            if (observer) observer(value_);
        }
    }

    void Observe(Observer observer)
    {
        observers_.push_back(std::move(observer));
    }

private:
    T value_{};
    std::vector<Observer> observers_;
};

}  // namespace kavio::ui
