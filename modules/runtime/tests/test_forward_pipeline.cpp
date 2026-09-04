// Covers the ForwardPipeline orchestration end to end: draw collection, per-program
// view/object constant packing, material set preparation and the recorded draw. Every
// other test in the runtime drives the pieces directly, so this is the only place where
// PrepareCamera and the draw pass actually run against a live swapchain.
#include <algorithm>
#include <atomic>
#include <functional>
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "forward_test_access.h"
#include "forward_pipeline/forward_bindings.h"
#include <radray/runtime/forward_pipeline/forward_pipeline.h>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/window_manager.h>
#include <radray/window/native_window.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>

#if defined(RADRAY_PLATFORM_WINDOWS) || defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace radray {
namespace {

constexpr uint32_t kFrameCount = 4;
enum class Scenario { Baseline,
                      ThreadedStress,
                      OffscreenViews,
                      DestroyPrepared,
                      SnapshotValues,
                      NonCanonical,
                      MissingObject,
                      NonDynamic };
constexpr AssetId kMeshId{
    0x11111111, 0x2222, 0x3333, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
constexpr AssetId kTextureId{
    0x22222222, 0x3333, 0x4444, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};

struct QuadVertex {
    float Position[3];
    float Normal[3];
    float UV[2];
};

// A unit quad facing -Z so the camera at -Z looks at its front face.
MeshResource MakeQuadMeshResource() {
    constexpr array<QuadVertex, 4> vertices{
        QuadVertex{{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        QuadVertex{{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        QuadVertex{{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        QuadVertex{{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}}};
    constexpr array<uint32_t, 6> indices{0, 2, 1, 0, 3, 2};

    MeshResource resource;
    resource.Name = "forward-pipeline-test-quad";
    resource.Bins.emplace_back(std::span<const byte>{
        reinterpret_cast<const byte*>(vertices.data()), sizeof(vertices)});
    resource.Bins.emplace_back(std::span<const byte>{
        reinterpret_cast<const byte*>(indices.data()), sizeof(indices)});

    MeshPrimitive primitive;
    primitive.VertexCount = static_cast<uint32_t>(vertices.size());
    primitive.Topology = PrimitiveTopology::TriangleList;
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = string{VertexSemantics::POSITION},
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 3,
        .Offset = offsetof(QuadVertex, Position),
        .Stride = sizeof(QuadVertex)});
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = string{VertexSemantics::NORMAL},
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 3,
        .Offset = offsetof(QuadVertex, Normal),
        .Stride = sizeof(QuadVertex)});
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = string{VertexSemantics::TEXCOORD},
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 2,
        .Offset = offsetof(QuadVertex, UV),
        .Stride = sizeof(QuadVertex)});
    primitive.IndexBuffer = IndexBufferEntry{
        .BufferIndex = 1,
        .IndexCount = static_cast<uint32_t>(indices.size()),
        .Offset = 0,
        .Stride = sizeof(uint32_t)};
    resource.Primitives.push_back(std::move(primitive));
    return resource;
}

Nullable<unique_ptr<TextureAsset>> MakeWhiteTexture(render::Device& device) {
    constexpr render::TextureFormat format = render::TextureFormat::RGBA8_UNORM;
    Nullable<unique_ptr<render::Texture>> texture = device.CreateTexture(
        render::TextureDescriptor{
            .Dim = render::TextureDimension::Dim2D,
            .Width = 1,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = format,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::Resource | render::TextureUse::CopyDestination,
            .Hints = render::ResourceHint::None});
    if (!texture.HasValue()) {
        return nullptr;
    }
    unique_ptr<render::Texture> textureObject = texture.Release();
    Nullable<unique_ptr<render::TextureView>> view = device.CreateTextureView(
        render::TextureViewDescriptor{
            .Target = textureObject.get(),
            .Dim = render::TextureDimension::Dim2D,
            .Format = format,
            .Range = render::SubresourceRange{0, 1, 0, 1},
            .Usage = render::TextureViewUsage::Resource});
    if (!view.HasValue()) {
        return nullptr;
    }
    const uint64_t pitch = std::max<uint64_t>(device.GetDetail().TextureDataPitchAlignment, 4);
    vector<byte> pixels(static_cast<size_t>(pitch), byte{0xff});
    auto upload = render::test::MakeUploadBuffer(device, pixels, render::BufferUse::CopySource);
    auto queue = device.GetCommandQueue(render::QueueType::Direct);
    if (!upload.HasValue() || !queue.HasValue()) {
        return nullptr;
    }
    auto command = device.CreateCommandBuffer(queue.Get());
    if (!command.HasValue()) {
        return nullptr;
    }
    command->Begin();
    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
        .Target = textureObject.get(), .Before = render::TextureState::Undefined, .After = render::TextureState::CopyDestination};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->CopyBufferToTexture(textureObject.get(), {0, 1, 0, 1}, upload.Get(), 0);
    barrier = render::BarrierTextureDescriptor{
        .Target = textureObject.get(), .Before = render::TextureState::CopyDestination, .After = render::TextureState::ShaderRead};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->End();
    render::CommandBuffer* commands[]{command.Get()};
    queue->Submit({.CmdBuffers = commands});
    queue->Wait();
    return make_unique<TextureAsset>(
        &device,
        "forward-pipeline-test-texture",
        std::move(textureObject),
        view.Release());
}

/// Result the test asserts on after the app loop returns.
struct ForwardPipelineRunResult {
    bool InitSucceeded{false};
    bool RetainedAfterDestruction{false};
    bool ReleasedAfterDestruction{false};
    bool NextSnapshotUsesNewValues{false};
    std::atomic<bool> SnapshotRendered{false};
    std::atomic<bool> DestroyedFrameRendered{false};
    bool MeshAssigned{false};
    uint32_t FramesRun{0};
    size_t PipelineStateCount{0};
    // Two-layer cache facts, filled once the first program exists.
    size_t ArtifactsAfterFirst{0};
    size_t ProgramsAfterFirst{0};
    bool SameRequestHits{false};
    bool ReorderedRecipeHits{false};
    bool InactiveRecipeHits{false};
    size_t ArtifactsAfterHits{0};
    size_t ProgramsAfterHits{0};
    bool ActiveRecipeSplitsProgram{false};
    size_t ArtifactsAfterActiveSplit{0};
    size_t ProgramsAfterActiveSplit{0};
    bool PolicySplitsArtifact{false};
    size_t ArtifactsAfterPolicySplit{0};
    bool MissingSourceFailsTwice{false};
    size_t ArtifactsAfterFailure{0};
    bool GoodRequestStillHits{false};
    bool DuplicateAssignmentFails{false};
    size_t ArtifactsAfterDuplicate{0};
    bool SawError{false};
    string FirstError;
};

class ObservedForwardPipeline final : public RenderPipeline {
public:
    ObservedForwardPipeline(unique_ptr<ForwardPipeline> forward,
                            std::function<bool(ForwardPipeline&, const AppUpdateContext&)> prepare,
                            std::function<void(const ForwardPipeline&, uint32_t)> render,
                            std::function<void(RenderPrepareContext&)> extra = {})
        : _forward(std::move(forward)), _prepare(std::move(prepare)), _render(std::move(render)), _extra(std::move(extra)) {}
    void PrepareFrame(RenderPrepareContext& prepare) override {
        const auto& ctx = prepare.App;
        _enabled[ctx.FlightIndex] = !_stopped;
        if (!_stopped) {
            _forward->PrepareFrame(prepare);
            if (_extra) _extra(prepare);
            _stopped = _prepare(*_forward, ctx);
        }
    }
    void Render(RenderPipelineContext& ctx) override {
        if (_enabled[ctx.FlightIndex()]) {
            _forward->Render(ctx);
            _render(*_forward, ctx.FlightIndex());
        }
    }

private:
    unique_ptr<ForwardPipeline> _forward;
    std::function<bool(ForwardPipeline&, const AppUpdateContext&)> _prepare;
    std::function<void(const ForwardPipeline&, uint32_t)> _render;
    std::function<void(RenderPrepareContext&)> _extra;
    array<bool, 2> _enabled{};
    bool _stopped{false};
};

class ForwardPipelineTestApp final : public Application {
public:
    explicit ForwardPipelineTestApp(ForwardPipelineRunResult* result, Scenario scenario) noexcept
        : _result(result), _scenario(scenario) {}

protected:
    void OnInit() override {
        if (GetWorld() == nullptr || GetRenderSystem() == nullptr ||
            GetAssetManager() == nullptr || GetGpuSystem() == nullptr) {
            Fail("runtime services are incomplete");
            return;
        }

        Nullable<unique_ptr<TextureAsset>> texture = MakeWhiteTexture(*GetDevice());
        if (!texture.HasValue()) {
            Fail("texture creation failed");
            return;
        }
        _texture = GetAssetManager()->AddReady<TextureAsset>(
            kTextureId, texture.Release());
        _mesh = GetAssetManager()->Load<StaticMesh>(AssetLoadRequest{
            .Id = kMeshId,
            .Task = LoadStaticMesh(
                GetGpuSystem()->GetFrameUploadScheduler(),
                MakeQuadMeshResource()),
            .DebugName = "forward-pipeline-test-quad"});
        if (!_texture.IsValid() || !_mesh.IsValid()) {
            Fail("asset registration failed");
            return;
        }

        // The pipeline owns its layout recipe: it names the declarations it uploads from a per-frame
        // arena, so this test asks for the same layout the pipeline itself would.
        ShaderProgramRequest request{
            .SourceName = "pipelines/forward/forward.hlsl",
            .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()};
        if (_scenario == Scenario::NonCanonical || _scenario == Scenario::MissingObject) {
            request.SourceName = "forward_groups.hlsl";
            if (_scenario == Scenario::MissingObject) {
                request.Defines.push_back({.Name = "MISSING_FORWARD_OBJECT", .Value = "1"});
                request.LayoutRecipe.D3D12.BufferPlacements.back().Selector.DeclarationName = "ObjectData";
                request.LayoutRecipe.Vulkan.BufferDescriptors.back().Selector.DeclarationName = "ObjectData";
            }
        } else {
            request.Assignments.push_back(shader::KeywordAssignment{.Name = "QUALITY", .Value = "high"});
        }
        if (_scenario == Scenario::NonDynamic) {
            request.LayoutRecipe.D3D12.BufferPlacements.pop_back();
            request.LayoutRecipe.Vulkan.BufferDescriptors.pop_back();
        }
        const Nullable<ShaderProgram*> program =
            GetRenderSystem()->GetOrCreateShaderProgram(request);
        if (!program.HasValue()) {
            Fail("forward shader program creation failed");
            return;
        }
        _program = program.Get();
        if (_scenario == Scenario::Baseline) {
            CheckCacheRules(request, _program);
        }

        Nullable<unique_ptr<Material>> material = Material::Create(
            _program,
            "ForwardMaterial");
        if (!material.HasValue()) {
            Fail("material creation failed");
            return;
        }
        _material = material.Release();
        const render::SamplerDescriptor sampler{
            .MinFilter = render::FilterMode::Linear,
            .MagFilter = render::FilterMode::Linear};
        if (!_material->SetFloat4("BaseColor", Eigen::Vector4f{1, 0, 0, 1}) ||
            !_material->SetTexture("AlbedoTexture", _texture) ||
            !_material->SetSampler("LinearSampler", sampler)) {
            Fail("material parameter setup failed");
            return;
        }

        _cameraActor = GetWorld()->SpawnActor<Actor>();
        CameraComponent* camera = _cameraActor.Get()->AddComponent<CameraComponent>();
        _cameraActor.Get()->SetRootComponent(camera);
        _camera = camera;
        camera->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        camera->SetPerspective(Radian(55.0f), 0.1f, 100.0f);

        _meshActor = GetWorld()->SpawnActor<Actor>();
        _meshComponent = _meshActor.Get()->AddComponent<StaticMeshComponent>();
        _meshActor.Get()->SetRootComponent(_meshComponent.Get());
        _meshComponent.Get()->SetMaterial(0, _material.get());

        // One light of each kind so both loops in FillViewParameters run.
        _dirLightActor = GetWorld()->SpawnActor<Actor>();
        DirectionalLightComponent* dirLight =
            _dirLightActor.Get()->AddComponent<DirectionalLightComponent>();
        _dirLightActor.Get()->SetRootComponent(dirLight);
        dirLight->SetIntensity(3.0f);
        dirLight->SetCastShadow(false);

        _pointLightActor = GetWorld()->SpawnActor<Actor>();
        PointLightComponent* pointLight =
            _pointLightActor.Get()->AddComponent<PointLightComponent>();
        _pointLightActor.Get()->SetRootComponent(pointLight);
        pointLight->SetWorldLocation(Eigen::Vector3f{1.0f, 1.0f, -2.0f});
        pointLight->SetIntensity(2.0f);

        if (_scenario == Scenario::ThreadedStress) {
            auto second = MakeWhiteTexture(*GetDevice());
            ASSERT_TRUE(second.HasValue());
            AssetId secondId = kTextureId;
            secondId = AssetId{0x33333333, 0x3333, 0x4444, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xdd};
            _secondTexture = GetAssetManager()->AddReady<TextureAsset>(secondId, second.Release());
        }
        if (_scenario == Scenario::NonCanonical) {
            const auto bindings = forward_detail::ResolveProgramBindings(*_program);
            ASSERT_TRUE(bindings.has_value());
            const bool d3d12 = GetDevice()->GetBackend() == render::RenderBackend::D3D12;
            EXPECT_EQ(bindings->ViewGroup, d3d12 ? 4u : 2u);
            EXPECT_EQ(bindings->MaterialGroup, d3d12 ? 7u : 5u);
            EXPECT_EQ(bindings->ObjectGroup, d3d12 ? 9u : 8u);
        }
        if (_scenario == Scenario::OffscreenViews) {
            for (uint32_t i = 0; i < 2; ++i) {
                auto target = render::test::MakeRenderTarget(GetDevice(), render::TextureFormat::RGBA8_UNORM, i ? 64 : 96, i ? 96 : 64,
                                                             render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource);
                ASSERT_TRUE(target);
                _offscreenIds.push_back(GetRenderSystem()->GetOutputs().RegisterExternal({fmt::format("Forward Offscreen {}", i), target->Tex.get(), target->View.get()}));
                ASSERT_TRUE(_offscreenIds.back().IsValid());
                _offscreenViews.push_back(AllocateViewStateId());
                _offscreenTargets.push_back(std::move(*target));
            }
        }
        GetRenderSystem()->SetPipeline(make_unique<ObservedForwardPipeline>(
            make_unique<ForwardPipeline>(this, GetWorld()->GetScene(), camera),
            [this](ForwardPipeline& pipeline, const AppUpdateContext& ctx) { return AfterPrepare(pipeline, ctx); },
            [this](const ForwardPipeline& pipeline, uint32_t flight) { AfterRender(pipeline, flight); },
            [this](RenderPrepareContext& prepare) {
                if (_offscreenIds.empty()) return;
                const RenderViewDesc source = GetRenderSystem()->GetFramePlan(prepare.App.FlightIndex).ViewFamilies.front().Views.front();
                for (uint32_t i = 0; i < _offscreenIds.size(); ++i) {
                    auto view = source;
                    view.StateId = _offscreenViews[i];
                    if (i == 1) {
                        view.WorldToView(0, 3) -= 100;
                        view.WorldPosition.x() += 100;
                    }
                    EXPECT_TRUE(prepare.Workloads.AddViewFamily({fmt::format("Offscreen {}", i), _offscreenIds[i], 1, {view}}));
                }
            }));
        _result->InitSucceeded = true;
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        if (!_result->MeshAssigned && _mesh.IsReady() && _meshComponent.HasValue()) {
            _meshComponent.Get()->SetStaticMesh(_mesh);
            _result->MeshAssigned = true;
        } else if (_mesh.IsFaulted() || _mesh.IsCanceled()) {
            Fail("mesh loading failed");
        }

        if (_scenario == Scenario::ThreadedStress && _result->MeshAssigned) {
            if (_result->FramesRun != 0 && _result->FramesRun % 20 == 0) {
                const bool large = (_result->FramesRun / 20) % 2 != 0;
                GetWindowManager()->GetMainWindow()->GetNativeWindow()->SetSize(large ? 400 : 320, large ? 300 : 240);
            }
            if (_result->FramesRun % 8 == 0) {
                GetWorld()->DestroyActor(_meshActor.Get());
                _meshActor = GetWorld()->SpawnActor<Actor>();
                _meshComponent = _meshActor.Get()->AddComponent<StaticMeshComponent>();
                _meshActor.Get()->SetRootComponent(_meshComponent.Get());
                _meshComponent.Get()->SetStaticMesh(_mesh);
                _meshComponent.Get()->SetMaterial(0, _material.get());
            }
            const bool alternate = (_result->FramesRun % 2) != 0;
            _meshComponent.Get()->SetWorldLocation({alternate ? 0.2f : -0.2f, 0.0f, 0.0f});
            EXPECT_TRUE(_material->SetFloat4("BaseColor", alternate ? Eigen::Vector4f{1, 0, 0, 1} : Eigen::Vector4f{0, 1, 0, 1}));
            EXPECT_TRUE(_material->SetTexture("AlbedoTexture", alternate ? _texture : _secondTexture));
            _dirLightActor.Get()->FindComponent<DirectionalLightComponent>()->SetIntensity(alternate ? 2.0f : 3.0f);
        }
        if (_destroyed && ctx.FlightIndex == _destroyedFlight) {
            _result->ReleasedAfterDestruction = GetAssetManager()->GetAssetCount() == 0;
        }
        // Count only frames where the draw actually had geometry, then ask the window
        // to close. There is no headless mode, so this is how the loop terminates.
        if (_result->MeshAssigned) {
            ++_result->FramesRun;
        }
        const uint32_t requiredFrames = _scenario == Scenario::ThreadedStress ? 220 : kFrameCount;
        if ((_result->FramesRun >= requiredFrames &&
             (_scenario != Scenario::DestroyPrepared || _result->ReleasedAfterDestruction)) ||
            _result->SawError) {
            RequestClose();
        }
    }

