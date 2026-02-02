#pragma once

#include <coroutine>

#include <cppcoro/coroutine.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/shared_task.hpp>
#include <cppcoro/when_all.hpp>
#include <cppcoro/async_manual_reset_event.hpp>
#include <cppcoro/is_awaitable.hpp>

namespace radray {

using cppcoro::coroutine_handle;
using cppcoro::suspend_always;
using cppcoro::noop_coroutine;
using cppcoro::suspend_never;

using cppcoro::task;
using cppcoro::shared_task;
using cppcoro::when_all;
using cppcoro::async_manual_reset_event;
using cppcoro::is_awaitable;
using cppcoro::is_awaitable_v;

struct FireAndForgetTask {
    struct promise_type {
        FireAndForgetTask get_return_object() { return {}; }
        suspend_never initial_suspend() { return {}; }
        suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { throw; }
    };
};

}  // namespace radray
