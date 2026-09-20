#include "scene_test_support.h"
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <radray/scope_guard.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/render/backend_shader_artifact.h>

namespace radray {
namespace {

struct MeshLifetime {
    uint32_t ActorsDestroyed{0}, AssetsDestroyed{0}, PayloadsReleased{0};
    std::thread::id Thread{std::this_thread::get_id()};
};

class DrawMesh final : public StaticMesh {
public:
    DrawMesh(MeshResource cpu, GpuMesh gpu, MeshLifetime& lifetime)
        : StaticMesh(std::move(cpu), {}, {-1, -1, 0}, {1, 1, 0}, std::move(gpu)), Life(lifetime) {}
    ~DrawMesh() noexcept override {
        EXPECT_EQ(std::this_thread::get_id(), Life.Thread);
        ++Life.AssetsDestroyed;
    }
    void OnUnload(AssetManager& manager) override {
        StaticMesh::OnUnload(manager);
        ++Life.PayloadsReleased;
    }
    MeshLifetime& Life;
};

class DrawActor final : public Actor {
public:
    explicit DrawActor(MeshLifetime& life) : Life(life) {}
    ~DrawActor() noexcept override { ++Life.ActorsDestroyed; }
    MeshLifetime& Life;
};

class FrameWaitProbe final : public IWaitFrameProcessor {
public:
    explicit FrameWaitProbe(GpuSystem& gpu) : Gpu(gpu) {}
    task<void> Wait() override {
        ++Calls;
        co_await Gpu.Wait();
    }
    GpuSystem& Gpu;
    uint32_t Calls{0};
};

bool SignalGate(render::Fence& fence, render::RenderBackend backend) {
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

task<void> WaitForDraw(GpuSystem& gpu, bool& notified) {
    co_await gpu.Wait();
    notified = true;
}

void RunMeshLifetime(render::RenderBackend backend, bool delayed, bool direct, bool threaded = false, uint32_t viewsToDraw = 1) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe)) GTEST_SKIP() << probe.Reason;
    }
    test::RuntimeLogCapture logs;
    MeshLifetime lifetime;
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor descriptor{
        .VulkanInstance = {.IsEnableDebugLayer = !delayed, .IsEnableSynchronizationValidation = !delayed},
        .DXGIFactory = {.IsEnableDebugLayer = !delayed},
        .FlightDataCount = 2,
        .EnableFrameProfiler = false};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor vk;
        vk.Queues = std::span{&queue, 1};
        descriptor.Device = vk;
    }
    GpuSystem gpu{descriptor};
    FrameWaitProbe assetWaits{gpu};
    AssetManager assets;
    assets.SetWaitFrameProcessor(&assetWaits);
    Application app;
    RenderSystem renderer{&app, 2};
    test::ScopedWorld world;
    const auto sceneId = test::ConnectWorld(world, renderer);
    auto* device = gpu.GetDevice();
    testing::Test::RecordProperty("backend", backend == render::RenderBackend::D3D12 ? "d3d12" : "vulkan");
    testing::Test::RecordProperty("validation", delayed ? "slow-fence-stress" : "native-validation");
    constexpr uint32_t width = 32, height = 32;
    constexpr auto format = render::TextureFormat::RGBA8_UNORM;
    const std::string_view source = R"hlsl(