    void OnShutdown() override {
        if (_scenario == Scenario::ThreadedStress) {
            for (uint32_t flight = 0; flight < 2; ++flight) {
                const auto& stats = GetRenderSystem()->GetPoolStats(flight);
                EXPECT_LE(stats.TextureCount, 2u);
                EXPECT_GT(stats.Hits, 90u);
            }
        }
        for (size_t i = 0; i < _offscreenTargets.size(); ++i) {
            auto& target = _offscreenTargets[i];
            const auto desc = target.Tex->GetDesc();
            auto surface = GetRenderSystem()->GetOutputs().ResolveExternal(_offscreenIds[i]);
            ASSERT_TRUE(surface);
            EXPECT_EQ(surface->CurrentState, render::TextureState::ShaderRead);
            const uint64_t row = Align(uint64_t{desc.Width} * 4, GetDevice()->GetDetail().TextureDataPitchAlignment);
            auto readback = GetDevice()->CreateBuffer({row * desc.Height, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}});
            auto queue = GetDevice()->GetCommandQueue(render::QueueType::Direct);
            ASSERT_TRUE(readback);
            ASSERT_TRUE(queue);
            auto command = GetDevice()->CreateCommandBuffer(queue.Get());
            ASSERT_TRUE(command);
            command->Begin();
            const render::ResourceBarrierDescriptor toCopy = render::BarrierTextureDescriptor{.Target = target.Tex.get(), .Before = surface->CurrentState, .After = render::TextureState::CopySource};
            command->ResourceBarrier(std::span{&toCopy, 1});
            command->CopyTextureToBuffer(readback.Get(), 0, target.Tex.get(), {0, 1, 0, 1});
            const render::ResourceBarrierDescriptor host = render::BarrierBufferDescriptor{.Target = readback.Get(), .Before = render::BufferState::CopyDestination, .After = render::BufferState::HostRead};
            command->ResourceBarrier(std::span{&host, 1});
            command->End();
            auto* raw = command.Get();
            queue->Submit({.CmdBuffers = std::span{&raw, 1}});
            queue->Wait();
            auto* mapped = static_cast<const uint8_t*>(readback->Map(0, row * desc.Height));
            ASSERT_NE(mapped, nullptr);
            readback->InvalidateMappedRange({0, row * desc.Height});
            const auto* pixel = mapped + row * (desc.Height / 2) + (desc.Width / 2) * 4;
            if (i == 0) {
                EXPECT_GT(pixel[0], pixel[1] * 2 + 10);
                EXPECT_LT(pixel[1], 4);
                EXPECT_LT(pixel[2], 4);
            } else {
                EXPECT_NEAR(pixel[0], .025f * 255, 1);
                EXPECT_NEAR(pixel[1], .030f * 255, 1);
                EXPECT_NEAR(pixel[2], .040f * 255, 1);
            }
            readback->Unmap();
            EXPECT_TRUE(GetRenderSystem()->GetOutputs().Unregister(_offscreenIds[i]));
            GetRenderSystem()->GetRenderPassRegistry()->RemoveFramebuffersUsing(target.View.get());
        }
        _offscreenTargets.clear();
        if (_program != nullptr) {
            _result->PipelineStateCount = _program->GetGraphicsPipelineStateCount();
        }
        World* world = GetWorld();
        if (world != nullptr) {
            for (Nullable<Actor*>* actor :
                 {&_meshActor, &_pointLightActor, &_dirLightActor, &_cameraActor}) {
                if (actor->HasValue()) {
                    world->DestroyActor(actor->Get());
                }
                *actor = nullptr;
            }
        }
        _meshComponent = nullptr;
        if (GetRenderSystem() != nullptr) {
            GetRenderSystem()->SetPipeline(nullptr);
        }
        _program = nullptr;
        _material.reset();
        _texture.Reset();
        _secondTexture.Reset();
        _mesh.Reset();
    }

private:
    bool AfterPrepare(ForwardPipeline& pipeline, const AppUpdateContext& ctx) {
        const auto& input = forward_detail::ForwardPipelineTestAccess::Input(pipeline, ctx.FlightIndex);
        if (input.Draws.empty()) {
            return false;
        }
        const auto& material = input.Materials[input.Draws.front().MaterialIndex];
        const auto bindings = forward_detail::ResolveProgramBindings(*material.Program.Get());
        if (!bindings) {
            return false;
        }
        const auto bytes = material.Parameters.GetBufferData(bindings->MaterialBufferIndex);
        _expectedMaterial[ctx.FlightIndex] = {bytes.begin(), bytes.end()};
        ShaderParameterStorage values{&material.Program.Get()->GetParameterLayout()};
        bool warned = false;
        const auto& family = GetRenderSystem()->GetFramePlan(ctx.FlightIndex).ViewFamilies.front();
        auto output = GetRenderSystem()->GetOutputs().Find(family.Output);
        if (!output) return false;
        string reason;
        auto resolved = ResolveRenderViewFamily(family, *output, 0, GetDevice()->GetCapabilities().Limits.MaxTexture2DDimension, reason);
        if (!resolved) return false;
        EXPECT_TRUE(forward_detail::FillViewParameters(values, input, resolved->Views.front(), warned));
        const auto view = values.GetBufferData(bindings->ViewBufferIndex);
        _expectedView[ctx.FlightIndex] = {view.begin(), view.end()};
        if (_scenario == Scenario::SnapshotValues) {
            if (_mutated) {
                EXPECT_TRUE(resolved->Views.front().WorldPosition.isApprox(Eigen::Vector3f{1, 0, -4}));
                const auto* color = material.Program.Get()->GetParameterLayout().Find("BaseColor");
                Eigen::Vector4f packed;
                std::memcpy(packed.data(), bytes.data() + color->ByteOffset, sizeof(float) * 4);
                _result->NextSnapshotUsesNewValues = packed.isApprox(Eigen::Vector4f{0, 1, 0, 1});
            } else {
                _camera.Get()->SetWorldLocation({1, 0, -4});
                EXPECT_TRUE(_material->SetFloat4("BaseColor", Eigen::Vector4f{0, 1, 0, 1}));
                _mutated = true;
            }
        }
        if (_scenario != Scenario::DestroyPrepared) {
            return false;
        }
        _destroyed = true;
        _destroyedFlight = ctx.FlightIndex;
        _destroyedInputs[ctx.FlightIndex] = true;
        for (Nullable<Actor*>* actor : {&_meshActor, &_pointLightActor, &_dirLightActor, &_cameraActor}) {
            GetWorld()->DestroyActor(actor->Get());
            *actor = nullptr;
        }
        _camera = nullptr;
        _meshComponent = nullptr;
        _material.reset();
        _mesh.Reset();
        _texture.Reset();
        GetAssetManager()->Pump();
        _result->RetainedAfterDestruction = GetAssetManager()->GetAssetCount() == 2;
        return true;
    }

