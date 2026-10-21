#include <gtest/gtest.h>

#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#ifdef CreateWindow
#undef CreateWindow
#endif
#endif

namespace radray {
namespace {

template <class T>
task<void> CollectWindowResult(task<T> operation, T& result) {
    result = co_await std::move(operation);
}

task<void> CollectWindowStop(task<WindowOperationStatus> operation, stop_token stop,
                             std::optional<WindowOperationStatus>& result, bool& finished) {
    result = co_await AwaitWithStopToken(std::move(operation), stop);
    finished = true;
}

task<void> RecordWindowResult(task<WindowOperationStatus> operation, vector<int>& events, int value,
                              WindowOperationStatus expected = WindowOperationStatus::Completed) {
    EXPECT_EQ(co_await std::move(operation), expected);
    events.push_back(value);
}

task<void> ResizeTwice(WindowManager& manager, WindowHandle handle, vector<int>& events) {
    EXPECT_EQ(co_await manager.SetSize(handle, 320, 300), WindowOperationStatus::Completed);
    events.push_back(1);
    EXPECT_EQ(co_await manager.SetSize(handle, 360, 300), WindowOperationStatus::Completed);
    events.push_back(2);
}

task<void> MutateNativeAfterCompletion(WindowManager& manager, WindowHandle handle) {
    co_await manager.SetSize(handle, 320, 300);
    manager.ResolveWindow(handle)->GetNativeWindow()->SetDecorated(false);
}

class WindowOperationsTest : public ::testing::Test {
protected:
    void SetUp() override {
#if defined(RADRAY_PLATFORM_WINDOWS)
        Manager = make_unique<WindowManager>(WindowManagerDescriptor{NativeWindowType::Win32HWND});
#else
        GTEST_SKIP() << "runtime window tests require Windows";
#endif
    }

    WindowHandle Create(bool main = false) {
        WindowCreateDescriptor desc{};
        desc.Title = "Window operation test";
        desc.Width = 300;
        desc.Height = 80;
        desc.StartVisible = false;
        WindowCreateResult result;
        Tasks.Spawn(CollectWindowResult(Manager->CreateWindow(std::move(desc), main), result));
        Pump();
        EXPECT_EQ(result.Status, WindowOperationStatus::Completed);
        return result.Handle;
    }

    void Pump() { Manager->ProcessOperations(Manager->GetOperationBoundary()); }

    unique_ptr<WindowManager> Manager;
    TaskScope Tasks;
};

TEST_F(WindowOperationsTest, CreateOwnsParametersUntilStartedAndPumped) {
    WindowCreateDescriptor desc{};
    desc.Title = "Owned window title";
    desc.Width = 301;
    desc.Height = 83;
    desc.StartVisible = false;
    auto operation = Manager->CreateWindow(desc);
    desc.Title.assign(4096, 'x');
    WindowCreateResult result;
    Tasks.Spawn(CollectWindowResult(std::move(operation), result));
    EXPECT_EQ(Manager->GetWindowCount(), 0u);
    Pump();
    auto window = Manager->ResolveWindow(result.Handle);
    ASSERT_TRUE(window);
    EXPECT_EQ(window->GetSize(), (Eigen::Vector2i{301, 83}));
#if defined(RADRAY_PLATFORM_WINDOWS)
    wchar_t title[64]{};
    GetWindowTextW(static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler()), title, 64);
    EXPECT_STREQ(title, L"Owned window title");
#endif
    EXPECT_FALSE(Manager->NeedsMaintenance());
}

TEST_F(WindowOperationsTest, AppliesFifoBeforeDeliveringAnyResults) {
    const auto handle = Create();
    vector<int> events;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int width, int) { events.push_back(width); });
    Tasks.Spawn(RecordWindowResult(Manager->SetSize(handle, 320, 300), events, 1));
    Tasks.Spawn(RecordWindowResult(Manager->SetSize(handle, 340, 300), events, 2));
    Pump();
    EXPECT_EQ(events, (vector<int>{320, 340, 1, 2}));
}

