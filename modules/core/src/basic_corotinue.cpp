#include <radray/basic_corotinue.h>

namespace radray {

TickScheduler::schedule_operation TickScheduler::schedule() noexcept {
    return schedule_operation{*this};
}

void TickScheduler::Tick() {
    _ready.swap(_next);
    _next.clear();
    for (auto handle : _ready) {
        if (handle) {
            handle.resume();
        }
    }
    _ready.clear();
}

void TickScheduler::EnqueueNext(cppcoro::coroutine_handle<> handle) noexcept {
    if (handle) {
        _next.push_back(handle);
    }
}

}  // namespace radray
