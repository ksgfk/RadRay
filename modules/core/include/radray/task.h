#pragma once

#include <coroutine>
#include <exception>
#include <utility>
#include <new>
#include <concepts>

namespace radray {

template <typename T = void>
class Task;

namespace detail {

struct TaskPromiseBase {
    std::coroutine_handle<> Continuation;
    std::exception_ptr Exception;

    struct FinalAwaitable {
        bool await_ready() const noexcept { return false; }

        template <typename P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> coroutine) const noexcept {
            auto& promise = coroutine.promise();
            if (promise.Continuation) {
                return promise.Continuation;
            } else {
                return std::noop_coroutine();
            }
        }

        void await_resume() const noexcept {}
    };
};

template <typename T>
struct TaskPromise : public TaskPromiseBase {
    union {
        T Val;
    };
    bool HasValue = false;

    TaskPromise() noexcept {}
    ~TaskPromise() {
        if (HasValue) {
            Val.~T();
        }
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaitable final_suspend() noexcept { return {}; }

    Task<T> get_return_object() noexcept;

    void unhandled_exception() noexcept {
        Exception = std::current_exception();
    }

    template <typename U>
    requires std::convertible_to<U, T>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>) {
        new (&Val) T(std::forward<U>(value));
        HasValue = true;
    }
};

template <>
struct TaskPromise<void> : public TaskPromiseBase {
    std::suspend_always initial_suspend() noexcept { return {}; }
    FinalAwaitable final_suspend() noexcept { return {}; }

    Task<void> get_return_object() noexcept;

    void unhandled_exception() noexcept {
        Exception = std::current_exception();
    }

    void return_void() noexcept {}
};

}  // namespace detail

template <typename T>
class [[nodiscard]] Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using handle_type = std::coroutine_handle<promise_type>;

    Task() noexcept : _coroutine(nullptr) {}
    explicit Task(handle_type handle) noexcept : _coroutine(handle) {}

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    Task(Task&& other) noexcept : _coroutine(std::exchange(other._coroutine, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            if (_coroutine) _coroutine.destroy();
            _coroutine = std::exchange(other._coroutine, nullptr);
        }
        return *this;
    }

    ~Task() {
        if (_coroutine) _coroutine.destroy();
    }

    bool IsReady() const noexcept {
        return !_coroutine || _coroutine.done();
    }

    bool await_ready() const noexcept {
        return !_coroutine || _coroutine.done();
    }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> continuation) noexcept {
        _coroutine.promise().Continuation = continuation;
        return _coroutine;
    }

    T await_resume() {
        if (_coroutine.promise().Exception) {
            std::rethrow_exception(_coroutine.promise().Exception);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(_coroutine.promise().Val);
        }
    }

private:
    handle_type _coroutine;
};

namespace detail {
template <typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}
}  // namespace detail

}  // namespace radray