TEST_F(WindowOperationsTest, CompletionContinuationStartsANewBatch) {
    const auto handle = Create();
    vector<int> events;
    Tasks.Spawn(ResizeTwice(*Manager, handle, events));
    Pump();
    EXPECT_EQ(events, (vector<int>{1}));
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 320);
    EXPECT_TRUE(Manager->NeedsMaintenance());
    Pump();
    EXPECT_EQ(events, (vector<int>{1, 2}));
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 360);
}

TEST_F(WindowOperationsTest, InvalidAndFailedResultsWaitUntilTheBatchHasFinished) {
    const auto handle = Create();
    vector<int> events;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int width, int) { events.push_back(width); });
    Tasks.Spawn(RecordWindowResult(Manager->SetSize({}, 320, 300), events, 1, WindowOperationStatus::InvalidWindow));
    Tasks.Spawn(RecordWindowResult(Manager->SetSize(handle, 0, 300), events, 2, WindowOperationStatus::Failed));
    Tasks.Spawn(RecordWindowResult(Manager->SetSize(handle, 340, 300), events, 3));
    Pump();
    EXPECT_EQ(events, (vector<int>{340, 1, 2, 3}));
}

TEST_F(WindowOperationsTest, BoundarySurvivesCancellationAndNewRequests) {
    const auto handle = Create();
    stop_source stop;
    std::optional<WindowOperationStatus> canceled;
    bool canceledFinished = false;
    Tasks.Spawn(CollectWindowStop(Manager->SetSize(handle, 320, 300), stop.get_token(), canceled, canceledFinished));
    auto second = WindowOperationStatus::Failed;
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 340, 300), second));
    const uint64_t boundary = Manager->GetOperationBoundary();
    stop.request_stop();
    EXPECT_TRUE(canceledFinished);
    EXPECT_FALSE(canceled.has_value());
    auto third = WindowOperationStatus::Failed;
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 360, 300), third));
    Manager->ProcessOperations(boundary);
    EXPECT_EQ(second, WindowOperationStatus::Completed);
    EXPECT_EQ(third, WindowOperationStatus::Failed);
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 340);
    Pump();
    EXPECT_EQ(third, WindowOperationStatus::Completed);
}

TEST_F(WindowOperationsTest, NativeCallbackEnqueuesForNextBatch) {
    const auto handle = Create();
    auto next = WindowOperationStatus::Failed;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int width, int) {
            if (width == 320) Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 360, 300), next));
        });
    auto first = WindowOperationStatus::Failed;
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 320, 300), first));
    Pump();
    EXPECT_EQ(first, WindowOperationStatus::Completed);
    EXPECT_EQ(next, WindowOperationStatus::Failed);
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 320);
    Pump();
    EXPECT_EQ(next, WindowOperationStatus::Completed);
}

TEST_F(WindowOperationsTest, CancelDuringNativeMutationDefersUnwindUntilItReturns) {
    const auto handle = Create();
    stop_source stop;
    std::optional<WindowOperationStatus> result;
    bool finished = false, callback = false;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int, int) {
            callback = true;
            stop.request_stop();
            EXPECT_FALSE(finished);
        });
    Tasks.Spawn(CollectWindowStop(Manager->SetSize(handle, 320, 300), stop.get_token(), result, finished));
    Pump();
    EXPECT_TRUE(callback);
    EXPECT_TRUE(finished);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 320);
    EXPECT_FALSE(Manager->NeedsMaintenance());
}

TEST_F(WindowOperationsTest, CancelOtherPendingOperationDoesNotResumeCallerInsideMutation) {
    const auto handle = Create();
    stop_source stop;
    std::optional<WindowOperationStatus> canceled;
    bool finished = false;
    auto first = WindowOperationStatus::Failed;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int, int) {
            stop.request_stop();
            EXPECT_FALSE(finished);
        });
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 320, 300), first));
    Tasks.Spawn(CollectWindowStop(Manager->SetSize(handle, 340, 300), stop.get_token(), canceled, finished));
    Pump();
    EXPECT_TRUE(finished);
    EXPECT_FALSE(canceled.has_value());
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 320);
}

