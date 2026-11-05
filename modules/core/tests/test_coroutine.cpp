#include <radray/coroutine.h>

#include <gtest/gtest.h>

#include <radray/nullable.h>

namespace radray {
namespace {

struct KeyedRecord : ManualCoroutineRecord {
    explicit KeyedRecord(int key) noexcept : Key(key) {}
    int Key;
};

using Scheduler = ManualCoroutineScheduler<KeyedRecord>;

// Suspends until the scheduler resumes the record; reports whether it completed without cancellation.
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
        return completed;
    }

private:
    Scheduler& _records;
    stop_token _stop;
    int _key;
    Nullable<KeyedRecord*> _record{nullptr};
};

template <class F>
task<void> WaitAndRun(Scheduler& records, int key, F continuation) {
    const auto stop = co_await CurrentStopToken();
    if (!co_await WaitForDispatch{records, stop, key}) co_await StopCurrentTask();
    continuation();
}

constexpr auto kAll = [](const KeyedRecord&) { return true; };

TEST(ManualCoroutineSchedulerTest, DispatchesOnlyReadyRecordsInRegistrationOrder) {
    Scheduler records;
    vector<int> completed;
    TaskScope tasks;
    for (int key : {1, 2, 3}) tasks.Spawn(WaitAndRun(records, key, [&, key] { completed.push_back(key); }));

    records.DispatchReady([](const KeyedRecord& record) { return record.Key != 2; });
    EXPECT_EQ(completed, (vector<int>{1, 3}));
    ASSERT_EQ(records.Count(), 1u);
    EXPECT_EQ(records.Front()->Key, 2);

    records.DispatchReady(kAll);
    EXPECT_EQ(completed, (vector<int>{1, 3, 2}));
    EXPECT_TRUE(records.Empty());
}

TEST(ManualCoroutineSchedulerTest, CallbackCanCancelAnotherWaiterAndAppendForNextDispatch) {
    Scheduler records;
    vector<int> completed;
    TaskScope first, second, appended;
    first.Spawn(WaitAndRun(records, 1, [&] {
        completed.push_back(1);
        second.RequestStop();
        appended.Spawn(WaitAndRun(records, 3, [&] { completed.push_back(3); }));
    }));
    second.Spawn(WaitAndRun(records, 2, [&] { completed.push_back(2); }));
    KeyedRecord* canceled = records.Back();
    const uint64_t canceledSequence = canceled->Sequence;

    records.DispatchReady(kAll);
    EXPECT_EQ(completed, (vector<int>{1}));
    EXPECT_FALSE(records.IsAlive(canceled, canceledSequence));
    ASSERT_EQ(records.Count(), 1u);
    EXPECT_GT(records.Front()->Sequence, canceledSequence);

    records.DispatchReady(kAll);
    EXPECT_EQ(completed, (vector<int>{1, 3}));
}

TEST(ManualCoroutineSchedulerTest, CapturedBoundariesExcludeWorkAddedDuringCrossSchedulerDispatch) {
    Scheduler first, second;
    vector<int> completed;
    TaskScope tasks;
    tasks.Spawn(WaitAndRun(first, 1, [&] {
        completed.push_back(1);
        tasks.Spawn(WaitAndRun(second, 3, [&] { completed.push_back(3); }));
    }));
    tasks.Spawn(WaitAndRun(second, 2, [&] { completed.push_back(2); }));
    const uint64_t firstBoundary = first.GetSequenceBoundary();
    const uint64_t secondBoundary = second.GetSequenceBoundary();

    first.DispatchReady(kAll, firstBoundary);
    second.DispatchReady(kAll, secondBoundary);
    EXPECT_EQ(completed, (vector<int>{1, 2}));
    ASSERT_EQ(second.Count(), 1u);

    second.DispatchReady(kAll);
    EXPECT_EQ(completed, (vector<int>{1, 2, 3}));
}

TEST(ManualCoroutineSchedulerTest, StopRequestResumesWaiterAsCanceled) {
    Scheduler records;
    bool completed = false;
    TaskScope tasks;
    tasks.Spawn(WaitAndRun(records, 1, [&] { completed = true; }));

    tasks.RequestStop();

    EXPECT_TRUE(records.Empty());
    EXPECT_FALSE(completed);
}

TEST(ManualCoroutineSchedulerTest, DeferredCancellationWaitsForOwnerToResume) {
    Scheduler records;
    bool completed = false;
    TaskScope tasks;
    tasks.Spawn(WaitAndRun(records, 1, [&] { completed = true; }));
    records.Front()->ResumeOnCancel = false;

    tasks.RequestStop();
    ASSERT_EQ(records.Count(), 1u);
    EXPECT_TRUE(records.Front()->Canceled);

    records.DispatchReady([](const KeyedRecord& record) { return record.Canceled; });
    EXPECT_TRUE(records.Empty());
    EXPECT_FALSE(completed);
}

TEST(TaskScopeTest, DestructionStopsAndJoinsSuspendedTasks) {
    Scheduler records;
    bool completed = false;
    {
        TaskScope tasks;
        tasks.Spawn(WaitAndRun(records, 1, [&] { completed = true; }));
        ASSERT_EQ(records.Count(), 1u);
    }

    EXPECT_TRUE(records.Empty());
    EXPECT_FALSE(completed);
}

task<int> ReturnUnlessStopped() {
    const stop_token stop = co_await CurrentStopToken();
    if (stop.stop_requested()) co_await StopCurrentTask();
    co_return 42;
}

TEST(AwaitWithStopTokenTest, ReturnsValueWhenNotStopped) {
    stop_source source;
    std::optional<int> result;
    {
        TaskScope tasks;
        tasks.Spawn([](stop_token stop, std::optional<int>& out) -> task<void> {
            out = co_await AwaitWithStopToken(ReturnUnlessStopped(), stop);
        }(source.get_token(), result));
    }

    EXPECT_EQ(result, 42);
}

TEST(AwaitWithStopTokenTest, ReturnsNulloptWhenStopped) {
    stop_source source;
    source.request_stop();
    std::optional<int> result{0};
    {
        TaskScope tasks;
        tasks.Spawn([](stop_token stop, std::optional<int>& out) -> task<void> {
            out = co_await AwaitWithStopToken(ReturnUnlessStopped(), stop);
        }(source.get_token(), result));
    }

    EXPECT_FALSE(result.has_value());
}

TEST(GetCoroutineStopTokenTest, TypeErasedHandleIsNeverStoppable) {
    EXPECT_FALSE(GetCoroutineStopToken(std::coroutine_handle<>{}).stop_possible());
}

}  // namespace
}  // namespace radray
