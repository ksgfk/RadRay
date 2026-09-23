#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_runtime_test_support.h"

#include <radray/runtime/gpu_system.h>

namespace radray {
namespace {

unique_ptr<GpuSystem> CreateGpuSystem(render::RenderBackend backend, bool profiler, RuntimeStartupResult& startup, bool validation = true) {
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor desc{
        .VulkanInstance = {.IsEnableDebugLayer = validation, .IsEnableSynchronizationValidation = validation},
        .DXGIFactory = {.IsEnableDebugLayer = validation},
        .FlightDataCount = 2,
        .EnableFrameProfiler = profiler};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor device;
        device.Queues = std::span{&queue, 1};
        desc.Device = device;
    }
    return GpuSystem::TryCreate(desc, startup);
}

void BufferBarrier(render::CommandBuffer* commands, render::Buffer* buffer, render::BufferStates before, render::BufferStates after) {
    const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = buffer, .Before = before, .After = after};
    commands->ResourceBarrier(std::span{&barrier, 1});
}

void RunCommandBatches(render::RenderBackend backend, bool profiler) {
    test::RuntimeLogCapture logs;
    RuntimeStartupResult startup;
    auto gpu = CreateGpuSystem(backend, profiler, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    auto* device = gpu->GetDevice();
    const bool vulkan = backend == render::RenderBackend::Vulkan;
    vector<render::CommandBuffer*> previous[2];
    auto signaled = device->CreateFence().Unwrap();
    for (uint32_t round = 0; round < 4; ++round) {
        const uint32_t flight = round % 2;
        gpu->BeginUpdateForFlight(flight);
        gpu->BeginFrameTiming(flight);
        auto context = gpu->BeginFrameRecord(flight, {}, {}, false);
        auto upload = device->CreateBuffer({.Size = 8, .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite}).Unwrap();
        auto work = device->CreateBuffer({.Size = 4, .Memory = render::MemoryType::Device, .Usage = render::BufferUse::CopySource | render::BufferUse::CopyDestination}).Unwrap();
        auto readback = device->CreateBuffer({.Size = 8, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        const uint32_t source[]{17 + round, 29 + round};
        {
            ScopedBufferMap map{upload.get(), {0, 8}};
            ASSERT_TRUE(map);
            std::memcpy(map.Data(), source, sizeof(source));
            upload->FlushMappedRange({0, 8});
        }
        auto* second = context.AllocateCommandBuffer();
        auto* first = context.AllocateCommandBuffer();
        auto* third = context.AllocateCommandBuffer();
        EXPECT_NE(first, second);
        EXPECT_NE(second, third);
        if (!previous[flight].empty()) {
            EXPECT_EQ(second, previous[flight][0]);
            EXPECT_EQ(first, previous[flight][1]);
            EXPECT_EQ(third, previous[flight][2]);
        }
        previous[flight] = {second, first, third};
        if (vulkan) {
            BufferBarrier(first, upload.get(), render::BufferState::HostWrite, render::BufferState::CopySource);
            BufferBarrier(first, readback.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
        }
        BufferBarrier(first, work.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
        first->CopyBufferToBuffer(work.get(), 0, upload.get(), 0, 4);
        BufferBarrier(second, work.get(), vulkan ? render::BufferState::CopyDestination : render::BufferState::Common, render::BufferState::CopySource);
        second->CopyBufferToBuffer(readback.get(), 0, work.get(), 0, 4);
        BufferBarrier(third, work.get(), render::BufferState::CopySource, render::BufferState::CopyDestination);
        third->CopyBufferToBuffer(work.get(), 0, upload.get(), 4, 4);
        BufferBarrier(third, work.get(), render::BufferState::CopyDestination, render::BufferState::CopySource);
        third->CopyBufferToBuffer(readback.get(), 4, work.get(), 0, 4);
        if (vulkan) BufferBarrier(third, readback.get(), render::BufferState::CopyDestination, render::BufferState::HostRead);
        {
            render::CommandBuffer* buffers[]{first};
            context.ReturnCommandBuffers({.CmdBuffers = buffers});
            buffers[0] = nullptr;
        }
        {
            render::CommandBuffer* buffers[]{second, third};
            render::Fence* signals[]{signaled.get()};
            uint64_t values[]{round + 1ull};
            context.ReturnCommandBuffers({.CmdBuffers = buffers, .SignalFences = signals, .SignalValues = values});
            buffers[0] = nullptr;
            signals[0] = nullptr;
            values[0] = 0;
        }
        auto* fourth = context.AllocateCommandBuffer();
        EXPECT_NE(fourth, first);
        EXPECT_NE(fourth, second);
        EXPECT_NE(fourth, third);
        context.ReturnCommandBuffers({.CmdBuffers = std::span{&fourth, 1}});
        context.SubmitFrame();
        const auto submitted = gpu->GetFlightGpuSignal(flight);
        EXPECT_TRUE(submitted.IsValid());
        gpu->EndFrameRecordAndSubmit(flight);
        EXPECT_EQ(gpu->GetFlightGpuSignal(flight).Value, submitted.Value);
        EXPECT_TRUE(gpu->CompleteFlightIfReady(flight, true));
        EXPECT_FALSE(gpu->GetFlightGpuSignal(flight).IsValid());
        gpu->ReleaseFrameResourcesGT({.FlightIndex = flight, .FrameSerial = context.FrameSerial()});
        EXPECT_GE(signaled->GetCompletedValue(), round + 1u);
        ScopedBufferMap map{readback.get(), {0, 8}};
        ASSERT_TRUE(map);
        readback->InvalidateMappedRange({0, 8});
        uint32_t actual[2]{};
        std::memcpy(actual, map.Data(), sizeof(actual));
        EXPECT_EQ(actual[0], source[0]);
        EXPECT_EQ(actual[1], source[1]);
    }
    // Empty frames still produce a completion fence, with or without profiling.
    auto empty = gpu->BeginFrameRecord(0, {}, {}, false);
    empty.SubmitFrame();
    EXPECT_TRUE(gpu->GetFlightGpuSignal(0).IsValid());
    EXPECT_TRUE(gpu->CompleteFlightIfReady(0, true));
    gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = empty.FrameSerial()});
    gpu->WaitAndRetireFlights();
    EXPECT_GE(gpu->GetLastGpuTimeMs(), 0.0f);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(GpuSystemTest, D3D12OrderedBatches) { RunCommandBatches(render::RenderBackend::D3D12, false); }
TEST(GpuSystemTest, D3D12OrderedBatchesWithProfiler) { RunCommandBatches(render::RenderBackend::D3D12, true); }
TEST(GpuSystemTest, VulkanOrderedBatches) { RunCommandBatches(render::RenderBackend::Vulkan, false); }
TEST(GpuSystemTest, VulkanOrderedBatchesWithProfiler) { RunCommandBatches(render::RenderBackend::Vulkan, true); }

bool ReleaseFence(render::Fence* fence, render::RenderBackend backend, uint64_t value) {
#if defined(RADRAY_ENABLE_D3D12)
    if (backend == render::RenderBackend::D3D12) {
        return SUCCEEDED(static_cast<render::d3d12::FenceD3D12*>(fence)->_fence->Signal(value));
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (backend == render::RenderBackend::Vulkan) {
        auto* native = static_cast<render::vulkan::FenceVulkan*>(fence);
        const VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, native->_fence->_semaphore, value};
        return native->_device->_ftb.vkSignalSemaphore(native->_device->_device, &signal) == VK_SUCCESS;
    }
#endif
    return false;
}

task<void> ObserveFlightCompletion(GpuSystem* gpu, bool& resumed) {
    co_await gpu->Wait();
    resumed = true;
}

void RunFinalFence(render::RenderBackend backend) {
    // Native host-signaled waits avoid validation-layer semaphore tracking.
    RuntimeStartupResult startup;
    auto gpu = CreateGpuSystem(backend, false, startup, false);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    bool resumed = false;
    TaskScope waiting;
    waiting.Spawn(ObserveFlightCompletion(gpu.get(), resumed));
    auto gate = gpu->GetDevice()->CreateFence().Unwrap();
    auto intermediate = gpu->GetDevice()->CreateFence().Unwrap();
    auto context = gpu->BeginFrameRecord(0, {}, {}, false);
    render::Fence* signals[]{intermediate.get()};
    render::Fence* waits[]{gate.get()};
    uint64_t values[]{1};
    context.ReturnCommandBuffers({.SignalFences = signals, .SignalValues = values});
    context.ReturnCommandBuffers({.WaitFences = waits, .WaitValues = values});
    waits[0] = nullptr;
    values[0] = 0;
    context.SubmitFrame();
    intermediate->Wait(1);
    EXPECT_FALSE(gpu->CompleteFlightIfReady(0, false));
    EXPECT_LT(gpu->GetFlightGpuSignal(0).Fence->GetCompletedValue(), gpu->GetFlightGpuSignal(0).Value);
    gpu->PumpWaitFrame(0);
    EXPECT_FALSE(resumed);
    EXPECT_TRUE(ReleaseFence(gate.get(), backend, 1));
    EXPECT_TRUE(gpu->CompleteFlightIfReady(0, true));
    gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
    gpu->PumpWaitFrame(0);
    EXPECT_TRUE(resumed);
    gpu->WaitAndRetireFlights();
}

TEST(GpuSystemTest, D3D12OnlyFinalFenceRetiresFlight) { RunFinalFence(render::RenderBackend::D3D12); }
TEST(GpuSystemTest, VulkanOnlyFinalFenceRetiresFlight) { RunFinalFence(render::RenderBackend::Vulkan); }

class GpuSystemDeathTest : public testing::Test {
protected:
    void SetUp() override {
        RuntimeStartupResult startup;
        Gpu = CreateGpuSystem(render::RenderBackend::D3D12, false, startup, false);
        if (test::CanSkipRuntimeStartup(render::RenderBackend::D3D12, startup)) GTEST_SKIP() << startup.Reason;
        ASSERT_NE(Gpu, nullptr) << startup.Reason;
    }
    unique_ptr<GpuSystem> Gpu;
};

TEST_F(GpuSystemDeathTest, RejectsInvalidCommandReturnsAndUnsealedFrames) {
    auto context = Gpu->BeginFrameRecord(0, {}, {}, false);
    auto* commands = context.AllocateCommandBuffer();
    auto foreign = Gpu->GetDevice()->CreateCommandBuffer(Gpu->GetMainQueue()).Unwrap();
    render::CommandBuffer* external[]{foreign.get()};
    EXPECT_DEATH(context.ReturnCommandBuffers({.CmdBuffers = external}), "");
    render::CommandBuffer* duplicate[]{commands, commands};
    EXPECT_DEATH(context.ReturnCommandBuffers({.CmdBuffers = duplicate}), "");
    EXPECT_DEATH(context.SubmitFrame(), "");
    EXPECT_DEATH(Gpu->BeginFrameRecord(0, {}, {}, false), "");
    render::SwapChainSyncObject* sync[]{nullptr};
    EXPECT_DEATH(context.ReturnCommandBuffers({.WaitToExecute = sync}), "");
    EXPECT_DEATH(context.ReturnCommandBuffers({.ReadyToPresent = sync}), "");
    render::Fence* fences[]{nullptr};
    uint64_t values[]{1};
    EXPECT_DEATH(context.ReturnCommandBuffers({.SignalFences = fences}), "");
    EXPECT_DEATH(context.ReturnCommandBuffers({.WaitFences = fences}), "");
    EXPECT_DEATH(context.ReturnCommandBuffers({.SignalFences = fences, .SignalValues = values}), "");
    EXPECT_DEATH(context.ReturnCommandBuffers({.WaitFences = fences, .WaitValues = values}), "");
    auto other = Gpu->BeginFrameRecord(1, {}, {}, false);
    EXPECT_DEATH(other.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}), "");
    context.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    EXPECT_DEATH(context.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}), "");
    context.SubmitFrame();
    EXPECT_DEATH(context.AllocateCommandBuffer(), "");
    EXPECT_DEATH(context.ReturnCommandBuffers({}), "");
    EXPECT_DEATH(context.AcquireWindow(nullptr), "");
    EXPECT_DEATH(context.SubmitFrame(), "");
    EXPECT_DEATH(Gpu->BeginFrameRecord(0, {}, {}, false), "");
    EXPECT_TRUE(Gpu->CompleteFlightIfReady(0, true));
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
    auto next = Gpu->BeginFrameRecord(0, {}, {}, false);
    EXPECT_DEATH(context.AllocateCommandBuffer(), "");
    next.SubmitFrame();
    other.SubmitFrame();
    Gpu->WaitAndRetireFlights();
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = next.FrameSerial()});
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 1, .FrameSerial = other.FrameSerial()});
}

