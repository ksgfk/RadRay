#include <radray/coroutine.h>

namespace radray {

void ManualCoroutineRecord::ResumeContinuation(std::coroutine_handle<> continuation) noexcept {
    continuation.resume();
}

TaskScope::~TaskScope() noexcept {
    RequestStop();
    WaitUntilEmpty();
}

void TaskScope::Spawn(task<void> t) {
    _scope.spawn(std::move(t));
}

bool TaskScope::RequestStop() noexcept {
    return _scope.request_stop();
}

task<void> TaskScope::Join() {
    co_await _scope.on_empty();
}

void TaskScope::WaitUntilEmpty() noexcept {
    (void)stdexec::sync_wait(_scope.on_empty());
}

stop_token TaskScope::GetStopToken() const noexcept {
    return _scope.get_stop_token();
}

task<stop_token> CurrentStopToken() {
    co_return co_await stdexec::get_stop_token();
}

task<void> StopCurrentTask() {
    co_await stdexec::just_stopped();
}

}  // namespace radray
