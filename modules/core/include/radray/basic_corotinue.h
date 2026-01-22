#pragma once

#include <exception>
#include <functional>
#include <utility>
#include <vector>

#include <radray/types.h>
#include <radray/logger.h>

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

class TickScheduler {
public:
    class schedule_operation;

    TickScheduler() = default;
    TickScheduler(const TickScheduler&) = delete;
    TickScheduler& operator=(const TickScheduler&) = delete;
    TickScheduler(TickScheduler&&) = delete;
    TickScheduler& operator=(TickScheduler&&) = delete;

    [[nodiscard]] schedule_operation schedule() noexcept;

    void Tick();

private:
    friend class schedule_operation;

    void EnqueueNext(cppcoro::coroutine_handle<> handle) noexcept;

    vector<cppcoro::coroutine_handle<>> _ready;
    vector<cppcoro::coroutine_handle<>> _next;
};

class TickScheduler::schedule_operation {
public:
    explicit schedule_operation(TickScheduler& service) noexcept : m_service(service) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(cppcoro::coroutine_handle<> awaiter) noexcept {
        m_service.EnqueueNext(awaiter);
    }
    void await_resume() const noexcept {}

private:
    TickScheduler& m_service;
};

}  // namespace radray