    void AfterRender(const ForwardPipeline& pipeline, uint32_t flight) {
        const auto& input = forward_detail::ForwardPipelineTestAccess::Input(pipeline, flight);
        if (input.Draws.empty()) {
            return;
        }
        const auto& material = input.Materials[input.Draws.front().MaterialIndex];
        const auto bindings = forward_detail::ResolveProgramBindings(*material.Program.Get());
        if (!bindings) {
            return;
        }
        const auto bytes = material.Parameters.GetBufferData(bindings->MaterialBufferIndex);
        EXPECT_EQ((vector<byte>{bytes.begin(), bytes.end()}), _expectedMaterial[flight]);
        ShaderParameterStorage values{&material.Program.Get()->GetParameterLayout()};
        bool warned = false;
        const auto& family = GetRenderSystem()->GetFramePlan(flight).ViewFamilies.front();
        auto output = GetRenderSystem()->GetOutputs().Find(family.Output);
        if (!output) return;
        string reason;
        auto resolved = ResolveRenderViewFamily(family, *output, 0, GetDevice()->GetCapabilities().Limits.MaxTexture2DDimension, reason);
        if (!resolved) return;
        EXPECT_TRUE(forward_detail::FillViewParameters(values, input, resolved->Views.front(), warned));
        const auto view = values.GetBufferData(bindings->ViewBufferIndex);
        EXPECT_EQ((vector<byte>{view.begin(), view.end()}), _expectedView[flight]);
        EXPECT_NE(input.Draws.front().Geometry->Vbv.Target, nullptr);
        ASSERT_FALSE(material.Textures.empty());
        EXPECT_TRUE(material.Textures.front().Texture->IsValid());
        _result->SnapshotRendered = true;
        if (_destroyedInputs[flight]) {
            _result->DestroyedFrameRendered = true;
        }
    }

