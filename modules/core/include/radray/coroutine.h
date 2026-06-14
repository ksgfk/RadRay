#pragma once

// Coroutine support for RadRay, layered on stdexec (P2300 sender/receiver).
//
// Business code should use the radray aliases and helpers from this file
// instead of depending on exec::task / stdexec::* directly. That keeps the
// stdexec dependency behind a small facade.

#include <optional>
#include <utility>

#include <exec/async_scope.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <radray/types.h>

namespace radray {

template <class T>
using task = exec::task<T>;

using stop_source = stdexec::inplace_stop_source;
using stop_token = stdexec::inplace_stop_token;

class TaskScope {
public:
    TaskScope() noexcept = default;
    TaskScope(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;

    ~TaskScope() noexcept {
        RequestStop();
        WaitUntilEmpty();
    }

    void Spawn(task<void> t) {
        _scope.spawn(std::move(t));
    }

    bool RequestStop() noexcept {
        return _scope.request_stop();
    }

    task<void> Join() {
        co_await _scope.on_empty();
    }

    void WaitUntilEmpty() noexcept {
        (void)stdexec::sync_wait(_scope.on_empty());
    }

    stop_token GetStopToken() const noexcept {
        return _scope.get_stop_token();
    }

private:
    exec::async_scope _scope;
};

inline task<stop_token> CurrentStopToken() {
    co_return co_await stdexec::get_stop_token();
}

inline task<void> StopCurrentTask() {
    co_await stdexec::just_stopped();
}

template <class T>
task<std::optional<T>> AwaitWithStopToken(task<T> t, stop_token stop) {
    co_return co_await stdexec::stopped_as_optional(
        stdexec::write_env(
            std::move(t),
            stdexec::prop{stdexec::get_stop_token, stop}));
}

}  // namespace radray