[shader("vertex")]
float4 VSMain(float3 position : POSITION) : SV_Position { return float4(position, 1); }
[shader("pixel")]
float4 PSMain() : SV_Target0 { return float4(1, 0, 1, 1); }
)hlsl";
    ShaderJit jit{{}};
    const auto target = *render::GetShaderTargetForBackend(backend);
    const auto bytes = std::as_bytes(std::span{source.data(), source.size()});
    auto contract = jit.DiscoverContractHash("lifecycle/triangle.hlsl", bytes, target);
    ASSERT_TRUE(contract);
    auto compiled = jit.Compile({.SourceName = "lifecycle/triangle.hlsl", .RootSource = vector<byte>{bytes.begin(), bytes.end()}, .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)), .ExpectedContract = *contract}, target);
    ASSERT_TRUE(compiled);
    auto artifact = render::CreateBackendShaderArtifact(*device, compiled->Metadata, {.Target = target, .ExpectedGpuArtifact = compiled->ExpectedGpuArtifact});
    ASSERT_TRUE(artifact);
    auto program = ShaderProgram::Create(device, std::move(*artifact)).Unwrap();
    auto image = render::test::MakeRenderTarget(device, format, width, height, render::TextureUse::RenderTarget | render::TextureUse::CopySource);
    ASSERT_TRUE(image);
    const render::RenderPassColorAttachmentDescriptor attachment{.Format = format, .SampleCount = 1, .Load = render::LoadAction::Clear, .Store = render::StoreAction::Store};
    auto pass = device->CreateRenderPass({.ColorAttachments = std::span{&attachment, 1}}).Unwrap();
    render::TextureView* views[]{image->View.get()};
    auto framebuffer = device->CreateFramebuffer({.Pass = pass.get(), .ColorAttachments = views, .Width = width, .Height = height, .Layers = 1}).Unwrap();
    const render::VertexBufferLayout vertexLayout{.Binding = 0, .ArrayStride = 12, .StepMode = render::VertexStepMode::Vertex};
    const render::VertexAttribute attribute{.BufferBinding = 0, .Semantic = "POSITION", .Format = render::VertexFormat::FLOAT32X3, .Location = 0};
    const render::VertexInputState input{.Buffers = std::span{&vertexLayout, 1}, .Attributes = std::span{&attribute, 1}};
    const auto color = render::ColorTargetState::Default(format);
    auto primitive = render::PrimitiveState::Default();
    primitive.Cull = render::CullMode::None;
    auto pso = device->CreateGraphicsPipelineState({.PipelineLayout = program->GetPipelineLayout(), .VS = program->GetStage(shader::ShaderStage::Vertex), .PS = program->GetStage(shader::ShaderStage::Pixel), .VertexInput = input, .Primitive = primitive, .DepthStencil = std::nullopt, .MultiSample = render::MultiSampleState::Default(), .ColorTargets = std::span{&color, 1}, .CompatibleRenderPass = pass.get()}).Unwrap();
    const array<float, 9> vertices{0, 0.8f, 0, -0.8f, -0.8f, 0, 0.8f, -0.8f, 0};
    const array<uint32_t, 3> indices{0, 1, 2};
    MeshResource cpu;
    cpu.Bins.emplace_back(std::as_bytes(std::span{vertices}));
    cpu.Bins.emplace_back(std::as_bytes(std::span{indices}));
    MeshPrimitive cpuPrimitive;
    cpuPrimitive.VertexCount = 3;
    cpuPrimitive.VertexBuffers.push_back({"POSITION", 0, 0, VertexDataType::FLOAT, 3, 0, 12});
    cpuPrimitive.IndexBuffer = {1, 3, 0, 4};
    cpu.Primitives.push_back(std::move(cpuPrimitive));
    GpuMesh geometry;
    geometry.Buffers.push_back(render::test::MakeUploadBuffer(*device, std::as_bytes(std::span{vertices}), render::BufferUse::Vertex).Unwrap());
    geometry.Draws.emplace_back();
    geometry.Draws[0].VertexBuffers.push_back({.Binding = 0, .View = {.Target = geometry.Buffers[0].get(), .Size = sizeof(vertices)}});
    auto mesh = assets.AddReady<StaticMesh>(AssetId{17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, make_unique<DrawMesh>(std::move(cpu), std::move(geometry), lifetime));
    auto* actor = world.SpawnActor<DrawActor>(lifetime);
    auto* component = actor->AddComponent<StaticMeshComponent>();
    if (!direct) component->SetStaticMesh(mesh);
    const auto id = component->GetShapeId();
    gpu.BeginUpdateForFlight(0);
    if (direct) gpu.RetainForFrameGT(0, mesh);
    const auto* borrowedMesh = &mesh->GetRenderMesh();
    mesh.Reset();
    test::PrepareScene(world, renderer, 0);
    renderer.PublishFrameGT(0);
    const uint64_t pitch = Align(uint64_t{width} * 4, device->GetDetail().TextureDataPitchAlignment);
    auto readback = device->CreateBuffer({.Size = pitch * height, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
    auto gate = device->CreateFence().Unwrap();
    bool released = !delayed;
    auto release = MakeScopeGuard([&]() noexcept { if (!released) SignalGate(*gate, backend); });
    bool notified = false;
    TaskScope notification;
    notification.Spawn(WaitForDraw(gpu, notified));
    uint64_t drawSerial = 0;
    const auto record = [&] {
        auto frame = gpu.BeginFrameRecord(0, {}, {}, false);
        drawSerial = frame.FrameSerial();
        renderer.ConsumeRenderUpdates(0, drawSerial);
        const auto* geometryView = direct ? borrowedMesh : renderer.GetSceneRT(sceneId)->GetStaticMesh(id)->Mesh.RenderMesh.Get();
        if (delayed) {
            render::Fence* fences[]{gate.get()};
            uint64_t values[]{1};
            frame.ReturnCommandBuffers({.WaitFences = fences, .WaitValues = values});
        }
        auto* commands = frame.AllocateCommandBuffer();
        const render::ResourceBarrierDescriptor toTarget = render::BarrierTextureDescriptor{.Target = image->Tex.get(), .Before = render::TextureState::Undefined, .After = render::TextureState::RenderTarget};
        commands->ResourceBarrier(std::span{&toTarget, 1});
        const render::ColorClearValue clear{{0, 0, 0, 1}};
        auto encoder = commands->BeginRenderPass({.Pass = pass.get(), .Target = framebuffer.get(), .ColorClearValues = std::span{&clear, 1}}).Unwrap();
        encoder->SetViewport({0, 0, float(width), float(height), 0, 1});
        encoder->SetScissor({0, 0, width, height});
        encoder->BindGraphicsPipelineState(pso.get());
        encoder->BindVertexBuffers(geometryView->Draws[0].VertexBuffers);
        for (uint32_t view = 0; view < viewsToDraw; ++view) {
            const auto viewWidth = width / viewsToDraw;
            encoder->SetViewport({float(view * viewWidth), 0, float(viewWidth), float(height), 0, 1});
            encoder->SetScissor({int32_t(view * viewWidth), 0, viewWidth, height});
            encoder->Draw(3, 1, 0, 0);
        }
        commands->EndRenderPass(std::move(encoder));
        const render::ResourceBarrierDescriptor toCopy = render::BarrierTextureDescriptor{.Target = image->Tex.get(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::CopySource};
        commands->ResourceBarrier(std::span{&toCopy, 1});
        commands->CopyTextureToBuffer(readback.get(), 0, image->Tex.get(), {0, 1, 0, 1});
        frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        gpu.EndFrameRecordAndSubmit(0);
    };
    if (threaded) {
        std::thread rt{record};
        rt.join();
    } else
        record();
    notification.RequestStop();
    notification.WaitUntilEmpty();
    EXPECT_FALSE(notified);
    world.DestroyActor(actor);
    world.FinalizeWorldGT();
    EXPECT_EQ(lifetime.ActorsDestroyed, 1u);
    EXPECT_EQ(lifetime.AssetsDestroyed, 0u);
    gpu.BeginUpdateForFlight(1);
    test::PrepareScene(world, renderer, 1);
    renderer.PublishFrameGT(1);
    auto removal = gpu.BeginFrameRecord(1, {}, {}, false, false);
    renderer.ConsumeRenderUpdates(1, removal.FrameSerial());
    gpu.EndFrameRecordAndSubmit(1);
    assets.Pump();
    EXPECT_EQ(lifetime.AssetsDestroyed, 0u);
    if (delayed) {
        EXPECT_FALSE(gpu.CompleteFlightIfReady(0, false));
        EXPECT_FALSE(gpu.CompleteFlightIfReady(1, false));
        released = SignalGate(*gate, backend);
        ASSERT_TRUE(released);
    }
    ASSERT_TRUE(gpu.CompleteFlightIfReady(0, true));
    const FlightCompletion drawn{.FlightIndex = 0, .GpuWorkCompleted = true, .FrameSerial = drawSerial};
    renderer.OnFlightCompletedGT(drawn);
    gpu.ReleaseFrameResourcesGT(drawn);
    ASSERT_TRUE(gpu.CompleteFlightIfReady(1, true));
    const FlightCompletion removed{.FlightIndex = 1, .GpuWorkCompleted = false, .FrameSerial = removal.FrameSerial()};
    renderer.OnFlightCompletedGT(removed);
    gpu.ReleaseFrameResourcesGT(removed);
    assets.Pump();
    EXPECT_EQ(lifetime.AssetsDestroyed, 1u);
    EXPECT_EQ(lifetime.PayloadsReleased, 1u);
    EXPECT_EQ(assetWaits.Calls, 0u);
    // No additional frame is submitted between the covering completion and reclamation.
    ScopedBufferMap map{readback.get(), {0, pitch * height}};
    ASSERT_TRUE(map);
    readback->InvalidateMappedRange({0, pitch * height});
    const auto* pixel = map.DataAs<uint8_t>() + pitch * (height / 2) + 4 * (width / 2);
    EXPECT_EQ(pixel[0], 255);
    EXPECT_EQ(pixel[1], 0);
    EXPECT_EQ(pixel[2], 255);
    EXPECT_EQ(pixel[3], 255);
    world.ShutdownWorld();
    renderer.BeginStoppingGT();
    renderer.AbandonUnpublishedFramesGT();
    gpu.WaitAndRetireFlights();
    gpu.CleanupCompletedFlights();
    gpu.AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

struct UploadOwner {
    unique_ptr<render::Buffer> Buffer;
    uint32_t* Destroyed;
    ~UploadOwner() noexcept { ++*Destroyed; }
};

task<AssetLoadResult> UploadNotification(GpuSystem& gpu, bool fail) {
    if (!fail) co_await gpu.Wait();
    co_return AssetLoadResult::Failure();
}

void RunUploadCancellation(render::RenderBackend backend) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe)) GTEST_SKIP() << probe.Reason;
    }
    test::RuntimeLogCapture logs;
    const render::VulkanCommandQueueDescriptor queue{render::QueueType::Direct, 1};
    GpuSystemDescriptor descriptor{.FlightDataCount = 2, .EnableFrameProfiler = false};
    if (backend == render::RenderBackend::Vulkan) {
        render::VulkanDeviceDescriptor vk;
        vk.Queues = std::span{&queue, 1};
        descriptor.Device = vk;
    }
    GpuSystem gpu{descriptor};
    AssetManager assets;
    auto* device = gpu.GetDevice();
    for (bool fail : {false, true}) {
        uint32_t destroyed = 0;
        gpu.BeginUpdateForFlight(0);
        auto owner = make_unique<UploadOwner>();
        owner->Destroyed = &destroyed;
        owner->Buffer = device->CreateBuffer({.Size = 4, .Memory = render::MemoryType::Device, .Usage = render::BufferUse::CopySource | render::BufferUse::CopyDestination}).Unwrap();
        auto* target = owner->Buffer.get();
        gpu.RetainForFrameGT(0, std::move(owner));
        auto readback = device->CreateBuffer({.Size = 4, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        auto gate = device->CreateFence().Unwrap();
        auto frame = gpu.BeginFrameRecord(0, {}, {}, false);
        render::Fence* fences[]{gate.get()};
        uint64_t values[]{1};
        frame.ReturnCommandBuffers({.WaitFences = fences, .WaitValues = values});
        auto* commands = frame.AllocateCommandBuffer();
        const uint32_t expected = 0x260920;
        frame.GetUploader().UploadBuffer(commands, {.SrcData = std::as_bytes(std::span{&expected, 1}),
                                                    .DstBuffer = target,
                                                    .Before = render::BufferState::Undefined,
                                                    .After = render::BufferState::CopySource});
        if (backend == render::RenderBackend::Vulkan) {
            const render::ResourceBarrierDescriptor barrier = render::BarrierBufferDescriptor{.Target = readback.get(),
                                                                                              .Before = render::BufferState::Undefined,
                                                                                              .After = render::BufferState::CopyDestination};
            commands->ResourceBarrier(std::span{&barrier, 1});
        }
        commands->CopyBufferToBuffer(readback.get(), 0, target, 0, 4);
        frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        gpu.EndFrameRecordAndSubmit(0);
        auto loading = assets.Load({.Id = AssetId{uint32_t(fail) + 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, .Task = UploadNotification(gpu, fail)});
        if (!fail) loading.Cancel();
        assets.Pump();
        EXPECT_TRUE(fail ? loading.IsFaulted() : loading.IsCanceled());
        loading.Reset();
        assets.Pump();
        EXPECT_EQ(destroyed, 0u);
        EXPECT_FALSE(gpu.CompleteFlightIfReady(0, false));
        EXPECT_TRUE(SignalGate(*gate, backend));
        EXPECT_TRUE(gpu.CompleteFlightIfReady(0, true));
        gpu.ReleaseFrameResourcesGT({.FlightIndex = 0, .FrameSerial = frame.FrameSerial()});
        EXPECT_EQ(destroyed, 1u);
        ScopedBufferMap map{readback.get(), {0, 4}};
        ASSERT_TRUE(map);
        readback->InvalidateMappedRange({0, 4});
        EXPECT_EQ(*map.DataAs<uint32_t>(), expected);
    }
    gpu.WaitAndRetireFlights();
    gpu.AbandonUnpublishedResourcesTerminalGT();
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(GpuSceneLifetime, D3D12SubmittedUploadSurvivesLoadFailureAndCancellation) { RunUploadCancellation(render::RenderBackend::D3D12); }
TEST(GpuSceneLifetime, VulkanSubmittedUploadSurvivesLoadFailureAndCancellation) { RunUploadCancellation(render::RenderBackend::Vulkan); }
TEST(GpuSceneLifetime, D3D12ThreadedThreeViewDraw) { RunMeshLifetime(render::RenderBackend::D3D12, true, false, true, 3); }
TEST(GpuSceneLifetime, VulkanThreadedThreeViewDraw) { RunMeshLifetime(render::RenderBackend::Vulkan, true, false, true, 3); }

TEST(GpuSceneLifetime, D3D12ValidatedDrawReadback) { RunMeshLifetime(render::RenderBackend::D3D12, false, false); }
TEST(GpuSceneLifetime, VulkanValidatedDrawReadback) { RunMeshLifetime(render::RenderBackend::Vulkan, false, false); }
TEST(GpuSceneLifetime, D3D12DelayedDrawOutlivesActor) { RunMeshLifetime(render::RenderBackend::D3D12, true, false); }
TEST(GpuSceneLifetime, VulkanDelayedDrawOutlivesActor) { RunMeshLifetime(render::RenderBackend::Vulkan, true, false); }
TEST(GpuSceneLifetime, D3D12DirectDrawOwnerSurvivesCanceledNotification) { RunMeshLifetime(render::RenderBackend::D3D12, true, true); }
TEST(GpuSceneLifetime, VulkanDirectDrawOwnerSurvivesCanceledNotification) { RunMeshLifetime(render::RenderBackend::Vulkan, true, true); }

}  // namespace
}  // namespace radray