    // The cache rules from ADR-0051: the compiled artifact is identified by source, defines,
    // assignments, the full policy, the target and the toolchain, and a program by that artifact plus
    // the resolved layout hash of the active backend only. Nothing else may split or merge an entry.
    void CheckCacheRules(const ShaderProgramRequest& base, ShaderProgram* program) {
        RenderSystem* system = GetRenderSystem();
        const bool d3d12 = GetDevice()->GetBackend() == render::RenderBackend::D3D12;
        ForwardPipelineRunResult& out = *_result;
        out.ArtifactsAfterFirst = system->GetShaderArtifactCacheSize();
        out.ProgramsAfterFirst = system->GetShaderProgramCacheSize();

        out.SameRequestHits = system->GetOrCreateShaderProgram(base).Get() == program;

        // Modifier order is not identity: resolution canonicalizes it before hashing.
        ShaderProgramRequest reordered = base;
        std::reverse(
            reordered.LayoutRecipe.D3D12.BufferPlacements.begin(),
            reordered.LayoutRecipe.D3D12.BufferPlacements.end());
        std::reverse(
            reordered.LayoutRecipe.Vulkan.BufferDescriptors.begin(),
            reordered.LayoutRecipe.Vulkan.BufferDescriptors.end());
        out.ReorderedRecipeHits = system->GetOrCreateShaderProgram(reordered).Get() == program;

        // A recipe change the active backend cannot see changes no identity at all: no recompile and no
        // second program.
        ShaderProgramRequest inactive = base;
        if (d3d12) {
            inactive.LayoutRecipe.Vulkan.BufferDescriptors.clear();
        } else {
            inactive.LayoutRecipe.D3D12.BufferPlacements.clear();
        }
        out.InactiveRecipeHits = system->GetOrCreateShaderProgram(inactive).Get() == program;
        out.ArtifactsAfterHits = system->GetShaderArtifactCacheSize();
        out.ProgramsAfterHits = system->GetShaderProgramCacheSize();

        // The active backend's own recipe decides the layout, so it splits the program while reusing the
        // compiled artifact.
        ShaderProgramRequest activeRecipe = base;
        if (d3d12) {
            activeRecipe.LayoutRecipe.D3D12.BufferPlacements.clear();
        } else {
            activeRecipe.LayoutRecipe.Vulkan.BufferDescriptors.clear();
        }
        const Nullable<ShaderProgram*> tableProgram =
            system->GetOrCreateShaderProgram(activeRecipe);
        out.ActiveRecipeSplitsProgram =
            tableProgram.HasValue() && tableProgram.Get() != program;
        out.ArtifactsAfterActiveSplit = system->GetShaderArtifactCacheSize();
        out.ProgramsAfterActiveSplit = system->GetShaderProgramCacheSize();

        // The compile policy is part of the artifact identity, so another shader model is another
        // artifact and therefore another program.
        ShaderProgramRequest otherPolicy = base;
        otherPolicy.Policy.ShaderModel = 61;
        const Nullable<ShaderProgram*> recompiled =
            system->GetOrCreateShaderProgram(otherPolicy);
        out.PolicySplitsArtifact = recompiled.HasValue() && recompiled.Get() != program;
        out.ArtifactsAfterPolicySplit = system->GetShaderArtifactCacheSize();

        // A failure is remembered under its own key: asking twice fails twice without adding a second
        // entry, and it leaves every other key alone.
        ShaderProgramRequest missing = base;
        missing.SourceName = "pipelines/forward/does_not_exist.hlsl";
        const bool firstMiss = !system->GetOrCreateShaderProgram(missing).HasValue();
        const bool secondMiss = !system->GetOrCreateShaderProgram(missing).HasValue();
        out.MissingSourceFailsTwice = firstMiss && secondMiss;
        out.ArtifactsAfterFailure = system->GetShaderArtifactCacheSize();
        out.GoodRequestStillHits = system->GetOrCreateShaderProgram(base).Get() == program;

        // A duplicate assignment is a malformed request rather than a property of a shader, so it is
        // reported and never remembered.
        ShaderProgramRequest duplicate = base;
        duplicate.Assignments.push_back(duplicate.Assignments.front());
        out.DuplicateAssignmentFails =
            !system->GetOrCreateShaderProgram(duplicate).HasValue();
        out.ArtifactsAfterDuplicate = system->GetShaderArtifactCacheSize();
    }

