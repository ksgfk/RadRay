#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_runtime_test_support.h"

#include <radray/basic_math.h>
#include <radray/runtime/frame_timeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/scope_guard.h>
#include <radray/runtime/render_framework/scene_manager.h>
#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray {
namespace {

unique_ptr<GpuSystem> CreateGpuSystem(render::RenderBackend backend, bool profiler, FrameTimeline& timeline, RuntimeStartupResult& startup, bool validation = true, bool rhiValidation = false) {
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor desc{
        .VulkanInstance = {.IsEnableDebugLayer = validation, .IsEnableSynchronizationValidation = validation},
        .DXGIFactory = {.IsEnableDebugLayer = validation},
        .RhiValidation = {.IsEnabled = rhiValidation},
        .FlightDataCount = timeline.GetFlightDataCount(),
        .EnableFrameProfiler = profiler};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor device;
        device.Queues = std::span{&queue, 1};
        desc.Device = device;
    }
    return GpuSystem::TryCreate(desc, timeline, startup);
}

void GpuSystemBufferBarrier(render::CommandBuffer* commands, render::Buffer* buffer, render::BufferStates before, render::BufferStates after) {
    const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = buffer, .Before = before, .After = after};
    commands->ResourceBarrier(std::span{&barrier, 1});
}

void RunCommandBatches(render::RenderBackend backend, bool profiler, bool rhiValidation = false) {
    test::RuntimeLogCapture logs;
    RuntimeStartupResult startup;
    FrameTimeline timeline{2};
    auto gpu = CreateGpuSystem(backend, profiler, timeline, startup, true, rhiValidation);
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
            GpuSystemBufferBarrier(first, upload.get(), render::BufferState::HostWrite, render::BufferState::CopySource);
            GpuSystemBufferBarrier(first, readback.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
        }
        GpuSystemBufferBarrier(first, work.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
        first->CopyBufferToBuffer(work.get(), 0, upload.get(), 0, 4);
        GpuSystemBufferBarrier(second, work.get(), vulkan ? render::BufferState::CopyDestination : render::BufferState::Common, render::BufferState::CopySource);
        second->CopyBufferToBuffer(readback.get(), 0, work.get(), 0, 4);
        GpuSystemBufferBarrier(third, work.get(), render::BufferState::CopySource, render::BufferState::CopyDestination);
        third->CopyBufferToBuffer(work.get(), 0, upload.get(), 4, 4);
        GpuSystemBufferBarrier(third, work.get(), render::BufferState::CopyDestination, render::BufferState::CopySource);
        third->CopyBufferToBuffer(readback.get(), 4, work.get(), 0, 4);
        if (vulkan) GpuSystemBufferBarrier(third, readback.get(), render::BufferState::CopyDestination, render::BufferState::HostRead);
        auto& allocator = context.GetCmdAllocator();
        allocator.Return(std::span{&third, 1});
        allocator.Return(std::span{&second, 1});
        allocator.Return(std::span{&first, 1});
        {
            render::CommandBuffer* buffers[]{first};
            context.RegisterClosedCommandBuffers({.CmdBuffers = buffers});
            buffers[0] = nullptr;
        }
        {
            render::CommandBuffer* buffers[]{second, third};
            render::Fence* signals[]{signaled.get()};
            uint64_t values[]{round + 1ull};
            context.RegisterClosedCommandBuffers({.CmdBuffers = buffers, .SignalFences = signals, .SignalValues = values});
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
        {
            FlightCompletion notification;
            while (timeline.TryReadCompletion(notification)) {
            }
        }
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
    {
        FlightCompletion notification;
        while (timeline.TryReadCompletion(notification)) {
        }
    }
    gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = empty.FrameSerial()});
    gpu->WaitAndRetireFlights();
    EXPECT_GE(gpu->GetLastGpuTimeMs(), 0.0f);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(GpuSystemTest, D3D12OrderedBatches) { RunCommandBatches(render::RenderBackend::D3D12, false); }
TEST(GpuSystemTest, D3D12OrderedBatchesWithProfiler) { RunCommandBatches(render::RenderBackend::D3D12, true); }
TEST(GpuSystemTest, VulkanOrderedBatches) { RunCommandBatches(render::RenderBackend::Vulkan, false); }
TEST(GpuSystemTest, VulkanOrderedBatchesWithProfiler) { RunCommandBatches(render::RenderBackend::Vulkan, true); }
TEST(GpuSystemTest, D3D12OrderedBatchesWithRhiValidation) { RunCommandBatches(render::RenderBackend::D3D12, true, true); }
TEST(GpuSystemTest, VulkanOrderedBatchesWithRhiValidation) { RunCommandBatches(render::RenderBackend::Vulkan, true, true); }

void RunSubmittedPrefix(render::RenderBackend backend) {
    test::RuntimeLogCapture logs;
    RuntimeStartupResult startup;
    FrameTimeline timeline{3};
    auto gpu = CreateGpuSystem(backend, false, timeline, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    // A domain can be reserved before a different slot submits. Serial identifies, not orders, submissions.
    gpu->BeginUpdateForFlight(1);
    gpu->BeginUpdateForFlight(0);
    gpu->BeginUpdateForFlight(2);
    for (uint32_t flight : {0u, 1u}) {
        gpu->BeginFrameTiming(flight);
        gpu->BeginFrameRecord(flight, {}, {}, false);
        gpu->EndFrameRecordAndSubmit(flight);
    }
    gpu->BeginFrameTiming(2);
    gpu->BeginFrameRecord(2, {}, {}, false);
    const auto last = gpu->GetFlightGpuSignal(1);
    last.Fence->Wait(last.Value);
    const SubmittedFlight submitted[]{{0, timeline.GetFrameSerial(0)}, {1, timeline.GetFrameSerial(1)}};
    EXPECT_GT(submitted[0].FrameSerial, submitted[1].FrameSerial);
    EXPECT_EQ(gpu->RetireSubmittedFlights(submitted), 2u);
    EXPECT_EQ(gpu->RetireSubmittedFlights({}), 0u);
    FlightCompletion completion;
    for (const auto& expected : submitted) {
        EXPECT_TRUE(timeline.TryReadCompletion(completion));
        EXPECT_EQ(completion.FlightIndex, expected.FlightIndex);
        EXPECT_EQ(completion.FrameSerial, expected.FrameSerial);
        gpu->ReleaseFrameResourcesGT(completion);
    }
    EXPECT_FALSE(timeline.TryReadCompletion(completion));
    // Slot 2 remained recording throughout retirement of the handed-off prefix.
    gpu->EndFrameRecordAndSubmit(2);
    gpu->WaitMainQueueIdle();
    const SubmittedFlight tail{2, timeline.GetFrameSerial(2)};
    EXPECT_EQ(gpu->RetireSubmittedFlights(std::span{&tail, 1}), 1u);
    EXPECT_TRUE(timeline.TryReadCompletion(completion));
    gpu->ReleaseFrameResourcesGT(completion);
    timeline.BeginStopping();
    timeline.CleanupCompletedFlights();
    gpu->AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(GpuSystemTest, D3D12RetireUsesOnlySubmittedPrefixAndFinalSignalOrder) { RunSubmittedPrefix(render::RenderBackend::D3D12); }
TEST(GpuSystemTest, VulkanRetireUsesOnlySubmittedPrefixAndFinalSignalOrder) { RunSubmittedPrefix(render::RenderBackend::Vulkan); }

Nullable<render::CommandAllocator*> CommandStorage(render::CommandBuffer* commands, render::RenderBackend backend) {
#if defined(RADRAY_ENABLE_D3D12)
    if (backend == render::RenderBackend::D3D12) return static_cast<render::d3d12::CmdListD3D12*>(commands)->_allocator;
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (backend == render::RenderBackend::Vulkan) return static_cast<render::vulkan::CommandBufferVulkan*>(commands)->_cmdPool;
#endif
    return nullptr;
}

class GpuSystemAllocatorTest : public testing::TestWithParam<render::RenderBackend> {};

TEST_P(GpuSystemAllocatorTest, PoolsRecordingGroupsAcrossEpochPatternsAndDiscard) {
    const auto backend = GetParam();
    test::RuntimeLogCapture logs;
    RuntimeStartupResult startup;
    FrameTimeline timeline{2};
    auto gpu = CreateGpuSystem(backend, false, timeline, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    auto* device = gpu->GetDevice();
    const bool vulkan = backend == render::RenderBackend::Vulkan;
    const array<uint32_t, 5> patterns{1, 2, 4, 1, 4};
    array<array<vector<render::CommandBuffer*>, 5>, 2> previous;
    array<vector<render::CommandAllocator*>, 2> knownStorages;
    array<uint32_t, 2> peaks{};
    for (uint32_t round = 0; round < 20; ++round) {
        const uint32_t flight = round % 2;
        const uint32_t pattern = (round / 2) % patterns.size();
        const uint32_t overlap = patterns[pattern];
        SCOPED_TRACE(fmt::format("round={} flight={} overlap={}", round, flight, overlap));
        gpu->BeginUpdateForFlight(flight);
        auto frame = gpu->BeginFrameRecord(flight, {}, {}, false);
        auto& allocator = frame.GetCmdAllocator();
        vector<render::CommandBuffer*> allocated;
        vector<render::CommandAllocator*> usedStorages;
        const auto allocate = [&] {
            auto* commands = allocator.Allocate();
            EXPECT_EQ(std::find(allocated.begin(), allocated.end(), commands), allocated.end());
            allocated.push_back(commands);
            auto* storage = CommandStorage(commands, backend).Unwrap();
            if (std::find(usedStorages.begin(), usedStorages.end(), storage) == usedStorages.end()) usedStorages.push_back(storage);
            auto& known = knownStorages[flight];
            if (std::find(known.begin(), known.end(), storage) == known.end()) known.push_back(storage);
            return commands;
        };
        auto* discardedOpen = allocate();
        allocator.Discard(std::span{&discardedOpen, 1});
        auto* discardedClosed = allocate();
        EXPECT_EQ(CommandStorage(discardedClosed, backend), CommandStorage(discardedOpen, backend));
        allocator.Return(std::span{&discardedClosed, 1});
        allocator.Discard(std::span{&discardedClosed, 1});
        auto upload = device->CreateBuffer({32, render::MemoryType::Upload, render::BufferUse::CopySource | render::BufferUse::MapWrite}).Unwrap();
        auto readback = device->CreateBuffer({32, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        array<uint32_t, 8> expected;
        for (uint32_t i = 0; i < expected.size(); ++i) expected[i] = round * 100 + i;
        {
            ScopedBufferMap map{upload.get(), {0, 32}};
            ASSERT_TRUE(map);
            std::memcpy(map.Data(), expected.data(), 32);
            upload->FlushMappedRange({0, 32});
        }
        for (uint32_t begin = 0; begin < expected.size(); begin += overlap) {
            vector<render::CommandBuffer*> batch;
            for (uint32_t i = begin; i < begin + overlap; ++i) {
                auto* commands = allocate();
                if (vulkan && i == 0) {
                    GpuSystemBufferBarrier(commands, upload.get(), render::BufferState::HostWrite, render::BufferState::CopySource);
                    GpuSystemBufferBarrier(commands, readback.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
                }
                commands->CopyBufferToBuffer(readback.get(), i * 4, upload.get(), i * 4, 4);
                if (vulkan && i + 1 == expected.size()) GpuSystemBufferBarrier(commands, readback.get(), render::BufferState::CopyDestination, render::BufferState::HostRead);
                batch.push_back(commands);
            }
            for (auto it = batch.rbegin(); it != batch.rend(); ++it) allocator.Return(std::span{&*it, 1});
            frame.RegisterClosedCommandBuffers({.CmdBuffers = batch});
        }
        EXPECT_EQ(usedStorages.size(), vulkan ? 1u : overlap);
        peaks[flight] = std::max(peaks[flight], overlap);
        EXPECT_EQ(knownStorages[flight].size(), vulkan ? 1u : peaks[flight]);
        if (round < 10)
            previous[flight][pattern] = allocated;
        else
            EXPECT_EQ(allocated, previous[flight][pattern]);
        frame.SubmitFrame();
        ASSERT_TRUE(gpu->CompleteFlightIfReady(flight, true));
        FlightCompletion notification;
        while (timeline.TryReadCompletion(notification)) {
        }
        gpu->ReleaseFrameResourcesGT({.FlightIndex = flight, .FrameSerial = frame.FrameSerial()});
        ScopedBufferMap map{readback.get(), {0, 32}};
        ASSERT_TRUE(map);
        readback->InvalidateMappedRange({0, 32});
        array<uint32_t, 8> actual;
        std::memcpy(actual.data(), map.Data(), 32);
        EXPECT_EQ(actual, expected);
    }
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

INSTANTIATE_TEST_SUITE_P(Backends, GpuSystemAllocatorTest, testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan));

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

task<void> ObserveFlightCompletion(FrameTimeline& timeline, bool& resumed) {
    co_await timeline.Wait();
    resumed = true;
}

void RunFinalFence(render::RenderBackend backend) {
    // Native host-signaled waits avoid validation-layer semaphore tracking.
    RuntimeStartupResult startup;
    FrameTimeline timeline{2};
    auto gpu = CreateGpuSystem(backend, false, timeline, startup, false);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    bool resumed = false;
    TaskScope waiting;
    waiting.Spawn(ObserveFlightCompletion(timeline, resumed));
    auto gate = gpu->GetDevice()->CreateFence().Unwrap();
    auto intermediate = gpu->GetDevice()->CreateFence().Unwrap();
    auto context = gpu->BeginFrameRecord(0, {}, {}, false);
    struct OwnerProbe {
        explicit OwnerProbe(uint32_t& retired) : Retired(retired) {}
        ~OwnerProbe() { ++Retired; }
        uint32_t& Retired;
    };
    uint32_t retired = 0;
    context.RetainGpuForFrameRT(make_unique<OwnerProbe>(retired));
    auto& resources = context.GetGpuFrameResources();
    const render::TextureDescriptor desc{.Dim = render::TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Usage = render::TextureUse::RenderTarget};
    auto target = resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget);
    ASSERT_TRUE(target);
    auto* command = context.AllocateCommandBuffer();
    auto* extra = context.AllocateCommandBuffer();
    const render::RenderPassColorAttachment color{target->View, render::LoadAction::Clear, render::StoreAction::Store};
    const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{target->Target, target->InitialState, render::TextureState::RenderTarget};
    command->ResourceBarrier(std::span{&before, 1});
    auto encoder = command->BeginRenderPass({std::span{&color, 1}, {}, 8, 8, 1});
    ASSERT_TRUE(encoder);
    command->EndRenderPass(encoder.Release());
    render::Fence* signals[]{intermediate.get()};
    render::Fence* waits[]{gate.get()};
    uint64_t values[]{1};
    render::CommandBuffer* batch[]{command, extra};
    context.ReturnCommandBuffers({.CmdBuffers = batch, .SignalFences = signals, .SignalValues = values});
    context.ReturnCommandBuffers({.WaitFences = waits, .WaitValues = values});
    waits[0] = nullptr;
    values[0] = 0;
    context.SubmitFrame();
    intermediate->Wait(1);
    EXPECT_FALSE(gpu->CompleteFlightIfReady(0, false));
    EXPECT_EQ(retired, 0u);
    EXPECT_FALSE(resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget));
#ifdef RADRAY_ENABLE_VULKAN
    if (backend == render::RenderBackend::Vulkan) {
        auto* native = render::vulkan::CastVkObject(command);
        ASSERT_EQ(native->_framebufferPins.size(), 1u);
        EXPECT_EQ(native->_framebufferPins.front()->Uses, 1u);
        EXPECT_EQ(native->_passPins.front()->Uses, 1u);
    }
#endif
    EXPECT_LT(gpu->GetFlightGpuSignal(0).Fence->GetCompletedValue(), gpu->GetFlightGpuSignal(0).Value);
    timeline.PumpWaitFrame(0);
    EXPECT_FALSE(resumed);
    EXPECT_TRUE(ReleaseFence(gate.get(), backend, 1));
    EXPECT_TRUE(gpu->CompleteFlightIfReady(0, true));
#ifdef RADRAY_ENABLE_VULKAN
    if (backend == render::RenderBackend::Vulkan) {
        auto* native = render::vulkan::CastVkObject(command);
        EXPECT_TRUE(native->_framebufferPins.empty());
        EXPECT_TRUE(native->_passPins.empty());
    }
#endif
    {
        FlightCompletion notification;
        while (timeline.TryReadCompletion(notification)) {
        }
    }
    gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
    EXPECT_EQ(retired, 1u);
    timeline.PumpWaitFrame(0);
    EXPECT_TRUE(resumed);
    gpu->WaitAndRetireFlights();
}

TEST(GpuSystemTest, D3D12OnlyFinalFenceRetiresFlight) { RunFinalFence(render::RenderBackend::D3D12); }
TEST(GpuSystemTest, VulkanOnlyFinalFenceRetiresFlight) { RunFinalFence(render::RenderBackend::Vulkan); }

void RunIndependentHosts(render::RenderBackend backend) {
    render::test::DeviceContext external;
    if (!render::test::TryCreateDevice(backend, external)) GTEST_SKIP() << external.Reason;
    auto texture = external.Device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 8, .Height = 8, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Usage = render::TextureUse::RenderTarget}).Unwrap();
    auto view = external.Device->CreateTextureView({texture.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Unwrap();
    auto gate = external.Device->CreateFence().Unwrap();
    FrameTimeline timelineA{1}, timelineB{1};
    RuntimeStartupResult startup;
    const GpuSystemAttachmentDescriptor desc{.Device = external.Device, .FlightDataCount = 1};
    auto hostA = GpuSystem::TryAttachDevice(desc, timelineA, startup);
    ASSERT_NE(hostA, nullptr) << startup.Reason;
    auto hostB = GpuSystem::TryAttachDevice(desc, timelineB, startup);
    ASSERT_NE(hostB, nullptr) << startup.Reason;
    EXPECT_NE(hostA->GetMainFence(), hostB->GetMainFence());
    auto contextA = hostA->BeginFrameRecord(0, {}, {}, false);
    auto contextB = hostB->BeginFrameRecord(0, {}, {}, false);
    EXPECT_EQ(contextA.FrameSerial(), contextB.FrameSerial());
    const auto record = [&](AppFrameContext& context, render::TextureStates before) {
        auto* command = context.AllocateCommandBuffer();
        const render::RenderPassColorAttachment color{view.get(), render::LoadAction::Clear, render::StoreAction::Store};
        const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{texture.get(), before, render::TextureState::RenderTarget};
        command->ResourceBarrier(std::span{&barrier, 1});
        auto encoder = command->BeginRenderPass({std::span{&color, 1}, {}, 8, 8, 1});
        EXPECT_TRUE(encoder);
        if (encoder) command->EndRenderPass(encoder.Release());
        context.ReturnCommandBuffers({.CmdBuffers = std::span{&command, 1}});
        return command;
    };
    auto* commandA = record(contextA, render::TextureState::Undefined);
    contextA.SubmitFrame();
    render::Fence* wait = gate.get();
    uint64_t value = 1;
    contextB.ReturnCommandBuffers({.WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
    auto* commandB = record(contextB, render::TextureState::RenderTarget);
    contextB.SubmitFrame();
    bool released = false;
    const auto cleanup = MakeScopeGuard([&]() {
        if (!released) ReleaseFence(gate.get(), backend, 1);
        for (auto* host : {hostA.get(), hostB.get()}) {
            host->CompleteFlightIfReady(0, true);
            FlightCompletion completion;
            while (host->GetFrameTimeline().TryReadCompletion(completion)) host->ReleaseFrameResourcesGT(completion);
        }
    });
#ifdef RADRAY_ENABLE_VULKAN
    if (backend == render::RenderBackend::Vulkan) {
        auto* a = render::vulkan::CastVkObject(commandA);
        auto* b = render::vulkan::CastVkObject(commandB);
        ASSERT_EQ(a->_framebufferPins.size(), 1u);
        ASSERT_EQ(b->_framebufferPins.size(), 1u);
        EXPECT_EQ(a->_framebufferPins.front(), b->_framebufferPins.front());
        EXPECT_EQ(a->_passPins.front(), b->_passPins.front());
        EXPECT_EQ(b->_framebufferPins.front()->Uses, 2u);
        EXPECT_EQ(b->_passPins.front()->Uses, 2u);
    }
#endif
    EXPECT_TRUE(hostA->CompleteFlightIfReady(0, true));
    EXPECT_TRUE(timelineA.IsCompleted(0, contextA.FrameSerial()));
    EXPECT_FALSE(timelineB.IsCompleted(0, contextB.FrameSerial()));
    EXPECT_FALSE(hostB->CompleteFlightIfReady(0, false));
#ifdef RADRAY_ENABLE_VULKAN
    if (backend == render::RenderBackend::Vulkan) {
        auto* a = render::vulkan::CastVkObject(commandA);
        auto* b = render::vulkan::CastVkObject(commandB);
        EXPECT_TRUE(a->_framebufferPins.empty());
        EXPECT_TRUE(a->_passPins.empty());
        EXPECT_EQ(b->_framebufferPins.front()->Uses, 1u);
        EXPECT_EQ(b->_passPins.front()->Uses, 1u);
    }
#endif
    released = ReleaseFence(gate.get(), backend, 1);
    EXPECT_TRUE(released);
    EXPECT_TRUE(hostB->CompleteFlightIfReady(0, true));
    EXPECT_TRUE(timelineB.IsCompleted(0, contextB.FrameSerial()));
#ifdef RADRAY_ENABLE_VULKAN
    if (backend == render::RenderBackend::Vulkan) {
        auto* b = render::vulkan::CastVkObject(commandB);
        EXPECT_TRUE(b->_framebufferPins.empty());
        EXPECT_TRUE(b->_passPins.empty());
    }
#endif
}

TEST(GpuSystemAttachment, D3D12HostsShareDeviceAndRetireIndependently) { RunIndependentHosts(render::RenderBackend::D3D12); }
TEST(GpuSystemAttachment, VulkanHostsShareDeviceAndRetireIndependently) { RunIndependentHosts(render::RenderBackend::Vulkan); }

TEST(GpuSystemAttachment, RejectsMissingDeviceAndMismatchedFlightCount) {
    FrameTimeline timeline{2};
    RuntimeStartupResult startup;
    EXPECT_EQ(GpuSystem::TryAttachDevice({}, timeline, startup), nullptr);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::InvalidDescriptor);
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, device)) GTEST_SKIP() << device.Reason;
    EXPECT_EQ(GpuSystem::TryAttachDevice({.Device = device.Device, .FlightDataCount = 1}, timeline, startup), nullptr);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::InvalidDescriptor);
    EXPECT_EQ(GpuSystem::TryAttachDevice({.Device = device.Device, .MainQueueIndex = UINT32_MAX}, timeline, startup), nullptr);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::InitializationFailed);
    EXPECT_NE(device.Device->GetCommandQueue(render::QueueType::Direct), nullptr);
}

class AttachedMesh final : public StaticMesh {
public:
    AttachedMesh(MeshResource cpu, GpuMesh gpu, uint32_t& destroyed)
        : StaticMesh(std::move(cpu), {}, {-1, -1, 0}, {1, 1, 0}, std::move(gpu)), Destroyed(destroyed) {}
    ~AttachedMesh() noexcept override { ++Destroyed; }
    uint32_t& Destroyed;
};

TEST(GpuSystemAttachment, SameThreadSceneAssetAndGpuPhasesRetainOwnersUntilTheCoveringFence) {
    render::test::DeviceContext external;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, external)) GTEST_SKIP() << external.Reason;
    FrameTimeline timeline{2};
    RuntimeStartupResult startup;
    auto gpu = GpuSystem::TryAttachDevice({.Device = external.Device}, timeline, startup);
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    EXPECT_EQ(gpu->GetDevice(), external.Device.get());
    EXPECT_EQ(gpu->GetMainQueue(), external.Queue);
    AssetManager assets;

    SceneManager scenes{2};
    uint32_t destroyed = 0;
    auto source = array<float, 9>{0, 0.8f, 0, -0.8f, -0.8f, 0, 0.8f, -0.8f, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource cpu;
    cpu.Bins.emplace_back(std::as_bytes(std::span{source}));
    cpu.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive primitive;
    primitive.VertexCount = 3;
    primitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    primitive.IndexBuffer = {1, 3, 0, 4};
    cpu.Primitives.push_back(std::move(primitive));
    GpuMesh geometry;
    auto vertexBuffer = render::test::MakeUploadBuffer(*external.Device, std::as_bytes(std::span{source}), render::BufferUse::Vertex | render::BufferUse::CopySource);
    ASSERT_TRUE(vertexBuffer);
    geometry.Buffers.push_back(vertexBuffer.Release());
    geometry.Draws.emplace_back();
    geometry.Draws[0].VertexBuffers.push_back({.Binding = 0, .View = {.Target = geometry.Buffers[0].get(), .Size = sizeof(source)}});
    auto mesh = assets.AddReady<StaticMesh>(AssetId{19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<AttachedMesh>(std::move(cpu), std::move(geometry), destroyed));
    ASSERT_TRUE(mesh.IsReady());
    ASSERT_TRUE(mesh->IsValid());
    const auto scene = scenes.CreateSceneGT();
    auto writer = scenes.GetSceneWriterGT(scene);
    ASSERT_TRUE(writer);
    const auto shape = writer->CreateShape();
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform(0, 3) = 17.25f;
    writer->SetStaticMesh(shape, mesh, transform);
    auto* borrowed = mesh->GetRenderMesh().Buffers[0].get();
    mesh.Reset();
    source.fill(99);
    auto gate = external.Device->CreateFence().Unwrap();
    auto readback = external.Device->CreateBuffer({.Size = 80, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
    array<uint64_t, 2> serials{};
    bool gateReleased = false;
    const auto retire = [&] {
        FlightCompletion completion;
        while (timeline.TryReadCompletion(completion)) {
            scenes.OnFlightCompletedGT(completion);
            gpu->ReleaseFrameResourcesGT(completion);
            serials[completion.FlightIndex] = 0;
        }
    };
    auto cleanup = MakeScopeGuard([&]() noexcept {
        if (!gateReleased) ReleaseFence(gate.get(), render::RenderBackend::D3D12, 1);
        for (uint32_t flight = 0; flight < 2; ++flight)
            if (serials[flight]) {
                gpu->EndFrameRecordAndSubmit(flight);
                gpu->CompleteFlightIfReady(flight, true);
            }
        retire();
        scenes.OnShutdown();
        assets.Pump();
        timeline.CleanupCompletedFlights();
        gpu->AbandonUnpublishedResourcesTerminalGT();
    });
    gpu->BeginUpdateForFlight(0);
    scenes.SealFrameGT(0);
    scenes.PublishFrameGT(0);
    auto frame = gpu->BeginFrameRecord(0, {}, {}, false);
    scenes.ConsumeRenderUpdates(0, frame.FrameSerial());
    serials[0] = frame.FrameSerial();
    auto sampled = scenes.GetSceneRT(scene)->GetStaticMesh(shape);
    ASSERT_TRUE(sampled);
    auto objects = scenes.PrepareSceneGpuRT(scene, frame);
    ASSERT_TRUE(objects);
    render::Fence* waits[]{gate.get()};
    uint64_t values[]{1};
    frame.ReturnCommandBuffers({.WaitFences = waits, .WaitValues = values});
    auto* commands = frame.AllocateCommandBuffer();
    commands->CopyBufferToBuffer(readback.get(), 0, borrowed, 4, 4);
    GpuSystemBufferBarrier(commands, objects->Objects.Target, render::BufferState::ShaderRead, render::BufferState::CopySource);
    commands->CopyBufferToBuffer(readback.get(), 16, objects->Objects.Target, uint64_t{shape.Index} * sizeof(SceneObjectGpuData), sizeof(SceneObjectGpuData));
    GpuSystemBufferBarrier(commands, objects->Objects.Target, render::BufferState::CopySource, render::BufferState::ShaderRead);
    frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    gpu->EndFrameRecordAndSubmit(0);
    const auto covering = gpu->GetFlightGpuSignal(0);
    EXPECT_TRUE(covering.IsValid());
    EXPECT_FALSE(gpu->CompleteFlightIfReady(0, false));
    scenes.DestroySceneGT(scene);
    gpu->BeginUpdateForFlight(1);
    scenes.SealFrameGT(1);
    scenes.PublishFrameGT(1);
    auto removal = gpu->BeginFrameRecord(1, {}, {}, false, false);
    scenes.ConsumeRenderUpdates(1, removal.FrameSerial());
    serials[1] = removal.FrameSerial();
    gpu->EndFrameRecordAndSubmit(1);
    EXPECT_EQ(scenes.GetSceneRT(scene), nullptr);
    assets.Pump();
    EXPECT_EQ(destroyed, 0u);
    EXPECT_FALSE(gpu->CompleteFlightIfReady(1, false));
    const auto replacement = scenes.CreateSceneGT();
    EXPECT_NE(replacement, scene);
    gateReleased = ReleaseFence(gate.get(), render::RenderBackend::D3D12, 1);
    ASSERT_TRUE(gateReleased);
    ASSERT_TRUE(gpu->CompleteFlightIfReady(0, true));
    retire();
    assets.Pump();
    EXPECT_EQ(destroyed, 0u);
    ASSERT_TRUE(gpu->CompleteFlightIfReady(1, true));
    retire();
    assets.Pump();
    EXPECT_EQ(destroyed, 1u);
    EXPECT_EQ(assets.GetAssetCount(), 0u);
    ScopedBufferMap map{readback.get(), {0, 80}};
    ASSERT_TRUE(map);
    readback->InvalidateMappedRange({0, 80});
    float observed{};
    std::memcpy(&observed, map.Data(), 4);
    EXPECT_FLOAT_EQ(observed, 0.8f);
    SceneObjectGpuData observedObject;
    std::memcpy(&observedObject, map.DataAs<byte>() + 16, sizeof(observedObject));
    for (size_t i = 0; i < observedObject.LocalToWorld.size(); ++i) EXPECT_FLOAT_EQ(observedObject.LocalToWorld[i], transform.data()[i]);
    EXPECT_GE(covering.Fence->GetCompletedValue(), covering.Value);
    EXPECT_EQ(external.ValidationErrors.load(), 0u);
}

TEST(GpuSystemAttachment, DestroyingAnAttachedHostLeavesTheExternalDeviceAndQueueUsable) {
    render::test::DeviceContext external;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, external)) GTEST_SKIP() << external.Reason;
    const auto* original = external.Device.get();
    const auto owners = external.Device.use_count();
    {
        FrameTimeline timeline{2};
        RuntimeStartupResult startup;
        auto gpu = GpuSystem::TryAttachDevice({.Device = external.Device}, timeline, startup);
        ASSERT_NE(gpu, nullptr);
        EXPECT_EQ(external.Device.use_count(), owners + 1);
        gpu->AbandonUnpublishedResourcesTerminalGT();
    }
    EXPECT_EQ(external.Device.get(), original);
    EXPECT_EQ(external.Device.use_count(), owners);
    auto fence = external.Device->CreateFence().Unwrap();
    render::Fence* signals[]{fence.get()};
    uint64_t values[]{7};
    external.Queue->Submit({.SignalFences = signals, .SignalValues = values});
    EXPECT_TRUE(fence->WaitFor(7, 10000));
    EXPECT_EQ(fence->GetCompletedValue(), 7u);
}

TEST(GpuSystemAttachment, SharedTextureStorageAndDistinctViewsSurviveSourceReleaseUntilTheRealFence) {
    render::test::DeviceContext external;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, external)) GTEST_SKIP() << external.Reason;
    FrameTimeline timeline{2};
    RuntimeStartupResult startup;
    auto gpu = GpuSystem::TryAttachDevice({.Device = external.Device}, timeline, startup);
    ASSERT_NE(gpu, nullptr);
    AssetManager assets;

    auto created = external.Device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 2, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::Resource | render::TextureUse::CopySource | render::TextureUse::CopyDestination});
    ASSERT_TRUE(created);
    shared_ptr<render::Texture> storage = created.Release();
    weak_ptr<render::Texture> retained = storage;
    auto originalView = external.Device->CreateTextureView({storage.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap();
    auto assetView = external.Device->CreateTextureView(originalView->GetDesc()).Unwrap();
    EXPECT_NE(originalView.get(), assetView.get());
    auto image = assets.AddReady<TextureAsset>(AssetId{20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1},
                                               make_unique<TextureAsset>(external.Device.get(), "shared atlas", storage, std::move(assetView)));
    ASSERT_TRUE(image.IsReady());
    ASSERT_TRUE(image->IsValid());
    EXPECT_EQ(image->GetTexture(), storage.get());
    EXPECT_EQ(image->GetSrv()->GetDesc().Target, storage.get());
    const TextureSubViewDesc sub{.Dim = render::TextureDimension::Dim2D, .Format = render::TextureFormat::RGBA8_UNORM, .Range = {0, 1, 0, 1}};
    auto* cached = image->GetOrCreateSrv(sub);
    ASSERT_NE(cached, nullptr);
    EXPECT_EQ(image->GetOrCreateSrv(sub), cached);
    auto* borrowed = image->GetTexture();
    auto gate = external.Device->CreateFence().Unwrap();
    auto readback = external.Device->CreateBuffer({.Size = external.Device->GetDetail().TextureDataPitchAlignment,
                                                   .Memory = render::MemoryType::ReadBack,
                                                   .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead})
                        .Unwrap();
    bool gateReleased = false;
    uint64_t outstanding = 0;
    auto cleanup = MakeScopeGuard([&]() noexcept {
        if (!gateReleased) ReleaseFence(gate.get(), render::RenderBackend::D3D12, 1);
        if (outstanding) {
            gpu->EndFrameRecordAndSubmit(0);
            gpu->CompleteFlightIfReady(0, true);
            FlightCompletion completion;
            while (timeline.TryReadCompletion(completion)) gpu->ReleaseFrameResourcesGT(completion);
        }
        timeline.CleanupCompletedFlights();
        gpu->AbandonUnpublishedResourcesTerminalGT();
    });
    gpu->BeginUpdateForFlight(0);
    gpu->RetainForFrameGT(0, image);
    auto frame = gpu->BeginFrameRecord(0, {}, {}, false);
    outstanding = frame.FrameSerial();
    render::Fence* waits[]{gate.get()};
    uint64_t values[]{1};
    frame.ReturnCommandBuffers({.WaitFences = waits, .WaitValues = values});
    auto pixels = array<uint32_t, 2>{0xff332211, 0xff665544};
    auto* commands = frame.AllocateCommandBuffer();
    frame.GetUploader().UploadTexture(commands, {.SrcData = std::as_bytes(std::span{pixels}), .DstTexture = borrowed, .DstRange = {0, 1, 0, 1}, .SrcRowPitch = 8, .Before = render::TextureState::Undefined, .After = render::TextureState::CopySource});
    commands->CopyTextureToBuffer(readback.get(), 0, borrowed, {0, 1, 0, 1});
    frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    pixels.fill(0);
    gpu->EndFrameRecordAndSubmit(0);
    originalView.reset();
    storage.reset();
    image.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 1u);
    EXPECT_FALSE(retained.expired());
    EXPECT_FALSE(gpu->CompleteFlightIfReady(0, false));
    gateReleased = ReleaseFence(gate.get(), render::RenderBackend::D3D12, 1);
    ASSERT_TRUE(gateReleased);
    ASSERT_TRUE(gpu->CompleteFlightIfReady(0, true));
    FlightCompletion completion;
    ASSERT_TRUE(timeline.TryReadCompletion(completion));
    EXPECT_EQ(completion.FrameSerial, outstanding);
    gpu->ReleaseFrameResourcesGT(completion);
    outstanding = 0;
    EXPECT_FALSE(retained.expired());
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
    EXPECT_TRUE(retained.expired());
    ScopedBufferMap map{readback.get(), {0, 8}};
    ASSERT_TRUE(map);
    readback->InvalidateMappedRange({0, 8});
    array<uint32_t, 2> observed{};
    std::memcpy(observed.data(), map.Data(), 8);
    EXPECT_EQ(observed[0], 0xff332211u);
    EXPECT_EQ(observed[1], 0xff665544u);
}

