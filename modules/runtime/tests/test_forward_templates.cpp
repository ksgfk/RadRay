#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "forward_pipeline/forward_frame.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <tuple>
#include <gtest/gtest.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/texture_asset.h>
#include <radray/scope_guard.h>

namespace radray {
namespace {
constexpr uint32_t kWarmFrames = 24, kSteadyFrames = 1000, kVariantWarmFrames = 12, kVariantFrames = 24;
constexpr uint32_t kTotalFrames = kWarmFrames + kSteadyFrames + kVariantWarmFrames + kVariantFrames;
enum class TemplateMode { Ldr,
                          Temporal,
                          Msaa };
struct TemplateResult {
    uint32_t Frames{0}, Steady{0}, Variants{0}, DrawFrames{0};
    array<bool, 3> Flights{};
    bool Initialized{false}, Failed{false}, PixelsChecked{false};
};
struct TemplateVertex {
    float Position[3], Normal[3], UV[2];
};
class TemplatePrimitive final : public PrimitiveSceneProxy {
public:
    explicit TemplatePrimitive(Material* material) : DrawMaterial(material) {}
    bool UsesRenderChangeNotifications() const noexcept override { return true; }
    uint64_t GetRenderDataRevision() const noexcept override { return 1; }
    uint64_t GetTransformRevision() const noexcept override { return GetLocalToWorldRevision(); }
    uint32_t GetSectionCount() const noexcept override { return 1; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return {Eigen::Vector3f::Zero(), Eigen::Vector3f::Ones()}; }
    MeshDrawArgs GetDrawArgs(uint32_t) const noexcept override { return {&Geometry, 0, 6, 0}; }
    Nullable<Material*> GetMaterial(uint32_t) const noexcept override { return DrawMaterial; }
    Material* DrawMaterial;
    GpuMesh::DrawData Geometry;
};
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

class TemplateObservedPipeline final : public RenderPipeline {
public:
    TemplateObservedPipeline(unique_ptr<ForwardPipeline> forward, TemplateResult& result, TemplateMode mode)
        : Forward(std::move(forward)), Result(result), Mode(mode) {}
    void CollectScenePolicies(RenderPrepareContext& context) override { Forward->CollectScenePolicies(context); }
    void PrepareFrame(RenderPrepareContext& context) override { Forward->PrepareFrame(context); }
    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        Forward->BuildGraph(context, graph, outputs);
    }
    void GraphRecorded(RenderPipelineContext& context, const RenderGraph& graph, RenderGraphExecutionResult execution) override {
        Forward->GraphRecorded(context, graph, execution);
        Result.Failed |= !execution.Success || Forward->Failed();
        ASSERT_TRUE(execution.Success) << graph.GetReport().ToText();
        ASSERT_FALSE(Forward->Failed());
        const uint32_t frame = Result.Frames++;
        Result.Flights[context.FlightIndex()] = true;
        const auto& stats = Forward->GetStageBStats(context.FlightIndex());
        EXPECT_GT(stats.OpaqueCommands, 0u);
        EXPECT_GT(stats.TransparentCommands, 0u);
        EXPECT_GT(stats.Execution.Draws, 0u);
        if (stats.OpaqueCommands && stats.TransparentCommands && stats.Execution.Draws) ++Result.DrawFrames;
        const auto& report = graph.GetReport();
        EXPECT_GE(report.TemplateInstances, Mode == TemplateMode::Ldr ? 3u : 5u);
        const bool steady = frame >= kWarmFrames && frame < kWarmFrames + kSteadyFrames;
        const bool variant = frame >= kWarmFrames + kSteadyFrames + kVariantWarmFrames;
        if (steady || variant) {
            if (Mode == TemplateMode::Temporal) {
                EXPECT_EQ(stats.TemporalViews, 1u) << frame;
                EXPECT_EQ(stats.ValidTemporalHistories, 1u) << frame;
            }
            EXPECT_TRUE(report.CompilePlanReused) << frame;
            EXPECT_EQ(report.ResourceDeclarations, 0u) << frame;
            EXPECT_EQ(report.PassDeclarations, 0u) << frame;
            EXPECT_EQ(report.PortResolveBuilds + report.TemplateMaterializations + report.TemplatePlacementBuilds, 0u) << frame;
            EXPECT_EQ(report.NormalizeBuilds + report.IrBuilds + report.TopologyBuilds + report.StoragePlanBuilds + report.RasterPlanBuilds +
                          report.ExecutionPlanBuilds + report.BarrierTemplateBuilds + report.RoutePlanBuilds,
                      0u)
                << frame;
            EXPECT_EQ(Forward->GetSceneSnapshot(context.FlightIndex()).Stats.PrimitiveBoundsRebuilt, 0u);
            EXPECT_EQ(stats.ObjectValueUpdates, 0u);
            if (steady)
                ++Result.Steady;
            else
                ++Result.Variants;
        }
    }
    unique_ptr<ForwardPipeline> Forward;

private:
    TemplateResult& Result;
    TemplateMode Mode;
};
class TemplateHost final : public Application {
public:
    TemplateHost(TemplateResult& result, TemplateMode mode) : Result(result), Mode(mode) {}

protected:
    void OnInit() override {
        SetRenderGraphRuntimeOptions({RenderValidationMode::Full, RenderGraphReportMode::Counters, false});
        auto texture = MakeWhiteTexture(*GetDevice());
        ASSERT_TRUE(texture);
        White = GetAssetManager()->AddReady<TextureAsset>(AssetId{0x91211111, 0x2222, 0x3333, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb}, texture.Release());
        ASSERT_TRUE(White.IsValid());
        auto* system = GetRenderSystem();
        ShaderProgramRequest litRequest{.SourceName = Mode == TemplateMode::Ldr ? "shaderlib/pipelines/forward/forward.hlsl" : "shaderlib/pipelines/forward/pbr.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()};
        if (Mode == TemplateMode::Ldr) litRequest.Assignments.push_back({.Name = "QUALITY", .Value = "high"});
        const auto lit = system->GetOrCreateShaderProgram(litRequest);
        ASSERT_TRUE(lit);
        MaterialPipelineState state;
        state.Primitive.Cull = render::CullMode::None;
        vector<MaterialPassDesc> passes{{"ForwardLit", lit.Get(), "ForwardMaterial", state}};
        if (Mode == TemplateMode::Ldr) {
            const auto depth = system->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/depth_only.hlsl", .LayoutRecipe = ForwardPipeline::GetDepthOnlyLayoutRecipe()});
            ASSERT_TRUE(depth);
            passes.push_back({"DepthOnly", depth.Get(), "", state});
        } else {
            const auto depth = system->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/depth_normals_motion.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
            const auto shadow = system->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/shadow_caster.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
            ASSERT_TRUE(depth);
            ASSERT_TRUE(shadow);
            passes.push_back({"DepthNormalsMotion", depth.Get(), "ForwardMaterial", state});
            passes.push_back({"ShadowCaster", shadow.Get(), "ForwardMaterial", state});
        }
        auto technique = MaterialTechnique::Create(std::move(passes), "ForwardLit");
        ASSERT_TRUE(technique);
        Technique = technique.Release();
        for (uint32_t index = 0; index < 2; ++index) {
            auto material = Material::Create(Technique.get());
            ASSERT_TRUE(material);
            ASSERT_TRUE(material->SetFloat4("BaseColor", {1, .015f, .005f, index ? .25f : 1.f}));
            ASSERT_TRUE(material->SetTexture("AlbedoTexture", White));
            render::SamplerDescriptor sampler;
            sampler.MinFilter = sampler.MagFilter = render::FilterMode::Linear;
            ASSERT_TRUE(material->SetSampler("LinearSampler", sampler));
            if (Mode != TemplateMode::Ldr) {
                ASSERT_TRUE(material->SetFloat4("Surface", {0, .5f, 0, 0}));
                ASSERT_TRUE(material->SetFloat4("Transmission", {0, 1, 0, 0}));
            }
            if (index) {
                material->SetRenderQueue(RenderQueue::Transparent);
                auto transparent = state;
                transparent.DepthStencil.DepthWriteEnable = false;
                transparent.Blend = render::BlendState::Default();
                transparent.Blend->Color = {render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add};
                ASSERT_TRUE(material->SetPassPipelineState("ForwardLit", transparent));
            }
            Materials.push_back(material.Release());
        }
        const TemplateVertex vertices[]{
            {{-1, -1, 0}, {0, 0, -1}, {0, 1}}, {{1, -1, 0}, {0, 0, -1}, {1, 1}}, {{1, 1, 0}, {0, 0, -1}, {1, 0}}, {{-1, 1, 0}, {0, 0, -1}, {0, 0}}};
        const uint32_t indices[]{0, 2, 1, 0, 3, 2};
        auto vertex = render::test::MakeUploadBuffer(*GetDevice(), std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
        auto index = render::test::MakeUploadBuffer(*GetDevice(), std::as_bytes(std::span{indices}), render::BufferUse::Index);
        ASSERT_TRUE(vertex);
        ASSERT_TRUE(index);
        Vertex = vertex.Release();
        Index = index.Release();
        for (uint32_t i = 0; i < 2; ++i) {
            auto primitive = make_unique<TemplatePrimitive>(Materials[i].get());
            primitive->Geometry.VertexBuffers = {{0, {Vertex.get(), 0, sizeof(vertices)}}};
            primitive->Geometry.Ibv = {Index.get(), 0, 4};
            primitive->Geometry.VertexLayout.Buffers = {{0, sizeof(TemplateVertex), render::VertexStepMode::Vertex}};
            primitive->Geometry.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}, {"NORMAL", 0, 0, 12, render::VertexFormat::FLOAT32X3}, {"TEXCOORD", 0, 0, 24, render::VertexFormat::FLOAT32X2}};
            Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
            transform(2, 3) = i ? -.01f : 0;
            primitive->SetLocalToWorld(transform);
            Primitives.push_back(primitive.get());
            GetWorld()->GetScene()->AddPrimitive(std::move(primitive));
        }
        auto actor = GetWorld()->SpawnActor<Actor>();
        auto* camera = actor->AddComponent<CameraComponent>();
        actor->SetRootComponent(camera);
        camera->SetWorldLocation({0, 0, -3});
        camera->SetPerspective(Radian(55.f), .1f, 20.f);
        auto lightActor = GetWorld()->SpawnActor<Actor>();
        auto* light = lightActor->AddComponent<DirectionalLightComponent>();
        lightActor->SetRootComponent(light);
        light->SetIntensity(3);
        light->SetCastShadow(true);
        for (uint32_t i = 0; i < 2; ++i) {
            auto target = render::test::MakeRenderTarget(GetDevice(), render::TextureFormat::RGBA8_UNORM, 48, 32,
                                                         render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource);
            ASSERT_TRUE(target);
            Outputs[i] = system->GetOutputs().RegisterExternal({fmt::format("Forward template {}", i), target->Tex.get(), target->View.get()});
            ASSERT_TRUE(Outputs[i].IsValid());
            Targets.push_back(std::move(*target));
        }
        auto forward = make_unique<ForwardPipeline>(this, GetWorld()->GetScene(), camera);
        auto observed = make_unique<TemplateObservedPipeline>(std::move(forward), Result, Mode);
        Pipeline = observed.get();
        system->SetPipeline(std::move(observed));
        Result.Initialized = true;
        UpdateInputs();
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (!Result.Initialized || Result.Failed || Result.Frames >= kTotalFrames || ++Ticks > kTotalFrames + 10) {
            test::CloseMainWindow(*this);
            return;
        }
        UpdateInputs();
    }
    void UpdateInputs() {
        auto settings = Mode == TemplateMode::Temporal ? ForwardPipelineSettings::Temporal() : Mode == TemplateMode::Msaa ? ForwardPipelineSettings::Msaa()
                                                                                                                          : ForwardPipelineSettings{};
        if (settings.Hdr) {
            settings.ShadowResolution = 32;
            settings.MaxLocalLights = 8;
            settings.MaxLightsPerTile = 8;
            settings.Fireflies = true;
            settings.Exposure = 1 + .15f * std::sin(float(Result.Frames) * .03f);
            settings.AoRadius = .75f + .1f * std::sin(float(Result.Frames) * .02f);
            settings.ShadowDistance = 10 + .2f * std::sin(float(Result.Frames) * .01f);
            settings.BloomStrength = .06f + .01f * std::sin(float(Result.Frames) * .025f);
        }
        const bool variant = Result.Frames >= kWarmFrames + kSteadyFrames && Result.Frames % 2;
        if (settings.Hdr && variant) settings.Bloom = false;
        ASSERT_TRUE(Pipeline->Forward->SetSettings(settings));
        vector<ForwardViewSource> sources;
        for (uint32_t i = 0; i < 2; ++i) {
            RenderViewDesc view;
            view.Name = i ? "observer" : "primary";
            view.StateId = Views[i];
            view.WorldPosition = {.03f * std::sin(float(Result.Frames) * .017f), 0, -3};
            view.WorldToView = LookAtFrontLH(view.WorldPosition, Eigen::Vector3f::UnitZ().eval(), Eigen::Vector3f::UnitY().eval());
            view.Projection = PerspectiveProjectionDesc{Radian(55.f), .1f, 20.f};
            if (Mode == TemplateMode::Ldr && variant && i == 0) view.ViewRect = view.ScissorRect = {0, 0, .5f, 1};
            sources.push_back({Outputs[i], view, i != 0});
            if (Mode == TemplateMode::Ldr && variant && i == 0) {
                view.StateId = Views[2];
                view.ViewRect = view.ScissorRect = {.5f, 0, .5f, 1};
                sources.push_back({Outputs[i], view, false});
            }
        }
        ASSERT_TRUE(Pipeline->Forward->SetViews(sources));
        if (settings.Hdr) {
            ForwardOutputSurface surface{Outputs[1], Outputs[0]};
            surface.LocalToWorld(0, 3) = 2.5f;
            surface.Brightness = .8f;
            ASSERT_TRUE(Pipeline->Forward->SetOutputSurfaces(std::span{&surface, 1}));
            const ForwardOutputOverlay overlay{Outputs[1], Outputs[0], {.75f, .05f, .2f, .2f}};
            ASSERT_TRUE(Pipeline->Forward->SetOutputOverlays(std::span{&overlay, 1}));
        }
    }
    void OnShutdown() override {
        auto cleanup = MakeScopeGuard([this]() noexcept { ReleaseTestResources(); });
        if (!Result.Initialized || Result.Failed) return;
        // The observer skips Bloom, so primary A/B feature changes cannot mask its mesh pixels.
        auto& target = Targets[1];
        const auto surface = GetRenderSystem()->GetOutputs().ResolveExternal(Outputs[1]);
        ASSERT_TRUE(surface);
        const auto desc = target.Tex->GetDesc();
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
        auto* bytes = static_cast<const uint8_t*>(readback->Map(0, row * desc.Height));
        ASSERT_NE(bytes, nullptr);
        readback->InvalidateMappedRange({0, row * desc.Height});
        const auto* center = bytes + row * (desc.Height / 2) + (desc.Width / 2) * 4;
        EXPECT_GT(center[0], center[1] + 20);
        EXPECT_GT(center[0], center[2] + 20);
        readback->Unmap();
        Result.PixelsChecked = true;
    }

private:
    void ReleaseTestResources() noexcept {
        auto* system = GetRenderSystem();
        if (system) system->SetPipeline(nullptr);
        Pipeline = nullptr;
        if (auto* world = GetWorld()) {
            for (auto* primitive : Primitives) world->GetScene()->RemovePrimitive(primitive);
        }
        Primitives.clear();
        for (size_t i = 0; i < Targets.size(); ++i) {
            if (system) {
                EXPECT_TRUE(system->GetOutputs().Unregister(Outputs[i]));
                system->GetRenderPassRegistry()->RemoveFramebuffersUsing(Targets[i].View.get());
            }
        }
        Targets.clear();
        Materials.clear();
        Technique.reset();
        White.Reset();
        Index.reset();
        Vertex.reset();
    }
    TemplateResult& Result;
    TemplateMode Mode;
    uint32_t Ticks{0};
    Nullable<TemplateObservedPipeline*> Pipeline{nullptr};
    array<RenderOutputId, 2> Outputs;
    array<ViewStateId, 3> Views{AllocateViewStateId(), AllocateViewStateId(), AllocateViewStateId()};
    vector<render::test::RenderTarget> Targets;
    unique_ptr<render::Buffer> Vertex, Index;
    StreamingAssetRef<TextureAsset> White;
    unique_ptr<MaterialTechnique> Technique;
    vector<unique_ptr<Material>> Materials;
    vector<TemplatePrimitive*> Primitives;
};
class ForwardTemplatesTest : public testing::TestWithParam<std::tuple<render::RenderBackend, TemplateMode>> {};
TEST_P(ForwardTemplatesTest, ProductGraphReusesAllStagesFor1000ChangingValueFramesAndWarmVariants) {
    const auto [backend, mode] = GetParam();
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device, true)) GTEST_SKIP() << device.Reason;
    }
    TemplateResult result;
    test::RuntimeLogCapture logs;
    TemplateHost app{result, mode};
    const std::filesystem::path root{RADRAY_PROJECT_DIR};
    ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = false, .ShaderSourceRoot = root, .ShaderIncludePaths = {root / "shaderlib"}, .WindowTitle = "Forward product templates", .WindowWidth = 64, .WindowHeight = 48, .BackBufferCount = 3, .FlightDataCount = 3, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate}), 0);
    EXPECT_TRUE(result.Initialized);
    EXPECT_FALSE(result.Failed);
    EXPECT_TRUE(result.PixelsChecked);
    EXPECT_GE(result.Steady, kSteadyFrames);
    EXPECT_GE(result.Variants, kVariantFrames);
    EXPECT_EQ(result.DrawFrames, result.Frames);
    EXPECT_TRUE(std::all_of(result.Flights.begin(), result.Flights.end(), [](bool seen) { return seen; }));
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}
INSTANTIATE_TEST_SUITE_P(Backends, ForwardTemplatesTest, testing::Combine(testing::Values(render::RenderBackend::D3D12, render::RenderBackend::Vulkan), testing::Values(TemplateMode::Ldr, TemplateMode::Temporal, TemplateMode::Msaa)));
}  // namespace
}  // namespace radray