TEST_F(WindowOperationsTest, CancelExecutedOperationWaitsForResultDelivery) {
    const auto handle = Create();
    stop_source stop;
    std::optional<WindowOperationStatus> canceled;
    bool finished = false;
    auto second = WindowOperationStatus::Failed;
    vector<int> events;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int width, int) {
            events.push_back(width);
            if (width == 340) stop.request_stop();
            EXPECT_FALSE(finished);
        });
    Tasks.Spawn(CollectWindowStop(Manager->SetSize(handle, 320, 300), stop.get_token(), canceled, finished));
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 340, 300), second));
    Pump();
    EXPECT_EQ(events, (vector<int>{320, 340}));
    EXPECT_TRUE(finished);
    EXPECT_FALSE(canceled.has_value());
    EXPECT_EQ(second, WindowOperationStatus::Completed);
    EXPECT_FALSE(Manager->NeedsMaintenance());
}

TEST_F(WindowOperationsTest, CancelNewRequestDuringMutationDefersItsStopContinuation) {
    const auto handle = Create();
    stop_source stop;
    std::optional<WindowOperationStatus> canceled;
    bool finished = false;
    auto first = WindowOperationStatus::Failed;
    sigslot::scoped_connection resized = Manager->ResolveWindow(handle)->GetNativeWindow()->EventResized().connect(
        [&](int width, int) {
            if (width != 320) return;
            Tasks.Spawn(CollectWindowStop(Manager->SetSize(handle, 340, 300), stop.get_token(), canceled, finished));
            stop.request_stop();
            EXPECT_FALSE(finished);
        });
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(handle, 320, 300), first));
    Pump();
    EXPECT_EQ(first, WindowOperationStatus::Completed);
    EXPECT_TRUE(finished);
    EXPECT_FALSE(canceled.has_value());
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 320);
    EXPECT_FALSE(Manager->NeedsMaintenance());
}

TEST_F(WindowOperationsTest, DestroyInvalidatesTargetsAndOwnersWithoutReusingIdentity) {
    const auto owner = Create();
    const auto child = Create();
    auto destroyed = WindowOperationStatus::Failed;
    auto resized = WindowOperationStatus::Failed;
    auto reparented = WindowOperationStatus::Failed;
    WindowCreateDescriptor desc{};
    desc.Title = "Invalid owner";
    desc.Width = 80;
    desc.Height = 60;
    desc.OwnerWindow = owner;
    WindowCreateResult created;
    Tasks.Spawn(CollectWindowResult(Manager->DestroyWindow(owner), destroyed));
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(owner, 320, 300), resized));
    Tasks.Spawn(CollectWindowResult(Manager->SetOwner(child, owner), reparented));
    Tasks.Spawn(CollectWindowResult(Manager->CreateWindow(desc), created));
    Pump();
    EXPECT_EQ(destroyed, WindowOperationStatus::Completed);
    EXPECT_EQ(resized, WindowOperationStatus::InvalidWindow);
    EXPECT_EQ(reparented, WindowOperationStatus::InvalidWindow);
    EXPECT_EQ(created.Status, WindowOperationStatus::InvalidWindow);
    EXPECT_FALSE(Manager->ResolveWindow(owner));
    EXPECT_GT(Create().Id, child.Id);
    EXPECT_FALSE(Manager->ResolveWindow(WindowHandle{nullptr, child.Id}));
}