    void Fail(std::string_view message) {
        if (!_result->SawError) {
            _result->SawError = true;
            _result->FirstError = string{message};
        }
        RequestClose();
    }

    void RequestClose() {
#if defined(_WIN32)
        WindowManager* windows = GetWindowManager();
        if (windows == nullptr) {
            return;
        }
        AppWindow* main = windows->GetMainWindow();
        if (main == nullptr || main->GetNativeWindow() == nullptr) {
            return;
        }
        auto handle = static_cast<HWND>(main->GetNativeWindow()->GetNativeHandler());
        if (handle != nullptr) {
            ::PostMessageW(handle, WM_CLOSE, 0, 0);
        }
#endif
    }

    ForwardPipelineRunResult* _result;
    Scenario _scenario;
    vector<render::test::RenderTarget> _offscreenTargets;
    vector<RenderOutputId> _offscreenIds;
    vector<ViewStateId> _offscreenViews;
    Nullable<CameraComponent*> _camera{nullptr};
    StreamingAssetRef<TextureAsset> _secondTexture;
    array<vector<byte>, 2> _expectedMaterial;
    array<vector<byte>, 2> _expectedView;
    array<bool, 2> _destroyedInputs{};
    bool _mutated{false};
    bool _destroyed{false};
    uint32_t _destroyedFlight{0};
    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<TextureAsset> _texture;
    unique_ptr<Material> _material;
    ShaderProgram* _program{nullptr};
    Nullable<Actor*> _cameraActor{nullptr};
    Nullable<Actor*> _meshActor{nullptr};
    Nullable<Actor*> _dirLightActor{nullptr};
    Nullable<Actor*> _pointLightActor{nullptr};
    Nullable<StaticMeshComponent*> _meshComponent{nullptr};
};

