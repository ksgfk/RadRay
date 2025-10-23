#pragma once

#include <mutex>
#include <condition_variable>
#include <utility>

#include <radray/types.h>

namespace radray {

template <class T>
class BoundedChannel {
public:
    explicit BoundedChannel(size_t capacity) noexcept
        : _capacity(capacity) {
        if (_capacity == 0) {
            _capacity = 1;
        }
    }

    BoundedChannel(const BoundedChannel&) = delete;
    BoundedChannel& operator=(const BoundedChannel&) = delete;
    BoundedChannel(BoundedChannel&&) = delete;
    BoundedChannel& operator=(BoundedChannel&&) = delete;

    ~BoundedChannel() noexcept = default;

    template <class U>
    bool TryWrite(U&& value) noexcept {
        std::unique_lock<std::mutex> lock(_mtx);
        if (_completed || _queue.size() >= _capacity) {
            return false;
        }
        _queue.emplace_back(std::forward<U>(value));
        _cv_not_empty.notify_one();
        return true;
    }

    template <class U>
    bool WaitWrite(U&& value) noexcept {
        std::unique_lock<std::mutex> lock(_mtx);
        _cv_not_full.wait(lock, [this] {
            return _queue.size() < _capacity || _completed;
        });
        if (_completed) {
            return false;
        }
        _queue.emplace_back(std::forward<U>(value));
        _cv_not_empty.notify_one();
        return true;
    }

    bool TryRead(T& out) noexcept {
        std::unique_lock<std::mutex> lock(_mtx);
        if (_queue.empty()) {
            return false;
        }
        out = std::move(_queue.front());
        _queue.pop_front();
        _cv_not_full.notify_one();
        return true;
    }

    bool WaitRead(T& out) noexcept {
        std::unique_lock<std::mutex> lock(_mtx);
        _cv_not_empty.wait(lock, [this] {
            return !_queue.empty() || _completed;
        });
        if (_queue.empty() && _completed) {
            return false;
        }
        out = std::move(_queue.front());
        _queue.pop_front();
        _cv_not_full.notify_one();
        return true;
    }

    void Complete() noexcept {
        {
            std::lock_guard<std::mutex> lock(_mtx);
            _completed = true;
        }
        _cv_not_empty.notify_all();
        _cv_not_full.notify_all();
    }

    bool IsCompleted() const noexcept {
        std::lock_guard<std::mutex> lock(_mtx);
        return _completed;
    }

    size_t Size() const noexcept {
        std::lock_guard<std::mutex> lock(_mtx);
        return _queue.size();
    }

    size_t Capacity() const noexcept { return _capacity; }

private:
    size_t _capacity{1};
    mutable std::mutex _mtx;
    std::condition_variable _cv_not_empty;
    std::condition_variable _cv_not_full;
    deque<T> _queue;
    bool _completed{false};
};

}  // namespace radray
