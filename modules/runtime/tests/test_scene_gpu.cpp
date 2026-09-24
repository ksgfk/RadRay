#include "test_scene_gpu.h"
#include "runtime_test_support.h"
#include "gpu_runtime_test_support.h"
#include "runtime_test_device.h"
#include <gtest/gtest.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>
#include <radray/scope_guard.h>

namespace radray::test {

bool ReleaseGpuGate(render::Fence& fence, render::RenderBackend backend) {
#if defined(RADRAY_ENABLE_D3D12)
    if (backend == render::RenderBackend::D3D12) return SUCCEEDED(static_cast<render::d3d12::FenceD3D12&>(fence)._fence->Signal(1));
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    if (backend == render::RenderBackend::Vulkan) {
        auto& native = static_cast<render::vulkan::FenceVulkan&>(fence);
        const VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, native._fence->_semaphore, 1};
        return native._device->_ftb.vkSignalSemaphore(native._device->_device, &info) == VK_SUCCESS;
    }
#endif
    return false;
}

void RunObjectBuffers(render::RenderBackend backend, uint32_t flights, bool threaded, bool delayed) {
    test::RuntimeLogCapture logs;
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor descriptor{.VulkanInstance = {.IsEnableDebugLayer = !delayed, .IsEnableSynchronizationValidation = !delayed},
                                   .DXGIFactory = {.IsEnableDebugLayer = !delayed},
                                   .FlightDataCount = flights,
                                   .EnableFrameProfiler = false};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor vk;
        vk.Queues = std::span{&queue, 1};
        descriptor.Device = vk;
    }
    RuntimeStartupResult startup;
    FrameTimeline timeline{flights};
    auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_NE(gpu, nullptr) << startup.Reason;
    Application app;
    RenderSystem renderer{&app, flights};
    auto scene = renderer.CreateSceneGT();
    auto* writer = renderer.GetSceneWriterGT(scene).Get();
    auto id = writer->CreateShape();
    Eigen::Matrix4f expected = Eigen::Matrix4f::Identity();
    writer->SetStaticMesh(id, {}, expected);
    const auto second = renderer.CreateSceneGT();
    auto* secondWriter = renderer.GetSceneWriterGT(second).Get();
    auto secondId = secondWriter->CreateShape();
    Eigen::Matrix4f other = Eigen::Matrix4f::Identity();
    other(1, 3) = 91;
    secondWriter->SetStaticMesh(secondId, {}, other);
    EXPECT_EQ(id.Index, secondId.Index);
    vector<unique_ptr<render::Buffer>> readbacks;
    vector<uint64_t> serials(flights, 0);
    vector<Eigen::Matrix4f> snapshots(flights);
    vector<render::Buffer*> physical(flights, nullptr);
    auto gate = gpu->GetDevice()->CreateFence().Unwrap();
    bool gateReleased = !delayed;
    auto guard = MakeScopeGuard([&]() noexcept { if (!gateReleased) ReleaseGpuGate(*gate, backend); });
    for (uint32_t f = 0; f < flights; ++f) {
        auto buffer = gpu->GetDevice()->CreateBuffer({.Size = 128, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead});
        ASSERT_TRUE(buffer);
        readbacks.push_back(buffer.Release());
    }
    const auto complete = [&](uint32_t f) {
        if (!serials[f]) return;
        ASSERT_TRUE(gpu->CompleteFlightIfReady(f, true));
        const FlightCompletion completion{f, true, serials[f]};
        renderer.OnFlightCompletedGT(completion);
        gpu->ReleaseFrameResourcesGT(completion);
        ScopedBufferMap map{readbacks[f].get(), {0, 128}};
        ASSERT_TRUE(map);
        const auto* values = map.DataAs<float>();
        for (uint32_t i = 0; i < 16; ++i) EXPECT_FLOAT_EQ(values[i], snapshots[f].data()[i]);
        for (uint32_t i = 0; i < 16; ++i) EXPECT_FLOAT_EQ(values[16 + i], other.data()[i]);
        serials[f] = 0;
    };
    for (uint32_t step = 0; step < flights * 6; ++step) {
        const uint32_t f = step % flights;
        complete(f);
        gpu->BeginUpdateForFlight(f);
        if (step == flights) {
            expected(0, 3) = 7;
            expected(1, 1) = -2;
            writer->SetTransform(id, expected);
        }
        if (step == flights * 3) {
            const auto old = id;
            writer->RemoveShape(old);
            id = writer->CreateShape();
            EXPECT_EQ(id.Index, old.Index);
            EXPECT_NE(id.Generation, old.Generation);
            expected(2, 3) = 13;
            writer->SetStaticMesh(id, {}, expected);
        }
        renderer.SealFrameGT(f);
        renderer.PublishFrameGT(f);
        snapshots[f] = expected;
        const auto record = [&] {
            auto frame = gpu->BeginFrameRecord(f, {}, {}, false);
            serials[f] = frame.FrameSerial();
            renderer.ConsumeRenderUpdates(f, frame.FrameSerial());
            if (delayed && step == flights * 6 - 1) {
                auto* wait = gate.get();
                uint64_t value = 1;
                frame.ReturnCommandBuffers({.WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
            }
            const auto previousBytes = frame.GetHostWrites().GetStats().CommittedBytes;
            auto objects = renderer.PrepareSceneGpuRT(scene, frame);
            ASSERT_TRUE(objects);
            auto again = renderer.PrepareSceneGpuRT(scene, frame);
            ASSERT_TRUE(again);
            EXPECT_EQ(objects->Objects.Target, again->Objects.Target);
            if (physical[f]) EXPECT_EQ(physical[f], objects->Objects.Target);
            physical[f] = objects->Objects.Target;
            auto otherObjects = renderer.PrepareSceneGpuRT(second, frame);
            ASSERT_TRUE(otherObjects);
            EXPECT_NE(objects->Objects.Target, otherObjects->Objects.Target);
            const auto stats = frame.GetHostWrites().GetStats();
            const bool changed = step < flights * 2 || (step >= flights * 3 && step < flights * 4);
            EXPECT_EQ(stats.CommittedBytes - previousBytes, step < flights ? 128u : changed ? 64u
                                                                                            : 0u);
            auto* commands = frame.AllocateCommandBuffer();
            uint64_t offset = 0;
            for (auto buffer : {objects->Objects.Target, otherObjects->Objects.Target}) {
                render::ResourceBarrierDescriptor before = render::BarrierBufferDescriptor{.Target = buffer, .Before = render::BufferState::ShaderRead, .After = render::BufferState::CopySource};
                commands->ResourceBarrier(std::span{&before, 1});
                commands->CopyBufferToBuffer(readbacks[f].get(), offset, buffer, 0, 64);
                render::ResourceBarrierDescriptor after = render::BarrierBufferDescriptor{.Target = buffer, .Before = render::BufferState::CopySource, .After = render::BufferState::ShaderRead};
                commands->ResourceBarrier(std::span{&after, 1});
                offset += 64;
            }
            frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
            gpu->EndFrameRecordAndSubmit(f);
        };
        if (threaded) {
            std::thread rt{record};
            rt.join();
        } else
            record();
    }
    for (uint32_t f = 0; f < flights - (delayed ? 1u : 0u); ++f) complete(f);
    renderer.DestroySceneGT(scene);
    renderer.DestroySceneGT(second);
    gpu->BeginUpdateForFlight(0);
    renderer.SealFrameGT(0);
    renderer.PublishFrameGT(0);
    auto frame = gpu->BeginFrameRecord(0, {}, {}, false);
    renderer.ConsumeRenderUpdates(0, frame.FrameSerial());
    EXPECT_FALSE(renderer.GetSceneRT(scene));
    gpu->EndFrameRecordAndSubmit(0);
    if (delayed) {
        EXPECT_FALSE(gpu->CompleteFlightIfReady(0, false));
        EXPECT_FALSE(gpu->CompleteFlightIfReady(flights - 1, false));
        gateReleased = ReleaseGpuGate(*gate, backend);
        ASSERT_TRUE(gateReleased);
        complete(flights - 1);
    }
    ASSERT_TRUE(gpu->CompleteFlightIfReady(0, true));
    const FlightCompletion completion{0, true, frame.FrameSerial()};
    renderer.OnFlightCompletedGT(completion);
    gpu->ReleaseFrameResourcesGT(completion);
    const auto rebuilt = renderer.CreateSceneGT();
    EXPECT_NE(rebuilt, scene);
    renderer.BeginStoppingGT();
    renderer.AbandonUnpublishedFramesGT();
    gpu->WaitAndRetireFlights();
    timeline.CleanupCompletedFlights();
    gpu->AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

class SceneGpu : public testing::TestWithParam<std::tuple<render::RenderBackend, uint32_t, bool>> {};
TEST_P(SceneGpu, ReadbackFlightIsolationAndGenerationReuse) {
    const auto [backend, flights, threaded] = GetParam();
    RunObjectBuffers(backend, flights, threaded);
}
INSTANTIATE_TEST_SUITE_P(Backends, SceneGpu,
                         testing::Combine(testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan), testing::Values(1u, 2u, 3u), testing::Bool()));

TEST(SceneGpuLifetime, D3D12DeletionWaitsForCoveringFence) { RunObjectBuffers(render::RenderBackend::D3D12, 2, true, true); }
TEST(SceneGpuLifetime, VulkanDeletionWaitsForCoveringFence) { RunObjectBuffers(render::RenderBackend::Vulkan, 2, true, true); }

class AllocationProbe final : public test::RuntimeTestDevice {
public:
    explicit AllocationProbe(render::Device* device) : Device(device) {}
    Nullable<unique_ptr<render::Buffer>> CreateBuffer(const render::BufferDescriptor& descriptor) noexcept override {
        ++Calls;
        return Fail ? Nullable<unique_ptr<render::Buffer>>{nullptr} : Device->CreateBuffer(descriptor);
    }
    render::Device* Device;
    bool Fail{true};
    uint32_t Calls{0};
};

void RunAllocationRecovery(render::RenderBackend backend) {
    test::RuntimeLogCapture logs;
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor descriptor{.FlightDataCount = 1, .EnableFrameProfiler = false};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor vk;
        vk.Queues = std::span{&queue, 1};
        descriptor.Device = vk;
    }
    RuntimeStartupResult startup;
    FrameTimeline timeline{1};
    auto gpu = GpuSystem::TryCreate(descriptor, timeline, startup);
    if (test::CanSkipRuntimeStartup(backend, startup)) GTEST_SKIP() << startup.Reason;
    ASSERT_TRUE(gpu) << startup.Reason;
    AllocationProbe allocator{gpu->GetDevice()};
    SceneGpuData mirror{&allocator, 1};
    RenderScene scene;
    radray::SceneApplyChanges changes;
    SceneUpdateBatch batch;
    const ShapeId first{0, 0}, high{17, 0};
    batch.CreateShapes = {first};
    batch.MeshStates = {{.Id = first}};
    scene.Apply(batch, &changes);
    mirror.ApplyChanges(changes);
    auto readback = gpu->GetDevice()->CreateBuffer({.Size = 18 * 64, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
    uint64_t generation = 0;
    for (uint32_t step = 0; step < 6; ++step) {
        gpu->BeginUpdateForFlight(0);
        if (step == 3) {
            batch.Clear();
            batch.CreateShapes = {high};
            Eigen::Matrix4f matrix = Eigen::Matrix4f::Identity();
            matrix(0, 3) = 23;
            batch.MeshStates = {{.Id = high, .LocalToWorld = matrix}};
            scene.Apply(batch, &changes);
            mirror.ApplyChanges(changes);
        }
        allocator.Fail = step == 0 || step == 3;
        auto frame = gpu->BeginFrameRecord(0, {}, {}, false);
        const auto bytes = frame.GetHostWrites().GetStats().CommittedBytes;
        const auto ranges = frame.GetHostWrites().GetStats().CommitCount;
        const auto calls = allocator.Calls;
        auto view = mirror.Prepare(scene, frame);
        EXPECT_EQ(view.has_value(), !allocator.Fail);
        auto again = mirror.Prepare(scene, frame);
        EXPECT_EQ(view.has_value(), again.has_value());
        EXPECT_LE(allocator.Calls - calls, 1u);
        if (view) {
            const auto count = step == 1 ? 1u : step == 4 ? 2u
                                                          : 0u;
            EXPECT_EQ(frame.GetHostWrites().GetStats().CommittedBytes - bytes, count * 64u);
            EXPECT_EQ(frame.GetHostWrites().GetStats().CommitCount - ranges, count);
            if (step == 4) EXPECT_GT(view->BufferGeneration, generation);
            generation = view->BufferGeneration;
            auto* commands = frame.AllocateCommandBuffer();
            auto* buffer = view->Objects.Target;
            render::ResourceBarrierDescriptor before = render::BarrierBufferDescriptor{.Target = buffer, .Before = render::BufferState::ShaderRead, .After = render::BufferState::CopySource};
            commands->ResourceBarrier(std::span{&before, 1});
            commands->CopyBufferToBuffer(readback.get(), 0, buffer, 0, step >= 4 ? 18 * 64 : 64);
            render::ResourceBarrierDescriptor after = render::BarrierBufferDescriptor{.Target = buffer, .Before = render::BufferState::CopySource, .After = render::BufferState::ShaderRead};
            commands->ResourceBarrier(std::span{&after, 1});
            frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        } else {
            EXPECT_EQ(frame.GetHostWrites().GetStats().CommittedBytes, bytes);
        }
        gpu->EndFrameRecordAndSubmit(0);
        ASSERT_TRUE(gpu->CompleteFlightIfReady(0, true));
        mirror.Complete(0, frame.FrameSerial(), true, scene);
        gpu->ReleaseFrameResourcesGT({0, true, frame.FrameSerial()});
        if (view) {
            ScopedBufferMap map{readback.get(), {0, 18 * 64}};
            ASSERT_TRUE(map);
            EXPECT_FLOAT_EQ(map.DataAs<float>()[0], 1);
            if (step >= 4) EXPECT_FLOAT_EQ(map.DataAs<float>()[17 * 16 + 12], 23);
        }
    }
    gpu->WaitAndRetireFlights();
    timeline.CleanupCompletedFlights();
    gpu->AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
TEST(SceneGpuAllocation, D3D12FailureGrowthAndSparseRanges) { RunAllocationRecovery(render::RenderBackend::D3D12); }
TEST(SceneGpuAllocation, VulkanFailureGrowthAndSparseRanges) { RunAllocationRecovery(render::RenderBackend::Vulkan); }

}  // namespace radray::test
