#include <radray/runtime/flight_completion.h>

namespace radray {

void FlightCompletionQueue::Push(FlightCompletion completion) {
    std::lock_guard lock(_mutex);
    _pending.push_back(completion);
}

FlightCompletionQueue::Drain::Drain(FlightCompletionQueue& queue) : _queue(queue) {
    std::lock_guard lock(queue._mutex);
    if (queue._draining) {
        return;
    }
    queue._draining = true;
    _active = true;
    _items.swap(queue._pending);
}

FlightCompletionQueue::Drain::~Drain() noexcept {
    if (!_active) {
        return;
    }
    std::lock_guard lock(_queue._mutex);
    _queue._draining = false;
}

std::span<const FlightCompletion> FlightCompletionQueue::Drain::Items() const noexcept {
    return _items;
}

}  // namespace radray
