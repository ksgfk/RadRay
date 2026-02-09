#pragma once

#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

#include <stdexec/coroutine.hpp>
#include <stdexec/execution.hpp>
#include <exec/task.hpp>

#include <radray/types.h>

namespace radray {

using std::coroutine_handle;
using exec::task;

struct FireAndForgetTask {
    struct promise_type : STDEXEC::with_awaitable_senders<promise_type> {
        FireAndForgetTask get_return_object() { return {}; }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { throw; }

        auto get_env() const noexcept {
            return STDEXEC::prop{STDEXEC::get_scheduler, STDEXEC::inline_scheduler{}};
        }
    };
};

}  // namespace radray
