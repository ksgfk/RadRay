#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_runtime_test_support.h"

#include <radray/runtime/frame_timeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/scope_guard.h>
#include <radray/runtime/render_framework/scene_manager.h>

namespace radray {
namespace {

unique_ptr<GpuSystem> CreateGpuSystem(render::RenderBackend backend, bool profiler, FrameTimeline& timeline, RuntimeStartupResult& startup, bool validation = true) {
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor desc{
        .VulkanInstance = {.IsEnableDebugLayer = validation, .IsEnableSynchronizationValidation = validation},
        .DXGIFactory = {.IsEnableDebugLayer = validation},
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

void RunCommandBatches(render::RenderBackend backend, bool profiler) {
    test::RuntimeLogCapture logs;
    RuntimeStartupResult startup;
    FrameTimeline timeline{2};
    auto gpu = CreateGpuSystem(backend, profiler, timeline, startup);
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
    timeline.PumpWaitFrame(0);
    EXPECT_FALSE(resumed);
    EXPECT_TRUE(ReleaseFence(gate.get(), backend, 1));
    EXPECT_TRUE(gpu->CompleteFlightIfReady(0, true));
    gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
    timeline.PumpWaitFrame(0);
    EXPECT_TRUE(resumed);
    gpu->WaitAndRetireFlights();
}

TEST(GpuSystemTest, D3D12OnlyFinalFenceRetiresFlight) { RunFinalFence(render::RenderBackend::D3D12); }
TEST(GpuSystemTest, VulkanOnlyFinalFenceRetiresFlight) { RunFinalFence(render::RenderBackend::Vulkan); }

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
    assets.SetWaitFrameProcessor(&timeline);
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
    auto readback = external.Device->CreateBuffer({.Size = 80, .Memory = render::MemoryType::ReadBack,
        .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
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
        for (uint32_t flight = 0; flight < 2; ++flight) if (serials[flight]) {
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
    assets.SetWaitFrameProcessor(&timeline);
    auto created = external.Device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 2, .Height = 1,
        .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = render::TextureFormat::RGBA8_UNORM,
        .Memory = render::MemoryType::Device, .Usage = render::TextureUse::Resource | render::TextureUse::CopySource | render::TextureUse::CopyDestination});
    ASSERT_TRUE(created);
    shared_ptr<render::Texture> storage = created.Release();
    weak_ptr<render::Texture> retained = storage;
    auto originalView = external.Device->CreateTextureView({storage.get(), render::TextureDimension::Dim2D,
        render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource}).Unwrap();
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
        .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
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
    auto frame = gpu->BeginFrameRecord(0, {}, {}, false);
    outstanding = frame.FrameSerial();
    render::Fence* waits[]{gate.get()};
    uint64_t values[]{1};
    frame.ReturnCommandBuffers({.WaitFences = waits, .WaitValues = values});
    auto pixels = array<uint32_t, 2>{0xff332211, 0xff665544};
    auto* commands = frame.AllocateCommandBuffer();
    frame.GetUploader().UploadTexture(commands, {.SrcData = std::as_bytes(std::span{pixels}), .DstTexture = borrowed,
        .DstRange = {0, 1, 0, 1}, .SrcRowPitch = 8, .Before = render::TextureState::Undefined, .After = render::TextureState::CopySource});
    commands->CopyTextureToBuffer(readback.get(), 0, borrowed, {0, 1, 0, 1});
    frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
    pixels.fill(0);
    gpu->EndFrameRecordAndSubmit(0);
    originalView.reset();
    storage.reset();
    image.Reset();
    assets.Pump();
    EXPECT_EQ(assets.GetAssetCount(), 0u);
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
    timeline.PumpWaitFrame(0);
    EXPECT_TRUE(retained.expired());
    ScopedBufferMap map{readback.get(), {0, 8}};
    ASSERT_TRUE(map);
    readback->InvalidateMappedRange({0, 8});
    array<uint32_t, 2> observed{};
    std::memcpy(observed.data(), map.Data(), 8);
    EXPECT_EQ(observed[0], 0xff332211u);
    EXPECT_EQ(observed[1], 0xff665544u);
}

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
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = context.FrameSerial()});
    auto next = Gpu->BeginFrameRecord(0, {}, {}, false);
    EXPECT_DEATH(context.AllocateCommandBuffer(), "");
    next.SubmitFrame();
    other.SubmitFrame();
    Gpu->WaitAndRetireFlights();
    Gpu->ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = next.FrameSerial()});
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
