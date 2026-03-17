#include <memory>
#include <utility>

#include <gtest/gtest.h>

#include <radray/render/common.h>

#include "gpu_runtime_test_support.h"

#define private public
#include <radray/runtime/gpu_system.h>
#undef private

using namespace radray;
using namespace radray::render;
using namespace radray::runtime_tests;

namespace {

struct RuntimeHarness {
    RuntimeHarness() {
        DirectQueue = Device->EnsureQueue(QueueType::Direct);
        ComputeQueue = Device->EnsureQueue(QueueType::Compute);
        CopyQueue = Device->EnsureQueue(QueueType::Copy);

        Runtime._backend = Device->Backend;
        Runtime._device = Device;
        Runtime._graphicsFence = GraphicsFence;
        Runtime._computeFence = ComputeFence;
        Runtime._copyFence = CopyFence;
    }

    void Attach(GpuFrameContext& frame) {
        frame._runtime = &Runtime;
    }

    void Attach(GpuAsyncContext& asyncContext) {
        asyncContext._runtime = &Runtime;
    }

    void Attach(GpuPresentSurface& surface, uint32_t width = 640, uint32_t height = 360, uint32_t backBufferCount = 3) {
        SwapChainDescriptor desc{};
        desc.PresentQueue = DirectQueue;
        desc.Width = width;
        desc.Height = height;
        desc.BackBufferCount = backBufferCount;
        desc.FlightFrameCount = 2;
        desc.Format = TextureFormat::BGRA8_UNORM;
        desc.PresentMode = PresentMode::FIFO;

        surface._runtime = &Runtime;
        surface._swapchain = std::make_unique<FakeSwapChain>(desc);
        surface._slotRetireStates.resize(desc.FlightFrameCount);
    }

    std::unique_ptr<GpuFrameContext> MakeOwnedFrame() {
        auto frame = std::make_unique<GpuFrameContext>();
        Attach(*frame);
        return frame;
    }

    std::unique_ptr<GpuAsyncContext> MakeOwnedAsync() {
        auto asyncContext = std::make_unique<GpuAsyncContext>();
        Attach(*asyncContext);
        return asyncContext;
    }

    GpuTask MakeTask(shared_ptr<FakeFence> fence, uint64_t targetValue) {
        GpuTask task{};
        task._runtime = &Runtime;
        task._completion = std::make_shared<GpuCompletionState>();
        task._completion->Points.push_back(GpuCompletionPoint{fence, targetValue});
        return task;
    }