TEST_F(WindowOperationsTest, DestroyMainWindowLatchesExit) {
    const auto main = Create(true);
    auto status = WindowOperationStatus::Failed;
    Tasks.Spawn(CollectWindowResult(Manager->DestroyWindow(main), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Completed);
    EXPECT_EQ(Manager->GetMainWindow(), nullptr);
    EXPECT_TRUE(Manager->ShouldExit());
}

TEST_F(WindowOperationsTest, ShutdownStopsPendingAndFutureOperations) {
    const auto handle = Create();
    std::optional<WindowOperationStatus> result;
    bool finished = false;
    Tasks.Spawn(CollectWindowStop(Manager->SetSize(handle, 320, 300), {}, result, finished));
    Manager->CloseOperations();
    EXPECT_TRUE(finished);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(Manager->ResolveWindow(handle)->GetSize().x(), 300);
    finished = false;
    Tasks.Spawn(CollectWindowStop(Manager->DestroyWindow(handle), {}, result, finished));
    EXPECT_TRUE(finished);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(Manager->ResolveWindow(handle));
}

TEST_F(WindowOperationsTest, HiddenSwapChainRequestDefersWithoutSpinningMaintenance) {
    const auto handle = Create();
    auto status = WindowOperationStatus::Failed;
    Tasks.Spawn(CollectWindowResult(Manager->AttachSwapChain(handle, WindowSwapChainDescriptor{}), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Deferred);
    EXPECT_FALSE(Manager->NeedsMaintenance());
    EXPECT_EQ(Manager->ResolveWindow(handle)->AcquireNextSwapChainFrame({}).Status, render::SwapChainStatus::RetryLater);
    Tasks.Spawn(CollectWindowResult(Manager->DetachSwapChain(handle), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Completed);
}

TEST_F(WindowOperationsTest, FailedSwapChainSetupRetainsRetryUntilDetached) {
    const auto handle = Create();
    auto status = WindowOperationStatus::Completed;
    Tasks.Spawn(CollectWindowResult(Manager->Show(handle, NativeWindowShowMode::NoActivate), status));
    Tasks.Spawn(CollectWindowResult(Manager->AttachSwapChain(handle, WindowSwapChainDescriptor{}), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Failed);
    EXPECT_TRUE(Manager->NeedsMaintenance());
    EXPECT_FALSE(Manager->ResolveWindow(handle)->IsSwapChainPresentable());
    EXPECT_EQ(Manager->ResolveWindow(handle)->AcquireNextSwapChainFrame({}).Status, render::SwapChainStatus::RetryLater);
    Tasks.Spawn(CollectWindowResult(Manager->DetachSwapChain(handle), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Completed);
    EXPECT_FALSE(Manager->NeedsMaintenance());
}

TEST_F(WindowOperationsTest, DestroyOwnerDetachesLiveChildren) {
    const auto owner = Create();
    WindowCreateDescriptor desc{};
    desc.Title = "Owned child";
    desc.Width = 300;
    desc.Height = 200;
    desc.StartVisible = false;
    desc.OwnerWindow = owner;
    WindowCreateResult child;
    Tasks.Spawn(CollectWindowResult(Manager->CreateWindow(desc), child));
    Pump();
    ASSERT_EQ(child.Status, WindowOperationStatus::Completed);
    auto status = WindowOperationStatus::Failed;
    Tasks.Spawn(CollectWindowResult(Manager->DestroyWindow(owner), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Completed);
    EXPECT_TRUE(Manager->ResolveWindow(child.Handle)->GetNativeWindow()->IsValid());
    Tasks.Spawn(CollectWindowResult(Manager->SetSize(child.Handle, 340, 200), status));
    Pump();
    EXPECT_EQ(status, WindowOperationStatus::Completed);
}

#if defined(RADRAY_PLATFORM_WINDOWS)
TEST_F(WindowOperationsTest, ImmediateNativeMutationOutsideMaintenanceIsRejected) {
    const auto handle = Create();
    auto* native = Manager->ResolveWindow(handle)->GetNativeWindow();
    EXPECT_DEATH(native->SetSize(320, 300), "");
    EXPECT_DEATH(native->SetDecorated(false), "");
    EXPECT_DEATH(native->Destroy(), "");
    EXPECT_DEATH({ Tasks.Spawn(MutateNativeAfterCompletion(*Manager, handle)); Pump(); }, "");
}
#endif

}  // namespace
}  // namespace radray