void RunRetainedHistoryAndReadback(render::RenderBackend backend, bool cancelCpu) {
    RuntimeStartupResult startup;
    FrameTimeline timeline{3};
    GpuSystemDescriptor desc{.RhiValidation = {.IsEnabled = false}, .FlightDataCount = 3, .EnableFrameProfiler = false};
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    if (backend == render::RenderBackend::Vulkan) desc.Device = render::VulkanDeviceDescriptor{.Queues = std::span{&queue, 1}};
    auto gpu = GpuSystem::TryCreate(desc, timeline, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_TRUE(gpu) << startup.Reason;
    struct History {
        shared_ptr<render::Buffer> Storage;
        uint64_t Generation, Producer;
        bool Valid{false};
    };
    auto makeHistory = [&](uint64_t generation) {
        auto buffer = gpu->GetDevice()->CreateBuffer({4, render::MemoryType::Device, render::BufferUse::CopyDestination | render::BufferUse::CopySource}).Unwrap();
        return make_shared<History>(History{shared_ptr<render::Buffer>{std::move(buffer)}, generation, 0});
    };
    auto current = makeHistory(1);
    gpu->BeginUpdateForFlight(0);
    auto producer = gpu->BeginFrameRecord(0, {}, {}, false);
    current->Producer = producer.FrameSerial();
    const uint32_t firstValue = 0x10203040;
    auto* upload = producer.AllocateCommandBuffer();
    producer.GetUploader().UploadBuffer(upload, {.SrcData = std::as_bytes(std::span{&firstValue, 1}), .DstBuffer = current->Storage.get(), .Before = render::BufferState::Undefined, .After = render::BufferState::CopySource});
    producer.GetCmdAllocator().Return(std::span{&upload, 1});
    producer.RegisterRequired(std::span{&upload, 1});
    producer.RetainGpuForFrameRT(current);
    producer.SubmitFrame();
    ASSERT_TRUE(gpu->CompleteFlightIfReady(0, true));
    FlightCompletion completion;
    ASSERT_TRUE(timeline.TryReadCompletion(completion));
    EXPECT_EQ(completion.FrameSerial, current->Producer);
    current->Valid = true;
    gpu->ReleaseFrameResourcesGT(completion);
    weak_ptr<History> previous = current;
    auto readbackNative = gpu->GetDevice()->CreateBuffer({4, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
    shared_ptr<render::Buffer> cpuResult{std::move(readbackNative)};
    weak_ptr<render::Buffer> resultStorage = cpuResult;
    auto gate = gpu->GetDevice()->CreateFence().Unwrap();
    bool released = false;
    auto releaseGate = MakeScopeGuard([&]() noexcept { if (!released) ReleaseFence(gate.get(), backend, 1); });
    gpu->BeginUpdateForFlight(1);
    auto reader = gpu->BeginFrameRecord(1, {}, {}, false);
    auto* read = reader.AllocateCommandBuffer();
    if (backend == render::RenderBackend::Vulkan) GpuSystemBufferBarrier(read, cpuResult.get(), render::BufferState::Undefined, render::BufferState::CopyDestination);
    read->CopyBufferToBuffer(cpuResult.get(), 0, current->Storage.get(), 0, 4);
    if (backend == render::RenderBackend::Vulkan) GpuSystemBufferBarrier(read, cpuResult.get(), render::BufferState::CopyDestination, render::BufferState::HostRead);
    render::Fence* wait = gate.get();
    uint64_t value = 1;
    reader.ReturnCommandBuffers({.CmdBuffers = std::span{&read, 1}, .WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
    reader.RetainGpuForFrameRT(current);
    reader.RetainGpuForFrameRT(cpuResult);
    reader.SubmitFrame();
    // Stop selecting the old generation for future work; its accepted reader still owns it.
    current = makeHistory(2);
    EXPECT_FALSE(previous.expired());
    EXPECT_FALSE(current->Valid);
    if (cancelCpu) cpuResult.reset();
    EXPECT_FALSE(resultStorage.expired());
    gpu->BeginUpdateForFlight(2);
    auto nextProducer = gpu->BeginFrameRecord(2, {}, {}, false);
    current->Producer = nextProducer.FrameSerial();
    const uint32_t nextValue = 0xaabbccdd;
    auto* update = nextProducer.AllocateCommandBuffer();
    nextProducer.GetUploader().UploadBuffer(update, {.SrcData = std::as_bytes(std::span{&nextValue, 1}), .DstBuffer = current->Storage.get(), .Before = render::BufferState::Undefined, .After = render::BufferState::CopySource});
    nextProducer.ReturnCommandBuffers({.CmdBuffers = std::span{&update, 1}});
    nextProducer.RetainGpuForFrameRT(current);
    nextProducer.SubmitFrame();
    EXPECT_FALSE(gpu->CompleteFlightIfReady(1, false));
    EXPECT_FALSE(gpu->CompleteFlightIfReady(2, false));
    EXPECT_FALSE(current->Valid);
    released = ReleaseFence(gate.get(), backend, 1);
    ASSERT_TRUE(released);
    ASSERT_TRUE(gpu->CompleteFlightIfReady(2, true));
    // Completing the later producer ends both native epochs before owners are released.
    while (timeline.TryReadCompletion(completion)) {
        if (completion.FrameSerial == current->Producer) current->Valid = true;
        gpu->ReleaseFrameResourcesGT(completion);
    }
    EXPECT_TRUE(previous.expired());
    EXPECT_TRUE(current->Valid);
    EXPECT_EQ(current->Generation, 2u);
    if (cancelCpu)
        EXPECT_TRUE(resultStorage.expired());
    else {
        EXPECT_FALSE(resultStorage.expired());
        ScopedBufferMap map{cpuResult.get(), {0, 4}};
        ASSERT_TRUE(map);
        cpuResult->InvalidateMappedRange({0, 4});
        EXPECT_EQ(*map.DataAs<const uint32_t>(), firstValue);
    }
    if (!cancelCpu) cpuResult.reset();
    EXPECT_TRUE(resultStorage.expired());
    gpu->AbandonUnpublishedResourcesTerminalGT();
}
TEST(GpuRetainedResult, D3D12HistoryReplacementKeepsAcceptedReaderAndDelayedCpuResult) { RunRetainedHistoryAndReadback(render::RenderBackend::D3D12, false); }
TEST(GpuRetainedResult, VulkanHistoryReplacementKeepsAcceptedReaderAndDelayedCpuResult) { RunRetainedHistoryAndReadback(render::RenderBackend::Vulkan, false); }
TEST(GpuRetainedResult, D3D12CpuCancellationKeepsPendingGpuWriteAlive) { RunRetainedHistoryAndReadback(render::RenderBackend::D3D12, true); }
TEST(GpuRetainedResult, VulkanCpuCancellationKeepsPendingGpuWriteAlive) { RunRetainedHistoryAndReadback(render::RenderBackend::Vulkan, true); }

void RunFrameTargetPool(render::RenderBackend backend, uint32_t flights) {
    test::RuntimeLogCapture logs;
    FrameTimeline timeline{flights};
    RuntimeStartupResult startup;
    auto gpu = CreateGpuSystem(backend, false, timeline, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    const auto pitch = Align(uint64_t{32}, gpu->GetDevice()->GetDetail().TextureDataPitchAlignment);
    vector<unique_ptr<render::Buffer>> results;
    vector<std::optional<GpuFrameResources::TextureLease>> previous(flights);
    vector<uint8_t> expected(flights);
    for (uint32_t i = 0; i < flights; ++i)
        results.push_back(gpu->GetDevice()->CreateBuffer({pitch * 8, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap());
    const auto collect = [&] {
        FlightCompletion completion;
        while (timeline.TryReadCompletion(completion)) {
            const auto flight = completion.FlightIndex;
            ScopedBufferMap map{results[flight].get(), {0, pitch * 8}};
            ASSERT_TRUE(map);
            results[flight]->InvalidateMappedRange({0, pitch * 8});
            EXPECT_NEAR(map.DataAs<const uint8_t>()[0], expected[flight], 1);
            gpu->ReleaseFrameResourcesGT(completion);
        }
    };
    const render::TextureDescriptor desc{render::TextureDimension::Dim2D, 8, 8, 1, 1, 1,
                                         render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::CopySource};
    for (uint32_t round = 0; round < flights * 3; ++round) {
        const auto flight = round % flights;
        ASSERT_TRUE(gpu->CompleteFlightIfReady(flight, true));
        collect();
        gpu->BeginUpdateForFlight(flight);
        auto frame = gpu->BeginFrameRecord(flight, {}, {}, false);
        auto& resources = frame.GetGpuFrameResources();
        resources.SetTextureBudget({1, 1, 8 * 8 * 4});
        auto target = resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget);
        ASSERT_TRUE(target);
        EXPECT_FALSE(target->Initialized);
        if (previous[flight]) {
            EXPECT_EQ(target->Target, previous[flight]->Target);
            EXPECT_FALSE(resources.IsCurrent(*previous[flight]));
            EXPECT_EQ(target->InitialState, render::TextureState::CopySource);
        }
        EXPECT_FALSE(resources.BorrowTexture(desc, render::TextureViewUsage::RenderTarget));
        auto* command = frame.AllocateCommandBuffer();
        expected[flight] = static_cast<uint8_t>(round + 1);
        const render::RenderPassColorAttachment color{target->View, render::LoadAction::Clear, render::StoreAction::Store, {{float(expected[flight]) / 255, 0, 0, 1}}};
        const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{target->Target, target->InitialState, render::TextureState::RenderTarget};
        command->ResourceBarrier(std::span{&before, 1});
        auto encoder = command->BeginRenderPass({std::span{&color, 1}, {}, 8, 8, 1});
        ASSERT_TRUE(encoder);
        command->EndRenderPass(encoder.Release());
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{target->Target, render::TextureState::RenderTarget, render::TextureState::CopySource};
        command->ResourceBarrier(std::span{&after, 1});
        command->CopyTextureToBuffer(results[flight].get(), 0, target->Target, {0, 1, 0, 1});
        frame.ReturnCommandBuffers({.CmdBuffers = std::span{&command, 1}});
        resources.SetTextureFinalState(*target, render::TextureState::CopySource);
        previous[flight] = *target;
        frame.SubmitFrame();
        EXPECT_FALSE(resources.IsCurrent(*target));
    }
    gpu->WaitAndRetireFlights();
    collect();
    gpu->AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
TEST(GpuFrameTargets, D3D12F1) { RunFrameTargetPool(render::RenderBackend::D3D12, 1); }
TEST(GpuFrameTargets, D3D12F2) { RunFrameTargetPool(render::RenderBackend::D3D12, 2); }
TEST(GpuFrameTargets, D3D12F3) { RunFrameTargetPool(render::RenderBackend::D3D12, 3); }
TEST(GpuFrameTargets, D3D12F4) { RunFrameTargetPool(render::RenderBackend::D3D12, 4); }
TEST(GpuFrameTargets, VulkanF1) { RunFrameTargetPool(render::RenderBackend::Vulkan, 1); }
TEST(GpuFrameTargets, VulkanF2) { RunFrameTargetPool(render::RenderBackend::Vulkan, 2); }
TEST(GpuFrameTargets, VulkanF3) { RunFrameTargetPool(render::RenderBackend::Vulkan, 3); }
TEST(GpuFrameTargets, VulkanF4) { RunFrameTargetPool(render::RenderBackend::Vulkan, 4); }

class GpuSystemDeathTest : public testing::Test {
protected:
    void SetUp() override {
        RuntimeStartupResult startup;
        Gpu = CreateGpuSystem(render::RenderBackend::D3D12, false, Timeline, startup, false);
        if (test::CanSkipRuntimeStartup(render::RenderBackend::D3D12, startup)) GTEST_SKIP() << startup.Reason;
        ASSERT_NE(Gpu, nullptr) << startup.Reason;
    }
    FrameTimeline Timeline{2};
    unique_ptr<GpuSystem> Gpu;
};

TEST_F(GpuSystemDeathTest, RejectsInvalidCommandReturnsAndUnsealedFrames) {
    auto context = Gpu->BeginFrameRecord(0, {}, {}, false);
    auto* commands = context.AllocateCommandBuffer();
    auto foreignStorage = Gpu->GetDevice()->CreateCommandAllocator(Gpu->GetMainQueue()).Unwrap();
    auto foreign = Gpu->GetDevice()->CreateCommandBuffer(foreignStorage.get()).Unwrap();
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
    {
        FlightCompletion notification;
        while (Timeline.TryReadCompletion(notification)) {
        }
    }
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
    auto next = Gpu->BeginFrameRecord(0, {}, {}, false);
    EXPECT_DEATH(context.AllocateCommandBuffer(), "");
    next.SubmitFrame();
    other.SubmitFrame();
    Gpu->WaitAndRetireFlights();
    {
        FlightCompletion notification;
        while (Timeline.TryReadCompletion(notification)) {
        }
    }
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = next.FrameSerial()});
    {
        FlightCompletion notification;
        while (Timeline.TryReadCompletion(notification)) {
        }
    }
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 1, .FrameSerial = other.FrameSerial()});
}

TEST_F(GpuSystemDeathTest, ClosedCommandRegistrationRequiresCompleteUniqueResults) {
    auto context = Gpu->BeginFrameRecord(0, {}, {}, false);
    auto& allocator = context.GetCmdAllocator();
    auto* commands = allocator.Allocate();
    EXPECT_DEATH(context.RegisterClosedCommandBuffers({.CmdBuffers = std::span{&commands, 1}}), "");
    allocator.Return(std::span{&commands, 1});
    EXPECT_DEATH(context.SubmitFrame(), "");
    EXPECT_DEATH(allocator.Return(std::span{&commands, 1}), "");
    render::CommandBuffer* duplicate[]{commands, commands};
    EXPECT_DEATH(context.RegisterClosedCommandBuffers({.CmdBuffers = duplicate}), "");
    context.RegisterClosedCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    EXPECT_DEATH(context.RegisterClosedCommandBuffers({.CmdBuffers = std::span{&commands, 1}}), "");
    context.SubmitFrame();
    EXPECT_DEATH((void)allocator.Allocate(), "");
    EXPECT_DEATH(allocator.Return({}), "");
    Gpu->WaitAndRetireFlights();
    {
        FlightCompletion notification;
        while (Timeline.TryReadCompletion(notification)) {
        }
    }
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
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
    {
        FlightCompletion notification;
        while (Timeline.TryReadCompletion(notification)) {
        }
    }
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
    {
        FlightCompletion notification;
        while (Timeline.TryReadCompletion(notification)) {
        }
    }
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
