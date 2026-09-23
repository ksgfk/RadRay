#include <gtest/gtest.h>

#include <radray/coroutine.h>
#include <radray/nullable.h>

namespace radray {
namespace {

struct ProbeRecord : ManualCoroutineRecord {
    explicit ProbeRecord(int key) noexcept : Key(key) {}
    int Key;
};

using Scheduler = ManualCoroutineScheduler<ProbeRecord>;

class WaitForDispatch {
public:
    WaitForDispatch(Scheduler& records, stop_token stop, int key) noexcept
        : _records(records), _stop(stop), _key(key) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> continuation) {
        _record = _records.Enqueue(_stop, continuation, _key);
    }
    bool await_resume() noexcept {
        const bool completed = !_record->Canceled && !_stop.stop_requested();
        _records.Erase(_record.Get());
        _record = nullptr;
        return completed;
    }

private:
    Scheduler& _records;
    stop_token _stop;
    int _key;
    Nullable<ProbeRecord*> _record{nullptr};
};

template <class F>
task<void> WaitAndRun(Scheduler& records, int key, F continuation) {
    const auto stop = co_await CurrentStopToken();
    if (!co_await WaitForDispatch{records, stop, key}) co_await StopCurrentTask();
    continuation();
}

TEST(ManualCoroutineScheduler, DispatchesOnlyReadyRecordsInRegistrationOrder) {
    Scheduler records;
    vector<int> completed;
    TaskScope tasks;
    for (int key : {1, 2, 3}) tasks.Spawn(WaitAndRun(records, key, [&, key] { completed.push_back(key); }));
    records.DispatchReady([](const auto& record) { return record.Key != 2; });
    EXPECT_EQ(completed, (vector<int>{1, 3}));
    ASSERT_EQ(records.Count(), 1u);
    EXPECT_EQ(records.Front()->Key, 2);
    records.DispatchReady([](const auto&) { return true; });
    EXPECT_EQ(completed, (vector<int>{1, 3, 2}));
    EXPECT_TRUE(records.Empty());
}

TEST(ManualCoroutineScheduler, CallbackCanCancelAnotherWaiterAndAppendForTheNextBatch) {
    Scheduler records;
    vector<int> completed;
    TaskScope first, second, appended;
    first.Spawn(WaitAndRun(records, 1, [&] {
        completed.push_back(1);
        second.RequestStop();
        appended.Spawn(WaitAndRun(records, 3, [&] { completed.push_back(3); }));
    }));
    second.Spawn(WaitAndRun(records, 2, [&] { completed.push_back(2); }));
    auto* oldRecord = records.Back();
    const auto oldSequence = oldRecord->Sequence;
    records.DispatchReady([](const auto&) { return true; });
    EXPECT_EQ(completed, (vector<int>{1}));
    EXPECT_FALSE(records.IsAlive(oldRecord, oldSequence));
    ASSERT_EQ(records.Count(), 1u);
    EXPECT_GT(records.Front()->Sequence, oldSequence);
    records.DispatchReady([](const auto&) { return true; });
    EXPECT_EQ(completed, (vector<int>{1, 3}));
}

TEST(ManualCoroutineScheduler, CapturedBoundariesExcludeCrossSchedulerCallbackWork) {
    Scheduler first, second;
    vector<int> completed;
    TaskScope tasks;
    tasks.Spawn(WaitAndRun(first, 1, [&] {
        completed.push_back(1);
        tasks.Spawn(WaitAndRun(second, 3, [&] { completed.push_back(3); }));
    }));
    tasks.Spawn(WaitAndRun(second, 2, [&] { completed.push_back(2); }));
    const auto firstBoundary = first.GetSequenceBoundary();
    const auto secondBoundary = second.GetSequenceBoundary();
    first.DispatchReady([](const auto&) { return true; }, firstBoundary);
    second.DispatchReady([](const auto&) { return true; }, secondBoundary);
    EXPECT_EQ(completed, (vector<int>{1, 2}));
    ASSERT_EQ(second.Count(), 1u);
    second.DispatchReady([](const auto&) { return true; });
    EXPECT_EQ(completed, (vector<int>{1, 2, 3}));
}

TEST(ManualCoroutineScheduler, DeferredCancellationWaitsForExplicitDispatch) {
    Scheduler records;
    bool completed = false;
    TaskScope tasks;
    tasks.Spawn(WaitAndRun(records, 1, [&] { completed = true; }));
    records.Front()->ResumeOnCancel = false;
    tasks.RequestStop();
    ASSERT_EQ(records.Count(), 1u);
    EXPECT_TRUE(records.Front()->Canceled);
    records.DispatchReady([](const auto& record) { return record.Canceled; });
    EXPECT_TRUE(records.Empty());
    EXPECT_FALSE(completed);
}

}  // namespace
}  // namespace radray
