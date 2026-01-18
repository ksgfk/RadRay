#pragma once

#include <cppcoro/coroutine.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/when_all.hpp>

namespace radray {

using cppcoro::coroutine_handle;
using cppcoro::suspend_always;
using cppcoro::noop_coroutine;
using cppcoro::suspend_never;

using cppcoro::task;
using cppcoro::when_all;

}  // namespace radray
