#include <radray/basic_corotinue.h>

namespace radray {

Scheduler::~Scheduler() noexcept {
    for (auto h : _tasks) {
        if (h) h.destroy();
    }
}

void Scheduler::Tick() {
    auto it = _tasks.begin();
    while (it != _tasks.end()) {
        if (it->done()) {
            if (auto ptr = it->promise()._exception) {
                OnException(ptr);
            }
            it->destroy();
            it = _tasks.erase(it);
        } else {
            ++it;
        }
    }
}

void Scheduler::OnException(std::exception_ptr e) {
    try {
        std::rethrow_exception(e);
    } catch (std::exception& ex) {
        RADRAY_ERR_LOG("[radray::Scheduler] Unhandled exception: {}", ex.what());
    } catch (...) {
        RADRAY_ERR_LOG("[radray::Scheduler] Unhandled unknown exception.");
    }
}

}  // namespace radray
