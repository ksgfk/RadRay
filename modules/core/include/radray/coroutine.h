#pragma once

// Coroutine support for RadRay, layered on stdexec (P2300 sender/receiver).
//
// Business code should use the radray aliases and helpers from this file
// instead of depending on exec::task / stdexec::* directly. That keeps the
// stdexec dependency behind a small facade.

#include <concepts>
#include <coroutine>
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

struct ManualCoroutineRecord {
    std::coroutine_handle<> Continuation{};
    stop_token Stop;
    bool Canceled{false};
};

template <class TRecord>
requires std::derived_from<TRecord, ManualCoroutineRecord>
class ManualCoroutineScheduler {
public:
    ManualCoroutineScheduler() noexcept = default;
    ManualCoroutineScheduler(const ManualCoroutineScheduler&) = delete;
    ManualCoroutineScheduler(ManualCoroutineScheduler&&) = delete;
    ManualCoroutineScheduler& operator=(const ManualCoroutineScheduler&) = delete;
    ManualCoroutineScheduler& operator=(ManualCoroutineScheduler&&) = delete;

    ~ManualCoroutineScheduler() noexcept {
        CancelAll();
    }

    template <class... Args>
    TRecord* Enqueue(stop_token stop, std::coroutine_handle<> continuation, Args&&... args) {
        auto entry = make_unique<Entry>(std::forward<Args>(args)...);
        TRecord* record = &entry->Record;
        record->Continuation = continuation;
        record->Stop = stop;
        record->Canceled = false;
        _records.emplace_back(std::move(entry));

        if (stop.stop_requested()) {
            record->Canceled = true;
        } else if (stop.stop_possible()) {
            _records.back()->StopCallback.emplace(stop, StopCallback{this, record});
        }
        return record;
    }

    bool Erase(TRecord* record) noexcept {
        for (size_t i = 0; i < _records.size(); ++i) {
            if (&_records[i]->Record == record) {
                _records[i]->StopCallback.reset();
                _records.erase(_records.begin() + static_cast<ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    bool IsAlive(TRecord* record) const noexcept {
        for (const unique_ptr<Entry>& entry : _records) {
            if (&entry->Record == record) {
                return true;
            }
        }
        return false;
    }

    TRecord* At(size_t index) noexcept {
        return index < _records.size() ? &_records[index]->Record : nullptr;
    }

    const TRecord* At(size_t index) const noexcept {
        return index < _records.size() ? &_records[index]->Record : nullptr;
    }

    TRecord* Front() noexcept {
        return _records.empty() ? nullptr : &_records.front()->Record;
    }

    TRecord* Back() noexcept {
        return _records.empty() ? nullptr : &_records.back()->Record;
    }

    size_t Count() const noexcept {
        return _records.size();
    }

    bool Empty() const noexcept {
        return _records.empty();
    }

    void ResumeRecord(TRecord* record) noexcept {
        if (record == nullptr) {
            return;
        }
        std::coroutine_handle<> continuation = record->Continuation;
        record->Continuation = {};
        if (continuation) {
            continuation.resume();
        }
    }

    void CancelRecord(TRecord* record) noexcept {
        if (record == nullptr) {
            return;
        }
        record->Canceled = true;
        ResumeRecord(record);
    }

    void CancelAll() noexcept {
        while (!_records.empty()) {
            TRecord* record = Back();
            record->Canceled = true;
            ResumeRecord(record);
            if (IsAlive(record)) {
                Erase(record);
            }
        }
    }

private:
    struct StopCallback {
        ManualCoroutineScheduler* Scheduler{nullptr};
        TRecord* Record{nullptr};

        void operator()() const noexcept {
            if (Scheduler != nullptr && Record != nullptr) {
                Scheduler->CancelRecord(Record);
            }
        }
    };

    using StopCallbackStorage = stop_token::template callback_type<StopCallback>;

    struct Entry {
        template <class... Args>
        explicit Entry(Args&&... args)
            : Record(std::forward<Args>(args)...) {}

        TRecord Record;
        std::optional<StopCallbackStorage> StopCallback;
    };

    vector<unique_ptr<Entry>> _records;
};

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
