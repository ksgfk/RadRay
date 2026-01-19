#pragma once

#include <exception>
#include <functional>

#include <radray/types.h>
#include <radray/logger.h>

#include <cppcoro/coroutine.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/shared_task.hpp>
#include <cppcoro/when_all.hpp>
#include <cppcoro/async_manual_reset_event.hpp>

namespace radray {

using cppcoro::coroutine_handle;
using cppcoro::suspend_always;
using cppcoro::noop_coroutine;
using cppcoro::suspend_never;

using cppcoro::task;
using cppcoro::shared_task;
using cppcoro::when_all;
using cppcoro::async_manual_reset_event;

class Scheduler {
public:
    Scheduler() = default;
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    virtual ~Scheduler() noexcept;

    template <typename Awaitable>
    void Schedule(Awaitable t) {
        auto session = WrapTask(std::move(t));
        _tasks.emplace_back(session.handle);
    }

    void Tick();

protected:
    struct TaskSession {
        struct promise_type {
            std::exception_ptr _exception;

            TaskSession get_return_object() { return TaskSession{coroutine_handle<promise_type>::from_promise(*this)}; }
            suspend_never initial_suspend() { return {}; }
            suspend_always final_suspend() noexcept { return {}; }
            void return_void() {}
            void unhandled_exception() {
                _exception = std::current_exception();
            }
        };

        coroutine_handle<promise_type> handle;
    };

    template <typename Awaitable>
    TaskSession WrapTask(Awaitable t) {
        co_await t;
    }

    virtual void OnException(std::exception_ptr e);

    list<coroutine_handle<TaskSession::promise_type>> _tasks;
};

}  // namespace radray