    GpuRuntime Runtime{};
    shared_ptr<FakeDevice> Device{std::make_shared<FakeDevice>()};
    shared_ptr<FakeFence> GraphicsFence{std::make_shared<FakeFence>()};
    shared_ptr<FakeFence> ComputeFence{std::make_shared<FakeFence>()};
    shared_ptr<FakeFence> CopyFence{std::make_shared<FakeFence>()};
    FakeCommandQueue* DirectQueue{nullptr};
    FakeCommandQueue* ComputeQueue{nullptr};
    FakeCommandQueue* CopyQueue{nullptr};
};

GpuTask MakeTaskFromPoints(
    GpuRuntime* runtime,
    std::initializer_list<std::pair<shared_ptr<FakeFence>, uint64_t>> points) {
    GpuTask task{};
    task._runtime = runtime;
    task._completion = std::make_shared<GpuCompletionState>();
    for (const auto& [fence, value] : points) {
        task._completion->Points.push_back(GpuCompletionPoint{fence, value});
    }
    return task;
}

TEST(GpuTaskCoreTest, DefaultConstructedTaskIsInvalid) {
    GpuTask task{};

    EXPECT_FALSE(task.IsValid());
    EXPECT_FALSE(task.IsCompleted());
    task.Wait();
}

TEST(GpuTaskCoreTest, SingleCompletionPointUsesFenceValue) {
    RuntimeHarness harness{};
    auto fence = std::make_shared<FakeFence>();
    auto task = harness.MakeTask(fence, 5);

    fence->CompletedValue = 4;
    EXPECT_FALSE(task.IsCompleted());

    fence->CompletedValue = 5;
    EXPECT_TRUE(task.IsCompleted());
}

TEST(GpuTaskCoreTest, MultiCompletionPointRequiresAllPoints) {
    RuntimeHarness harness{};
    auto graphicsFence = std::make_shared<FakeFence>();
    auto computeFence = std::make_shared<FakeFence>();
    auto task = MakeTaskFromPoints(
        &harness.Runtime,
        {
            {graphicsFence, 3},
            {computeFence, 7},
        });

    graphicsFence->CompletedValue = 3;
    computeFence->CompletedValue = 6;
    EXPECT_FALSE(task.IsCompleted());

    computeFence->CompletedValue = 7;
    EXPECT_TRUE(task.IsCompleted());
}

TEST(GpuTaskCoreTest, WaitPropagatesToAllFences) {
    RuntimeHarness harness{};
    auto graphicsFence = std::make_shared<FakeFence>();
    auto computeFence = std::make_shared<FakeFence>();
    graphicsFence->AdvanceOnWait = true;
    graphicsFence->WaitAdvanceValue = 4;
    computeFence->AdvanceOnWait = true;
    computeFence->WaitAdvanceValue = 9;

    auto task = MakeTaskFromPoints(
        &harness.Runtime,
        {
            {graphicsFence, 4},
            {computeFence, 9},
        });

    task.Wait();

    EXPECT_EQ(graphicsFence->WaitCallCount, 1u);
    EXPECT_EQ(computeFence->WaitCallCount, 1u);
    EXPECT_TRUE(task.IsCompleted());
}

TEST(GpuFrameContextCoreTest, DefaultContextStartsEmpty) {
    RuntimeHarness harness{};
    GpuFrameContext frame{};
    harness.Attach(frame);

    EXPECT_TRUE(frame.IsEmpty());
}

TEST(GpuFrameContextCoreTest, WaitForRejectsInvalidTaskAndStoresValidDependency) {
    RuntimeHarness harness{};
    GpuFrameContext frame{};
    harness.Attach(frame);

    GpuTask invalidTask{};
    EXPECT_FALSE(frame.WaitFor(invalidTask));
    EXPECT_TRUE(frame._dependencies.empty());

    auto fence = std::make_shared<FakeFence>();
    auto task = harness.MakeTask(fence, 2);
    EXPECT_TRUE(frame.WaitFor(task));
    ASSERT_EQ(frame._dependencies.size(), 1u);
    EXPECT_EQ(frame._dependencies.front(), task._completion);
}

TEST(GpuFrameContextCoreTest, AddRejectsNullCommandBuffers) {
    RuntimeHarness harness{};
    GpuFrameContext frame{};
    harness.Attach(frame);

    EXPECT_FALSE(frame.AddGraphicsCommandBuffer({}));
    EXPECT_FALSE(frame.AddComputeCommandBuffer({}));
    EXPECT_FALSE(frame.AddCopyCommandBuffer({}));
    EXPECT_TRUE(frame.IsEmpty());
}

TEST(GpuFrameContextCoreTest, CreateCommandBuffersUseMatchingQueues) {
    RuntimeHarness harness{};
    GpuFrameContext frame{};
    harness.Attach(frame);

    auto graphicsCmd = frame.CreateGraphicsCommandBuffer();
    ASSERT_TRUE(graphicsCmd.HasValue());
    auto graphics = graphicsCmd.Release();
    auto* fakeGraphics = dynamic_cast<FakeCommandBuffer*>(graphics.get());
    ASSERT_NE(fakeGraphics, nullptr);
    EXPECT_EQ(fakeGraphics->QueueType, QueueType::Direct);

    auto computeCmd = frame.CreateComputeCommandBuffer();
    ASSERT_TRUE(computeCmd.HasValue());
    auto compute = computeCmd.Release();
    auto* fakeCompute = dynamic_cast<FakeCommandBuffer*>(compute.get());
    ASSERT_NE(fakeCompute, nullptr);
    EXPECT_EQ(fakeCompute->QueueType, QueueType::Compute);

    auto copyCmd = frame.CreateCopyCommandBuffer();
    ASSERT_TRUE(copyCmd.HasValue());
    auto copy = copyCmd.Release();
    auto* fakeCopy = dynamic_cast<FakeCommandBuffer*>(copy.get());
    ASSERT_NE(fakeCopy, nullptr);
    EXPECT_EQ(fakeCopy->QueueType, QueueType::Copy);
}

TEST(GpuFrameContextCoreTest, CreateCommandBufferReturnsNullWhenQueueMissing) {
    RuntimeHarness harness{};
    harness.Device = std::make_shared<FakeDevice>();
    harness.Runtime._device = harness.Device;
    GpuFrameContext frame{};
    harness.Attach(frame);

    EXPECT_FALSE(frame.CreateGraphicsCommandBuffer().HasValue());
    EXPECT_FALSE(frame.CreateComputeCommandBuffer().HasValue());
    EXPECT_FALSE(frame.CreateCopyCommandBuffer().HasValue());
}

TEST(GpuFrameContextCoreTest, HelperPathAndPlanPathAreMutuallyExclusive) {
    RuntimeHarness harness{};

    GpuFrameContext helperFrame{};
    harness.Attach(helperFrame);
    auto graphicsCmd = helperFrame.CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(graphicsCmd.get(), nullptr);
    EXPECT_TRUE(helperFrame.AddGraphicsCommandBuffer(std::move(graphicsCmd)));
    GpuQueueSubmitStep helperStep{};
    vector<GpuQueueSubmitStep> helperPlan{};
    helperPlan.push_back(std::move(helperStep));
    EXPECT_FALSE(helperFrame.SetSubmissionPlan(std::move(helperPlan)));

    GpuFrameContext plannedFrame{};
    harness.Attach(plannedFrame);
    auto plannedCmd = plannedFrame.CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(plannedCmd.get(), nullptr);
    GpuQueueSubmitStep plannedStep{};
    plannedStep.Queue = QueueType::Direct;
    plannedStep.CommandBuffers.push_back(std::move(plannedCmd));
    vector<GpuQueueSubmitStep> plannedPlan{};
    plannedPlan.push_back(std::move(plannedStep));
    EXPECT_TRUE(plannedFrame.SetSubmissionPlan(std::move(plannedPlan)));

    auto lateCmd = plannedFrame.CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(lateCmd.get(), nullptr);
    EXPECT_FALSE(plannedFrame.AddGraphicsCommandBuffer(std::move(lateCmd)));
}

TEST(GpuFrameContextCoreTest, HelperPathRejectsMultipleQueueTypes) {
    RuntimeHarness harness{};
    GpuFrameContext frame{};
    harness.Attach(frame);

    auto graphicsCmd = frame.CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(graphicsCmd.get(), nullptr);
    EXPECT_TRUE(frame.AddGraphicsCommandBuffer(std::move(graphicsCmd)));

    auto computeCmd = frame.CreateComputeCommandBuffer().Release();
    ASSERT_NE(computeCmd.get(), nullptr);
    EXPECT_FALSE(frame.AddComputeCommandBuffer(std::move(computeCmd)));
}

TEST(GpuAsyncContextCoreTest, MirrorsFrameContextHelperAndPlanRules) {
    RuntimeHarness harness{};
    GpuAsyncContext asyncContext{};
    harness.Attach(asyncContext);

    EXPECT_TRUE(asyncContext.IsEmpty());
    EXPECT_FALSE(asyncContext.WaitFor(GpuTask{}));

    auto graphicsCmd = asyncContext.CreateGraphicsCommandBuffer();
    ASSERT_TRUE(graphicsCmd.HasValue());
    auto graphics = graphicsCmd.Release();
    auto* fakeGraphics = dynamic_cast<FakeCommandBuffer*>(graphics.get());
    ASSERT_NE(fakeGraphics, nullptr);
    EXPECT_EQ(fakeGraphics->QueueType, QueueType::Direct);
    EXPECT_TRUE(asyncContext.AddGraphicsCommandBuffer(std::move(graphics)));

    auto computeCmd = asyncContext.CreateComputeCommandBuffer().Release();
    ASSERT_NE(computeCmd.get(), nullptr);
    EXPECT_FALSE(asyncContext.AddComputeCommandBuffer(std::move(computeCmd)));
}

TEST(GpuRuntimeCoreTest, EmptySubmitsReturnInvalidTasks) {
    RuntimeHarness harness{};

    auto frame = harness.MakeOwnedFrame();
    auto frameTask = harness.Runtime.Submit(std::move(frame));
    EXPECT_FALSE(frameTask.IsValid());

    auto asyncContext = harness.MakeOwnedAsync();
    auto asyncTask = harness.Runtime.Submit(std::move(asyncContext));
    EXPECT_FALSE(asyncTask.IsValid());
}

TEST(GpuRuntimeCoreTest, HelperPathSubmitUsesSingleQueueAndSignalsFence) {
    RuntimeHarness harness{};
    auto frame = harness.MakeOwnedFrame();

    auto dependencyFence = std::make_shared<FakeFence>();
    GpuTask dependency = harness.MakeTask(dependencyFence, 6);
    ASSERT_TRUE(frame->WaitFor(dependency));

    auto graphicsCmd = frame->CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(graphicsCmd.get(), nullptr);
    auto* rawGraphicsCmd = graphicsCmd.get();
    ASSERT_TRUE(frame->AddGraphicsCommandBuffer(std::move(graphicsCmd)));

    auto task = harness.Runtime.Submit(std::move(frame));

    EXPECT_TRUE(task.IsValid());
    ASSERT_EQ(harness.DirectQueue->Submits.size(), 1u);
    const SubmittedBatch& batch = harness.DirectQueue->Submits.front();
    ASSERT_EQ(batch.CommandBuffers.size(), 1u);
    EXPECT_EQ(batch.CommandBuffers.front(), rawGraphicsCmd);
    ASSERT_EQ(batch.WaitFences.size(), 1u);
    EXPECT_EQ(batch.WaitFences.front(), dependencyFence.get());
    ASSERT_EQ(batch.WaitValues.size(), 1u);
    EXPECT_EQ(batch.WaitValues.front(), 6u);
    ASSERT_EQ(batch.SignalFences.size(), 1u);
    EXPECT_EQ(batch.SignalFences.front(), harness.GraphicsFence.get());
    ASSERT_EQ(batch.SignalValues.size(), 1u);
    EXPECT_GT(batch.SignalValues.front(), 0u);
}

TEST(GpuRuntimeCoreTest, ExplicitPlanSubmitUsesDeclaredQueuesAndCommandOrder) {
    RuntimeHarness harness{};
    auto frame = harness.MakeOwnedFrame();

    auto computeCmd = frame->CreateComputeCommandBuffer().Release();
    auto graphicsCmd = frame->CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(computeCmd.get(), nullptr);
    ASSERT_NE(graphicsCmd.get(), nullptr);
    auto* rawCompute = computeCmd.get();
    auto* rawGraphics = graphicsCmd.get();

    GpuQueueSubmitStep computeStep{};
    computeStep.Queue = QueueType::Compute;
    computeStep.CommandBuffers.push_back(std::move(computeCmd));

    GpuQueueSubmitStep graphicsStep{};
    graphicsStep.Queue = QueueType::Direct;
    graphicsStep.WaitStepIndices.push_back(0);
    graphicsStep.CommandBuffers.push_back(std::move(graphicsCmd));

    vector<GpuQueueSubmitStep> plan{};
    plan.push_back(std::move(computeStep));
    plan.push_back(std::move(graphicsStep));
    ASSERT_TRUE(frame->SetSubmissionPlan(std::move(plan)));

    auto task = harness.Runtime.Submit(std::move(frame));

    EXPECT_TRUE(task.IsValid());
    ASSERT_EQ(harness.ComputeQueue->Submits.size(), 1u);
    EXPECT_EQ(harness.ComputeQueue->Submits.front().CommandBuffers.front(), rawCompute);

    ASSERT_EQ(harness.DirectQueue->Submits.size(), 1u);
    EXPECT_EQ(harness.DirectQueue->Submits.front().CommandBuffers.front(), rawGraphics);
    EXPECT_FALSE(harness.DirectQueue->Submits.front().WaitFences.empty());
}

TEST(GpuRuntimeCoreTest, ProcessTasksReclaimsCompletedTasksAndKeepsPendingOnes) {
    RuntimeHarness harness{};
    auto completedFence = std::make_shared<FakeFence>();
    completedFence->CompletedValue = 5;
    auto pendingFence = std::make_shared<FakeFence>();
    pendingFence->CompletedValue = 1;

    GpuRuntime::InFlightTask completed{};
    completed.Completion = std::make_shared<GpuCompletionState>();
    completed.Completion->Points.push_back(GpuCompletionPoint{completedFence, 5});
    completed.Frame = std::make_unique<GpuFrameContext>();
    harness.Attach(*completed.Frame);

    GpuRuntime::InFlightTask pending{};
    pending.Completion = std::make_shared<GpuCompletionState>();
    pending.Completion->Points.push_back(GpuCompletionPoint{pendingFence, 3});
    pending.Async = std::make_unique<GpuAsyncContext>();
    harness.Attach(*pending.Async);

    harness.Runtime._inFlightTasks.push_back(std::move(completed));
    harness.Runtime._inFlightTasks.push_back(std::move(pending));

    harness.Runtime.ProcessTasks();

    ASSERT_EQ(harness.Runtime._inFlightTasks.size(), 1u);
    ASSERT_NE(harness.Runtime._inFlightTasks.front().Completion, nullptr);
    EXPECT_EQ(harness.Runtime._inFlightTasks.front().Completion->Points.front().TargetValue, 3u);
}

TEST(GpuPresentSurfaceCoreTest, DefaultSurfaceIsInvalidAndDestroyIsIdempotent) {
    GpuPresentSurface surface{};

    EXPECT_FALSE(surface.IsValid());
    surface.Destroy();
    surface.Destroy();
    EXPECT_FALSE(surface.IsValid());
}

TEST(GpuPresentSurfaceCoreTest, AcquireSurfaceReturnsLeaseFromFakeSwapChain) {
    RuntimeHarness harness{};
    GpuPresentSurface surface{};
    harness.Attach(surface);
    auto* fakeSwapChain = dynamic_cast<FakeSwapChain*>(surface._swapchain.get());
    ASSERT_NE(fakeSwapChain, nullptr);
    fakeSwapChain->EnqueueAcquireSuccess(1);

    GpuFrameContext frame{};
    harness.Attach(frame);

    auto result = frame.AcquireSurface(surface);

    EXPECT_EQ(result.Status, GpuSurfaceAcquireStatus::Success);
    ASSERT_TRUE(result.Lease.HasValue());
    EXPECT_TRUE(result.Lease.Get()->IsValid());
    EXPECT_EQ(result.Lease.Get()->GetSurface(), &surface);
    EXPECT_EQ(result.Lease.Get()->GetFrameSlotIndex(), 1u);
    EXPECT_EQ(result.Lease.Get()->GetBackBufferTexture(), fakeSwapChain->GetBackBuffer(1));
}

TEST(GpuPresentSurfaceCoreTest, PresentRejectsInvalidForeignAndDuplicateLeases) {
    RuntimeHarness harness{};
    GpuPresentSurface surface{};
    harness.Attach(surface);
    auto* fakeSwapChain = dynamic_cast<FakeSwapChain*>(surface._swapchain.get());
    ASSERT_NE(fakeSwapChain, nullptr);

    GpuFrameContext frame{};
    harness.Attach(frame);

    GpuSurfaceLease invalidLease{};
    EXPECT_FALSE(frame.Present(invalidLease));

    auto ownedLease = std::make_unique<GpuSurfaceLease>();
    ownedLease->_surface = &surface;
    ownedLease->_backBuffer = fakeSwapChain->GetBackBuffer(0);
    ownedLease->_frameSlotIndex = 0;
    auto* ownedLeasePtr = ownedLease.get();
    frame._surfaceLeases.push_back(std::move(ownedLease));

    EXPECT_TRUE(frame.Present(*ownedLeasePtr));
    EXPECT_FALSE(frame.Present(*ownedLeasePtr));

    GpuFrameContext foreignFrame{};
    harness.Attach(foreignFrame);
    EXPECT_FALSE(foreignFrame.Present(*ownedLeasePtr));
}

TEST(GpuPresentSurfaceCoreTest, FrameSubmitPresentsRequestedLeaseAndUpdatesSlotRetireState) {
    RuntimeHarness harness{};
    auto frame = harness.MakeOwnedFrame();
    auto readyToPresent = std::make_unique<FakeSwapChainSyncObject>();

    GpuPresentSurface surface{};
    harness.Attach(surface);
    auto* fakeSwapChain = dynamic_cast<FakeSwapChain*>(surface._swapchain.get());
    ASSERT_NE(fakeSwapChain, nullptr);

    auto lease = std::make_unique<GpuSurfaceLease>();
    lease->_surface = &surface;
    lease->_backBuffer = fakeSwapChain->GetBackBuffer(0);
    lease->_readyToPresent = readyToPresent.get();
    lease->_frameSlotIndex = 0;
    auto* leasePtr = lease.get();
    frame->_surfaceLeases.push_back(std::move(lease));

    ASSERT_TRUE(frame->Present(*leasePtr));

    auto graphicsCmd = frame->CreateGraphicsCommandBuffer().Release();
    ASSERT_NE(graphicsCmd.get(), nullptr);
    ASSERT_TRUE(frame->AddGraphicsCommandBuffer(std::move(graphicsCmd)));

    auto task = harness.Runtime.Submit(std::move(frame));

    EXPECT_TRUE(task.IsValid());
    EXPECT_EQ(fakeSwapChain->PresentCallCount, 1u);
    ASSERT_LT(0u, surface._slotRetireStates.size());
    EXPECT_NE(surface._slotRetireStates[0], nullptr);
}

TEST(GpuPresentSurfaceCoreTest, TryAcquireSurfaceUnavailableStatusNeedsRenderAcquireState) {
    GTEST_SKIP() << "render::AcquireResult 尚未状态化，Suboptimal / OutOfDate 覆盖待后续启用";
}

}  // namespace