void RunForwardPipeline(render::RenderBackend backend, Scenario scenario = Scenario::Baseline) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) {
            GTEST_SKIP() << "Backend unavailable";
        }
    }
    test::RuntimeLogCapture logs;
    const std::filesystem::path projectRoot{RADRAY_PROJECT_DIR};
    ForwardPipelineRunResult result;
    ForwardPipelineTestApp app{&result, scenario};
    const ApplicationRuntimeDescriptor descriptor{
        .Backend = backend,
        .EnableValidation = true,
        .Multithreaded = scenario != Scenario::Baseline,
        .AppName = "test_forward_pipeline",
        .EngineName = "RadRay",
        .RenderCachePath = {},
        .AssetRoot = {},
        .ShaderSourceRoot = (scenario == Scenario::NonCanonical || scenario == Scenario::MissingObject)
                                ? projectRoot / "modules/runtime/tests/data"
                                : projectRoot / "shaderlib",
        .ShaderIncludePaths = {projectRoot / "shaderlib"},
        .WindowTitle = "test_forward_pipeline",
        .WindowWidth = 320,
        .WindowHeight = 240,
        .BackBufferCount = 3,
        .FlightDataCount = 2,
        .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
        .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(descriptor), 0);

    EXPECT_FALSE(result.SawError) << result.FirstError;
    EXPECT_TRUE(result.InitSucceeded);
    EXPECT_TRUE(result.MeshAssigned);
    EXPECT_GE(result.FramesRun, kFrameCount);
    // A PSO only exists if ValidateProgram accepted the program, the view and object
    // constants packed, the material set prepared and the draw loop reached
    // GetOrCreateGraphicsPipelineState. One key per (material, geometry, pass).
    const bool invalid = scenario == Scenario::MissingObject || scenario == Scenario::NonDynamic;
    EXPECT_EQ(result.PipelineStateCount, invalid ? 0u : scenario == Scenario::OffscreenViews ? 2u
                                                                                             : 1u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    EXPECT_EQ(logs.DescriptorRewrites.load(), 0u);
    EXPECT_EQ(logs.IncompatiblePrograms.load(), invalid ? 1u : 0u);
    if (!invalid) {
        EXPECT_TRUE(result.SnapshotRendered.load());
    }
    if (scenario == Scenario::ThreadedStress) {
        EXPECT_GE(result.FramesRun, 220u);
    }
    if (scenario == Scenario::DestroyPrepared) {
        EXPECT_TRUE(result.RetainedAfterDestruction);
        EXPECT_TRUE(result.ReleasedAfterDestruction);
        EXPECT_TRUE(result.DestroyedFrameRendered.load());
    }
    if (scenario == Scenario::SnapshotValues) {
        EXPECT_TRUE(result.NextSnapshotUsesNewValues);
    }
    if (scenario != Scenario::Baseline) {
        return;
    }

    // Two layers, two identities: one compiled artifact serves every recipe, and only the active
    // backend's resolved layout adds a program.
    EXPECT_EQ(result.ArtifactsAfterFirst, 1u);
    EXPECT_EQ(result.ProgramsAfterFirst, 1u);
    EXPECT_TRUE(result.SameRequestHits);
    EXPECT_TRUE(result.ReorderedRecipeHits);
    EXPECT_TRUE(result.InactiveRecipeHits);
    EXPECT_EQ(result.ArtifactsAfterHits, 1u);
    EXPECT_EQ(result.ProgramsAfterHits, 1u);
    EXPECT_TRUE(result.ActiveRecipeSplitsProgram);
    EXPECT_EQ(result.ArtifactsAfterActiveSplit, 1u);
    EXPECT_EQ(result.ProgramsAfterActiveSplit, 2u);
    EXPECT_TRUE(result.PolicySplitsArtifact);
    EXPECT_EQ(result.ArtifactsAfterPolicySplit, 2u);
    EXPECT_TRUE(result.MissingSourceFailsTwice);
    EXPECT_EQ(result.ArtifactsAfterFailure, 3u);
    EXPECT_TRUE(result.GoodRequestStillHits);
    EXPECT_TRUE(result.DuplicateAssignmentFails);
    EXPECT_EQ(result.ArtifactsAfterDuplicate, 3u);
}

}  // namespace