TEST_F(GpuSystemDeathTest, EnforcesFrameResourceRetirementBeforeSlotReuse) {
    auto first = Gpu->BeginFrameRecord(0, {}, {}, false);
    const FlightCompletion firstCompletion{.FlightIndex = 0, .FrameSerial = first.FrameSerial()};
    EXPECT_DEATH(Gpu->ReleaseFrameResourcesGT(firstCompletion), "");
    first.SubmitFrame();
    ASSERT_TRUE(Gpu->CompleteFlightIfReady(0, true));
    // An empty frame still owns its serial until GT consumes the completion.
    EXPECT_DEATH(Gpu->BeginUpdateForFlight(0), "");
    EXPECT_DEATH(Gpu->BeginFrameRecord(0, {}, {}, false), "");
    EXPECT_DEATH(Gpu->RetainForFrameGT(0, 42u), "");
    EXPECT_DEATH(Gpu->AbandonUnpublishedResourcesTerminalGT(), "");
    Gpu->ReleaseFrameResourcesGT(firstCompletion);
    EXPECT_EQ(first.FrameSerial(), firstCompletion.FrameSerial);
    EXPECT_DEATH(Gpu->ReleaseFrameResourcesGT(firstCompletion), "");

    Gpu->BeginUpdateForFlight(0);
    auto payload = make_shared<uint32_t>(42);
    weak_ptr<uint32_t> retained = payload;
    Gpu->RetainForFrameGT(0, std::move(payload));
    EXPECT_DEATH(Gpu->ReleaseFrameResourcesGT(firstCompletion), "");
    auto next = Gpu->BeginFrameRecord(0, {}, {}, false);
    EXPECT_GT(next.FrameSerial(), firstCompletion.FrameSerial);
    EXPECT_FALSE(retained.expired());
    EXPECT_DEATH(Gpu->ReleaseFrameResourcesGT(firstCompletion), "");
    next.SubmitFrame();
    ASSERT_TRUE(Gpu->CompleteFlightIfReady(0, true));
    EXPECT_FALSE(retained.expired());
    EXPECT_DEATH(Gpu->ReleaseFrameResourcesGT(firstCompletion), "");
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = next.FrameSerial()});
    EXPECT_TRUE(retained.expired());

    Gpu->BeginUpdateForFlight(0);
    payload = make_shared<uint32_t>(84);
    retained = payload;
    Gpu->RetainForFrameGT(0, std::move(payload));
    EXPECT_FALSE(retained.expired());
    Gpu->AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(retained.expired());
    EXPECT_DEATH(Gpu->RetainForFrameGT(0, 84u), "");
}

}  // namespace
}  // namespace radray