#if defined(RADRAY_ENABLE_D3D12)
TEST(RadRayRuntimeForwardPipeline, D3D12DrawsCollectedMeshThroughForwardPipeline) {
    RunForwardPipeline(render::RenderBackend::D3D12);
}
#endif

#if defined(RADRAY_ENABLE_VULKAN)
TEST(RadRayRuntimeForwardPipeline, VulkanDrawsCollectedMeshThroughForwardPipeline) {
    RunForwardPipeline(render::RenderBackend::Vulkan);
}
#endif

TEST(RadRayRuntimeForwardPipeline, D3D12MultithreadedDrawsWhileGameStateChanges) { RunForwardPipeline(render::RenderBackend::D3D12, Scenario::ThreadedStress); }
TEST(RadRayRuntimeForwardPipeline, D3D12OffscreenViewsHaveIndependentPreparedDraws) { RunForwardPipeline(render::RenderBackend::D3D12, Scenario::OffscreenViews); }
TEST(RadRayRuntimeForwardPipeline, VulkanOffscreenViewsHaveIndependentPreparedDraws) { RunForwardPipeline(render::RenderBackend::Vulkan, Scenario::OffscreenViews); }
TEST(RadRayRuntimeForwardPipeline, VulkanMultithreadedDrawsWhileGameStateChanges) { RunForwardPipeline(render::RenderBackend::Vulkan, Scenario::ThreadedStress); }
TEST(RadRayRuntimeForwardPipeline, PreparedFrameSurvivesActorAndMaterialDestruction) { RunForwardPipeline(render::RenderBackend::D3D12, Scenario::DestroyPrepared); }
TEST(RadRayRuntimeForwardPipeline, VulkanPreparedFrameSurvivesActorAndMaterialDestruction) { RunForwardPipeline(render::RenderBackend::Vulkan, Scenario::DestroyPrepared); }
TEST(RadRayRuntimeForwardPipeline, PreparedFrameUsesOldCameraAndMaterialValues) { RunForwardPipeline(render::RenderBackend::D3D12, Scenario::SnapshotValues); }
TEST(RadRayRuntimeForwardBindings, D3D12NonCanonicalGroupsAreUsedVerbatim) { RunForwardPipeline(render::RenderBackend::D3D12, Scenario::NonCanonical); }
TEST(RadRayRuntimeForwardBindings, VulkanNonCanonicalGroupsAreUsedVerbatim) { RunForwardPipeline(render::RenderBackend::Vulkan, Scenario::NonCanonical); }
TEST(RadRayRuntimeForwardBindings, MissingRequiredDeclarationFailsClosed) { RunForwardPipeline(render::RenderBackend::D3D12, Scenario::MissingObject); }
TEST(RadRayRuntimeForwardBindings, NonDynamicRequiredBufferFailsClosed) { RunForwardPipeline(render::RenderBackend::Vulkan, Scenario::NonDynamic); }

}  // namespace radray
