#pragma once

#include "runtime_profile_support.h"
#include "runtime_profile_submission.h"
#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "forward_pipeline/forward_capture.h"

#include <radray/profiler.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/application_extension.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/texture_asset.h>

namespace radray::profile {

struct IntegratedFrame {
    uint64_t FrameSerial{0}, BeginNs{0}, AuthoringNs{0}, PrepareNs{0}, ComposeNs{0}, ExecuteNs{0}, ReportSerializationNs{0}, RecordedNs{0}, RetireObservedNs{0};
    uint32_t FlightIndex{0}, Index{0};
    uint64_t MeshDraws{0}, DepthCommands{0}, OpaqueCommands{0}, TransparentCommands{0};
    uint32_t ResolvedViews{0}, AvailableViews{0}, AvailableOutputs{0}, FullViews{0}, AuxiliaryViews{0}, WrittenOutputs{0};
    bool StageCommandsKnown{false}, ReportPassesKnown{false};
    uint32_t ReportExecutedPasses{0};
    RenderSceneSnapshotStats Snapshot;
    uint64_t Draw{0}, DrawIndexed{0}, DrawIndirect{0}, DrawIndexedIndirect{0}, Dispatch{0}, DispatchIndirect{0};
    uint64_t SetPipeline{0}, SetParameters{0}, VertexBuffer{0}, IndexBuffer{0};
    uint64_t SceneEpoch{0}, PublishedPages{0}, PublishedBytes{0}, PublishedMaterialBytes{0}, LegacyMaterialsObserved{0}, LegacyBytesCompared{0}, ObjectValueUpdates{0};
    uint64_t DrawRecordFullSyncs{0}, DrawRecordPrimitivesVisited{0}, DrawRecordCopies{0}, SceneCommits{0}, SnapshotPublications{0}, PendingResourcesObserved{0};
    uint64_t GroupPreparations{0}, RecipeBuilds{0}, SetCreations{0}, SetCacheHits{0}, BufferBytesCopied{0}, SharedBufferUploads{0}, SharedBufferHits{0}, SharedGroupHits{0};
    bool SharedGroupHitsKnown{false};
    bool FrameResourceCountersKnown{false};
    bool PublicationCountersKnown{false}, ObjectValueCountersKnown{false};
    bool CommandCallsKnown{false};
    SubmissionObservation Submission;
    bool Measured{false}, Complete{false};
};

template <typename Stats>
void CaptureStageCommands(const Stats& stats, IntegratedFrame& row) noexcept {
    if constexpr (requires { stats.DepthCommands; stats.OpaqueCommands; stats.TransparentCommands; }) {
        row.DepthCommands = stats.DepthCommands;
        row.OpaqueCommands = stats.OpaqueCommands;
        row.TransparentCommands = stats.TransparentCommands;
        row.StageCommandsKnown = true;
    }
}

template <typename Pipeline>
constexpr std::string_view IntegratedSnapshotMode() {
    if constexpr (requires(Pipeline& pipeline, RenderPrepareContext& context) { pipeline.CollectScenePolicies(context); }) return "scene-publication";
    else return "legacy";
}

class ProfileWorldBoundary final : public ApplicationExtension {
public:
    ProfileWorldBoundary(vector<IntegratedFrame>& frames, array<uint32_t, 2>& indices) : _frames(frames), _indices(indices) {}
    void OnAfterWorldTick(const AppUpdateContext& context) override {
        const auto& row = _frames[_indices[context.FlightIndex]];
        if (row.BeginNs == 0 || row.Complete || !row.Measured) return;
        TraceMarker(row.Index, row.FlightIndex, 0, "world_done");
    }
private:
    vector<IntegratedFrame>& _frames;
    array<uint32_t, 2>& _indices;
};

template <typename Snapshot>
void CapturePublicationCounters(const Snapshot& snapshot, IntegratedFrame& row) noexcept {
    if constexpr (requires { snapshot.SceneEpoch; snapshot.Stats.PublishedPages; }) {
        row.SceneEpoch = snapshot.SceneEpoch;
        row.PublishedPages = snapshot.Stats.PublishedPages;
        row.PublishedBytes = snapshot.Stats.PublishedBytes;
        row.PublishedMaterialBytes = snapshot.Stats.PublishedMaterialBytes;
        row.LegacyMaterialsObserved = snapshot.Stats.LegacyMaterialsObserved;
        row.LegacyBytesCompared = snapshot.Stats.LegacyBytesCompared;
        row.DrawRecordFullSyncs = snapshot.Stats.DrawRecordFullSyncs;
        row.DrawRecordPrimitivesVisited = snapshot.Stats.DrawRecordPrimitivesVisited;
        row.DrawRecordCopies = snapshot.Stats.DrawRecordCopies;
        row.SceneCommits = snapshot.Stats.SceneCommits;
        row.SnapshotPublications = snapshot.Stats.SnapshotPublications;
        row.PendingResourcesObserved = snapshot.Stats.PendingResourcesObserved;
        row.PublicationCountersKnown = true;
    }
}

template <typename Pipeline>
void CaptureFrameResourceCounters(const Pipeline& pipeline, uint32_t flight, IntegratedFrame& row) noexcept {
    if constexpr (requires { pipeline.GetFrameDrawResourceStats(flight); }) {
        const auto stats = pipeline.GetFrameDrawResourceStats(flight);
        row.GroupPreparations = stats.GroupPreparations;
        row.RecipeBuilds = stats.RecipeBuilds;
        row.SetCreations = stats.SetCreations;
        row.SetCacheHits = stats.SetCacheHits;
        row.BufferBytesCopied = stats.BufferBytesCopied;
        row.SharedBufferUploads = stats.SharedBufferUploads;
        row.SharedBufferHits = stats.SharedBufferHits;
        if constexpr (requires { stats.SharedGroupHits; }) {
            row.SharedGroupHits = stats.SharedGroupHits;
            row.SharedGroupHitsKnown = true;
        }
        row.FrameResourceCountersKnown = true;
    }
}

template <typename Stats>
void CaptureObjectValueCounters(const Stats& stats, IntegratedFrame& row) noexcept {
    if constexpr (requires { stats.ObjectValueUpdates; }) {
        row.ObjectValueUpdates = stats.ObjectValueUpdates;
        row.ObjectValueCountersKnown = true;
    }
}

template <typename Report>
void CaptureCommandCalls(const Report& report, IntegratedFrame& row) noexcept {
    if constexpr (requires { report.CommandCalls.Draw; }) {
        const auto& calls = report.CommandCalls;
        row.Draw = calls.Draw;
        row.DrawIndexed = calls.DrawIndexed;
        row.DrawIndirect = calls.DrawIndirect;
        row.DrawIndexedIndirect = calls.DrawIndexedIndirect;
        row.Dispatch = calls.Dispatch;
        row.DispatchIndirect = calls.DispatchIndirect;
        row.SetPipeline = calls.SetPipeline;
        row.SetParameters = calls.SetParameters;
        row.VertexBuffer = calls.VertexBuffer;
        row.IndexBuffer = calls.IndexBuffer;
        row.CommandCallsKnown = true;
    }
}

template <typename Base, typename Pipeline, bool = requires(Pipeline& pipeline, RenderPrepareContext& context) { pipeline.CollectScenePolicies(context); }>
class ProfilePipelinePolicies : public Base {
public:
    unique_ptr<Pipeline> Forward;
};

template <typename Base, typename Pipeline>
class ProfilePipelinePolicies<Base, Pipeline, true> : public Base {
public:
    void CollectScenePolicies(RenderPrepareContext& context) override { Forward->CollectScenePolicies(context); }
    unique_ptr<Pipeline> Forward;
};

class IntegratedPipeline final : public ProfilePipelinePolicies<RenderPipeline, ForwardPipeline> {
public:
    IntegratedPipeline(unique_ptr<ForwardPipeline> forward, vector<IntegratedFrame>& frames, array<uint32_t, 2>& indices, RenderOutputId observerOutput, bool serializeReport)
        : _frames(frames), _indices(indices), _observerOutput(observerOutput), _serializeReport(serializeReport) {
        Forward = std::move(forward);
        if (_serializeReport) {
            _capture.Name = "profile";
            _capture.Directory = ".";
        }
    }

    void PrepareFrame(RenderPrepareContext& context) override {
        auto& row = _frames[_indices[context.App.FlightIndex]];
        const auto begin = TimestampNs();
        Forward->PrepareFrame(context);
        row.PrepareNs = TimestampNs() - begin;
        RADRAY_PROFILE_SCOPE_N("Profile.PrepareObservation");
        row.Snapshot = Forward->GetSceneSnapshot(context.App.FlightIndex).Stats;
        CapturePublicationCounters(Forward->GetSceneSnapshot(context.App.FlightIndex), row);
        CaptureObjectValueCounters(Forward->GetStageBStats(context.App.FlightIndex), row);
        if (row.Measured) TraceMarker(row.Index, row.FlightIndex, 0, "gt_ready");
    }

    void BuildGraph(RenderPipelineContext& context, RenderGraph& graph, std::span<RenderGraphOutputBinding> outputs) override {
        auto& row = _frames[_indices[context.FlightIndex()]];
        row.FrameSerial = context.FrameSerial();
        if (row.Measured) TraceMarker(row.Index, row.FlightIndex, row.FrameSerial, "rt_begin");
        {
            RADRAY_PROFILE_SCOPE_N("Profile.ComposeObservation");
            for (const auto& family : context.ViewFamilies()) {
                row.ResolvedViews += static_cast<uint32_t>(family.Views.size());
                if (!family.OutputAvailable) continue;
                row.AvailableViews += static_cast<uint32_t>(family.Views.size());
                if (family.OutputId == _observerOutput) row.AuxiliaryViews += static_cast<uint32_t>(family.Views.size());
                else row.FullViews += static_cast<uint32_t>(family.Views.size());
            }
            row.AvailableOutputs = static_cast<uint32_t>(context.OutputSurfaces().size());
        }
        const auto begin = TimestampNs();
        Forward->BuildGraph(context, graph, outputs);
        row.ComposeNs = TimestampNs() - begin;
        row.RecordedNs = TimestampNs();
    }

    void GraphRecorded(RenderPipelineContext& context, const RenderGraph& graph, RenderGraphExecutionResult result) override {
        auto& row = _frames[_indices[context.FlightIndex()]];
        row.ExecuteNs = TimestampNs() - row.RecordedNs;
        row.RecordedNs = TimestampNs();
        Forward->GraphRecorded(context, graph, result);
        row.Submission.SampleIndex = row.Index;
        row.Submission.TraceEnabled = row.Measured;
        ObserveSubmission(result, row.FrameSerial, row.FlightIndex, row.Submission);
        if (row.Measured) TraceMarker(row.Index, row.FlightIndex, row.FrameSerial, "rt_recorded");
        const auto& stats = Forward->GetStageBStats(context.FlightIndex());
        row.MeshDraws = stats.Execution.Draws;
        CaptureStageCommands(stats, row);
        for (const auto& surface : context.OutputSurfaces()) row.WrittenOutputs += surface.Written;
        row.ReportPassesKnown = context.GetRuntimeOptions().Report == RenderGraphReportMode::Full;
        if (row.ReportPassesKnown)
            for (const auto& pass : graph.GetReport().Passes) row.ReportExecutedPasses += pass.Executed;
        CaptureCommandCalls(graph.GetReport(), row);
        CaptureFrameResourceCounters(*Forward, context.FlightIndex(), row);
        if (_serializeReport) {
            const auto begin = TimestampNs();
            _capture.CaptureReport(graph.GetReport());
            row.ReportSerializationNs = TimestampNs() - begin;
        }
    }

private:
    vector<IntegratedFrame>& _frames;
    array<uint32_t, 2>& _indices;
    RenderOutputId _observerOutput;
    bool _serializeReport{false};
    forward_detail::ForwardCapture _capture;
};

inline Nullable<unique_ptr<TextureAsset>> MakeProfileWhiteTexture(render::Device& device) {
    auto texture = device.CreateTexture({render::TextureDimension::Dim2D, 1, 1, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
                                         render::MemoryType::Device, render::TextureUse::Resource | render::TextureUse::CopyDestination, {}});
    if (!texture) return nullptr;
    auto view = device.CreateTextureView({texture.Get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::Resource});
    if (!view) return nullptr;
    vector<byte> pixels(size_t(std::max<uint64_t>(device.GetDetail().TextureDataPitchAlignment, 4)), byte{0xff});
    auto upload = render::test::MakeUploadBuffer(device, pixels, render::BufferUse::CopySource);
    const auto queue = device.GetCommandQueue(render::QueueType::Direct);
    if (!upload || !queue) return nullptr;
    auto command = device.CreateCommandBuffer(queue.Get());
    if (!command) return nullptr;
    command->Begin();
    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{.Target = texture.Get(), .Before = render::TextureState::Undefined, .After = render::TextureState::CopyDestination};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->CopyBufferToTexture(texture.Get(), {0, 1, 0, 1}, upload.Get(), 0);
    barrier = render::BarrierTextureDescriptor{.Target = texture.Get(), .Before = render::TextureState::CopyDestination, .After = render::TextureState::ShaderRead};
    command->ResourceBarrier(std::span{&barrier, 1});
    command->End();
    auto* raw = command.Get();
    queue->Submit({.CmdBuffers = std::span{&raw, 1}});
    queue->Wait();
    return make_unique<TextureAsset>(&device, "profile-white-v1", texture.Release(), view.Release());
}

class IntegratedApp final : public Application {
public:
    explicit IntegratedApp(const Options& options) : _options(options), _lowChange(std::getenv("RADRAY_PROFILE_LOW_CHANGE") != nullptr) {
        Frames.resize(size_t(options.Warmup + options.Samples) * options.Rounds + 8);
    }

    vector<IntegratedFrame> Frames;
    uint32_t Completed{0};
    bool Failed{false};
    string Failure;

protected:
    void OnInit() override {
        // A hidden console startup can suppress the native window's first ShowWindow call.
        // This fixture needs its own presentable main output; restore it without activation.
        GetWindowManager()->GetMainWindow()->GetNativeWindow()->Show(NativeWindowShowMode::NoActivate);
        if (!AddExtension(make_unique<ProfileWorldBoundary>(Frames, _flightIndices))) return Fail("profile world boundary extension rejected");
        SetRenderGraphRuntimeOptions(_options.Runtime);
        auto* device = GetDevice();
        auto white = MakeProfileWhiteTexture(*device);
        if (!white) return Fail("white texture setup failed");
        _white = GetAssetManager()->AddReady<TextureAsset>(AssetId{0x55221101, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, white.Release());
        struct Vertex { float Position[3], Normal[3], Uv[2]; };
        const array<Vertex, 3> vertices{{{{-.4f, -.4f, 0}, {0, 0, -1}, {0, 1}}, {{.4f, -.4f, 0}, {0, 0, -1}, {1, 1}}, {{0, .4f, 0}, {0, 0, -1}, {.5f, 0}}}};
        const array<uint32_t, 3> indices{0, 1, 2};
        auto vertexBuffer = render::test::MakeUploadBuffer(*device, std::as_bytes(std::span{vertices}), render::BufferUse::Vertex);
        auto indexBuffer = render::test::MakeUploadBuffer(*device, std::as_bytes(std::span{indices}), render::BufferUse::Index);
        if (!vertexBuffer || !indexBuffer) return Fail("mesh upload setup failed");
        GpuMesh geometry;
        auto& draw = geometry.Draws.emplace_back();
        draw.VertexBuffers = {{0, {vertexBuffer.Get(), 0, sizeof(vertices)}}};
        draw.Ibv = {indexBuffer.Get(), 0, 4};
        draw.VertexLayout.Buffers = {{0, sizeof(Vertex), render::VertexStepMode::Vertex}};
        draw.VertexLayout.Attributes = {{"POSITION", 0, 0, 0, render::VertexFormat::FLOAT32X3}, {"NORMAL", 0, 0, 12, render::VertexFormat::FLOAT32X3}, {"TEXCOORD", 0, 0, 24, render::VertexFormat::FLOAT32X2}};
        geometry.Buffers.push_back(vertexBuffer.Release());
        geometry.Buffers.push_back(indexBuffer.Release());
        _mesh = GetAssetManager()->AddReady<StaticMesh>(AssetId{0x55221102, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}, make_unique<StaticMesh>(MeshResource{}, vector<StaticMeshSection>{{0, 0, 3, 0, 2}}, Eigen::Vector3f{-.4f, -.4f, 0}, Eigen::Vector3f{.4f, .4f, 0}, std::move(geometry)));
        const auto lit = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/pbr.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
        const auto depth = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/depth_normals_motion.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
        const auto shadow = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/shadow_caster.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
        if (!lit || !depth || !shadow) return Fail("profile shader creation failed");
        MaterialPipelineState state;
        state.Primitive.Cull = render::CullMode::None;
        auto technique = MaterialTechnique::Create({{"ForwardLit", lit.Get(), "ForwardMaterial", state}, {"DepthNormalsMotion", depth.Get(), "ForwardMaterial", state}, {"ShadowCaster", shadow.Get(), "ForwardMaterial", state}}, "ForwardLit");
        if (!technique) return Fail("profile technique creation failed");
        _technique = technique.Release();
        for (uint32_t i = 0; i < 8; ++i) {
            auto material = Material::Create(_technique.get());
            if (!material) return Fail("profile material creation failed");
            const bool transparent = i == 7;
            if (!material->SetFloat4("BaseColor", {.15f + float(i) * .1f, .35f, .8f - float(i) * .07f, transparent ? .4f : 1.f}) ||
                !material->SetFloat4("Surface", {float(i % 3) * .3f, .2f + float(i) * .08f, 0, 0}) ||
                !material->SetTexture("AlbedoTexture", _white) || !material->SetSampler("LinearSampler", {})) return Fail("profile material values failed");
            if (transparent) {
                material->SetRenderQueue(RenderQueue::Transparent);
                auto policy = state;
                policy.Blend = render::BlendState::Default();
                policy.Blend->Color = {render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add};
                policy.DepthStencil.DepthWriteEnable = false;
                if (!material->SetPassPipelineState("ForwardLit", policy)) return Fail("profile transparent policy setup failed");
            }
            _materials.push_back(material.Release());
        }
        for (uint32_t i = 0; i < _options.Primitives; ++i) {
            auto transform = Eigen::Matrix4f::Identity().eval();
            transform(0, 3) = float(i % 32) - 15.5f;
            transform(1, 3) = float((i / 32) % 32) - 15.5f;
            transform(2, 3) = float(i / 1024) * .5f;
            auto proxy = make_unique<StaticMeshSceneProxy>(_mesh, vector<Nullable<Material*>>{_materials[i % _materials.size()].get()}, transform);
            _proxies.push_back(proxy.get());
            _transforms.push_back(transform);
            if (!GetWorld()->GetScene()->AddPrimitive(std::move(proxy))) return Fail("profile primitive registration failed");
        }
        auto actor = GetWorld()->SpawnActor<Actor>();
        auto* camera = actor->AddComponent<CameraComponent>();
        actor->SetRootComponent(camera);
        camera->SetWorldLocation({0, 0, -40});
        camera->SetPerspective(Radian(55.f), .1f, 200);
        _actors.push_back(actor);
        auto sunActor = GetWorld()->SpawnActor<Actor>();
        auto* sun = sunActor->AddComponent<DirectionalLightComponent>();
        sunActor->SetRootComponent(sun);
        sun->SetIntensity(3);
        sun->SetCastShadow(true);
        _actors.push_back(sunActor);
        for (uint32_t i = 0; i < 8; ++i) {
            auto lightActor = GetWorld()->SpawnActor<Actor>();
            auto* light = lightActor->AddComponent<PointLightComponent>();
            lightActor->SetRootComponent(light);
            light->SetWorldLocation({float(i % 4) * 8 - 12, float(i / 4) * 12 - 6, -3});
            light->SetAttenuationRadius(12);
            light->SetIntensity(8);
            _actors.push_back(lightActor);
        }
        auto observer = render::test::MakeRenderTarget(device, render::TextureFormat::RGBA8_UNORM, 320, 240,
                                                       render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource);
        if (!observer) return Fail("profile observer output failed");
        _observer = std::move(*observer);
        _observerId = GetRenderSystem()->GetOutputs().RegisterExternal({"profile observer", _observer.Tex.get(), _observer.View.get()});
        if (!_observerId.IsValid()) return Fail("profile observer registration failed");
        auto forward = make_unique<ForwardPipeline>(this, GetWorld()->GetScene(), camera);
        auto settings = ForwardPipelineSettings::Temporal();
        settings.ShadowResolution = 512;
        if (!forward->SetSettings(settings)) return Fail("profile settings rejected");
        const auto mainOutput = GetWindowManager()->GetMainWindow()->GetRenderOutputId();
        RenderViewDesc main;
        main.Name = "profile main";
        main.StateId = AllocateViewStateId();
        main.WorldPosition = {0, 0, -40};
        main.WorldToView = LookAtLH(main.WorldPosition, Eigen::Vector3f::Zero().eval(), Eigen::Vector3f::UnitY().eval());
        main.Projection = PerspectiveProjectionDesc{Radian(55.f), .1f, 200};
        main.ViewRect = main.ScissorRect = {0, 0, .5f, 1};
        auto second = main;
        second.Name = "profile second";
        second.StateId = AllocateViewStateId();
        second.WorldPosition = {9, 5, -42};
        second.WorldToView = LookAtLH(second.WorldPosition, Eigen::Vector3f::Zero().eval(), Eigen::Vector3f::UnitY().eval());
        second.ViewRect = second.ScissorRect = {.5f, 0, .5f, 1};
        auto observerView = main;
        observerView.Name = "profile observer";
        observerView.StateId = AllocateViewStateId();
        observerView.WorldPosition = {0, 12, -40};
        observerView.WorldToView = LookAtLH(observerView.WorldPosition, Eigen::Vector3f::Zero().eval(), Eigen::Vector3f::UnitY().eval());
        observerView.Projection = OrthographicProjectionDesc{40, .1f, 200};
        observerView.ViewRect = observerView.ScissorRect = {};
        const array<ForwardViewSource, 3> views{{{mainOutput, main}, {mainOutput, second}, {_observerId, observerView, true}}};
        const array<ForwardOutputOverlay, 1> overlays{{{_observerId, mainOutput, {.72f, .03f, .25f, .25f}}}};
        if (!forward->SetViews(views) || !forward->SetOutputOverlays(overlays)) return Fail("profile view routing rejected");
        auto pipeline = make_unique<IntegratedPipeline>(std::move(forward), Frames, _flightIndices, _observerId, _options.SerializeReport);
        _pipeline = pipeline.get();
        if (!GetRenderSystem()->SetPipeline(std::move(pipeline))) return Fail("profile pipeline installation failed");
    }

    void OnUpdate(const AppUpdateContext& context) override {
        if (Failed || !_pipeline) return;
        const auto total = (_options.Warmup + _options.Samples) * _options.Rounds;
        if (_nextFrame >= total) {
            test::CloseMainWindow(*this);
        }
        if (_nextFrame >= Frames.size()) return Fail("profile frame limit exceeded");
        auto& row = Frames[_nextFrame];
        row.Index = _nextFrame;
        row.Measured = row.Index < total;
        row.FlightIndex = context.FlightIndex;
        row.BeginNs = TimestampNs();
        _flightIndices[context.FlightIndex] = _nextFrame++;
        if (row.Measured) TraceMarker(row.Index, row.FlightIndex, 0, "authoring_begin");
        {
            RADRAY_PROFILE_SCOPE_N("Profile.Authoring");
            if (_lowChange) {
                for (size_t i = 0; i < std::max<size_t>(1, _proxies.size() / 100); ++i) {
                    auto transform = _transforms[i];
                    transform(0, 3) += float(row.Index % 16) * .002f;
                    _proxies[i]->SetLocalToWorld(transform);
                }
            }
        }
        row.AuthoringNs = TimestampNs() - row.BeginNs;
        if (row.Measured) TraceMarker(row.Index, row.FlightIndex, 0, "authoring_done");
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        if (!_pipeline || Failed) return;
        auto& row = Frames[_flightIndices[completion.FlightIndex]];
        if (!completion.GpuWorkCompleted || row.FrameSerial != completion.FrameSerial) return;
        row.RetireObservedNs = TimestampNs();
        if (row.Measured) TraceMarker(row.Index, row.FlightIndex, row.FrameSerial, "gt_observed");
        if (!row.Complete) ++Completed;
        row.Complete = true;
        if (_pipeline->Forward->Failed()) Fail("profile graph execution failed");
    }

    void OnShutdown() override {
        if (_pipeline && _pipeline->Forward->Failed()) Fail("profile pipeline reported failure");
        GetRenderSystem()->SetPipeline(nullptr);
        _pipeline = nullptr;
        for (auto* proxy : _proxies) GetWorld()->GetScene()->RemovePrimitive(proxy);
        _proxies.clear();
        for (auto* actor : _actors) GetWorld()->DestroyActor(actor);
        _actors.clear();
        if (_observerId.IsValid()) GetRenderSystem()->GetOutputs().Unregister(_observerId);
        if (_observer.View) GetRenderSystem()->GetRenderPassRegistry()->RemoveFramebuffersUsing(_observer.View.get());
        _observer = {};
        _materials.clear();
        _technique.reset();
        _mesh = {};
        _white = {};
    }

private:
    void Fail(std::string_view message) {
        Failed = true;
        if (Failure.empty()) Failure = message;
        test::CloseMainWindow(*this);
    }

    Options _options;
    bool _lowChange{false};
    uint32_t _nextFrame{0};
    array<uint32_t, 2> _flightIndices{};
    Nullable<IntegratedPipeline*> _pipeline{nullptr};
    vector<PrimitiveSceneProxy*> _proxies;
    vector<Eigen::Matrix4f> _transforms;
    vector<Actor*> _actors;
    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<TextureAsset> _white;
    unique_ptr<MaterialTechnique> _technique;
    vector<unique_ptr<Material>> _materials;
    render::test::RenderTarget _observer;
    RenderOutputId _observerId;
};

inline void PrintIntegratedFrames(const Options& options, std::string_view backend, std::span<const IntegratedFrame> frames) {
    const auto period = options.Warmup + options.Samples;
    array<vector<double>, 8> medians;
    const array<std::string_view, 8> names{"authoring", "pipelinePrepare", "composeGraph", "graphExecute", "reportSerialization", "updateToRetireObserved", "updateToSubmitCallback", "updateToCompletionCallback"};
    for (uint32_t round = 0; round < options.Rounds; ++round) {
        array<vector<uint64_t>, 8> phases;
        for (const auto& row : frames) {
            if (!row.Complete || row.Index / period != round) continue;
            const bool warmup = row.Index % period < options.Warmup;
            fmt::print("PROFILE_FRAME {{\"fixture\":\"three-view-forward\",\"backend\":{:?},\"round\":{},\"sampleIndex\":{},\"warmup\":{},\"frameSerial\":{},\"flightIndex\":{},\"beginNs\":{},\"authoringNs\":{},\"pipelinePrepareNs\":{},\"composeNs\":{},\"executeNs\":{},\"recordedNs\":{},\"retireObservedNs\":{},\"actualMeshDraws\":{},\"totalDraws\":null,\"stageCommandsKnown\":{},\"depthCommands\":{},\"opaqueCommands\":{},\"transparentCommands\":{},\"snapshotMaterialBytes\":{},\"drawRecordBuilds\":{},\"retainedAssets\":{},\"inputMaterials\":{},\"inputPrimitives\":{},\"resolvedViewCount\":{},\"viewCount\":{},\"fullViewCount\":{},\"auxiliaryViewCount\":{},\"outputCount\":{},\"writtenOutputCount\":{},\"reportExecutedPassesKnown\":{},\"reportExecutedPasses\":{}}}\n",
                       backend, round, row.Index % period, warmup, row.FrameSerial, row.FlightIndex, row.BeginNs, row.AuthoringNs, row.PrepareNs, row.ComposeNs, row.ExecuteNs, row.RecordedNs, row.RetireObservedNs,
                       row.MeshDraws, row.StageCommandsKnown, row.StageCommandsKnown ? fmt::format("{}", row.DepthCommands) : "null", row.StageCommandsKnown ? fmt::format("{}", row.OpaqueCommands) : "null", row.StageCommandsKnown ? fmt::format("{}", row.TransparentCommands) : "null",
                       row.Snapshot.MaterialBytesCopied, row.Snapshot.DrawRecordBuilds, row.Snapshot.RetainedAssets, row.Snapshot.InputMaterials, row.Snapshot.InputPrimitives,
                       row.ResolvedViews, row.AvailableViews, row.FullViews, row.AuxiliaryViews, row.AvailableOutputs, row.WrittenOutputs, row.ReportPassesKnown, row.ReportPassesKnown ? fmt::format("{}", row.ReportExecutedPasses) : "null");
            fmt::print("PROFILE_COMMAND_CALLS {{\"backend\":{:?},\"round\":{},\"frameSerial\":{},\"flightIndex\":{},\"scope\":\"runtimeToRhi\",\"known\":{},\"draw\":{},\"drawIndexed\":{},\"drawIndirect\":{},\"drawIndexedIndirect\":{},\"dispatch\":{},\"dispatchIndirect\":{},\"setPipeline\":{},\"setParameters\":{},\"vertexBuffer\":{},\"indexBuffer\":{}}}\n",
                       backend, round, row.FrameSerial, row.FlightIndex, row.CommandCallsKnown, row.Draw, row.DrawIndexed, row.DrawIndirect, row.DrawIndexedIndirect, row.Dispatch, row.DispatchIndirect, row.SetPipeline, row.SetParameters, row.VertexBuffer, row.IndexBuffer);
            fmt::print("PROFILE_PUBLICATION {{\"backend\":{:?},\"round\":{},\"frameSerial\":{},\"flightIndex\":{},\"known\":{},\"sceneEpoch\":{},\"publishedPages\":{},\"publishedBytes\":{},\"publishedMaterialBytes\":{},\"legacyMaterialsObserved\":{},\"legacyBytesCompared\":{},\"objectValueUpdatesKnown\":{},\"objectValueUpdates\":{}}}\n",
                       backend, round, row.FrameSerial, row.FlightIndex, row.PublicationCountersKnown, row.SceneEpoch, row.PublishedPages, row.PublishedBytes, row.PublishedMaterialBytes, row.LegacyMaterialsObserved, row.LegacyBytesCompared, row.ObjectValueCountersKnown, row.ObjectValueUpdates);
            fmt::print("PROFILE_RESOURCES {{\"backend\":{:?},\"round\":{},\"frameSerial\":{},\"flightIndex\":{},\"known\":{},\"groupPreparations\":{},\"recipeBuilds\":{},\"setCreations\":{},\"setCacheHits\":{},\"bufferBytesCopied\":{},\"sharedBufferUploads\":{},\"sharedBufferHits\":{},\"sharedGroupHitsKnown\":{},\"sharedGroupHits\":{},\"sceneCountersKnown\":{},\"drawRecordFullSyncs\":{},\"drawRecordPrimitivesVisited\":{},\"drawRecordCopies\":{},\"sceneCommits\":{},\"snapshotPublications\":{},\"pendingResourcesObserved\":{}}}\n",
                       backend, round, row.FrameSerial, row.FlightIndex, row.FrameResourceCountersKnown, row.GroupPreparations, row.RecipeBuilds, row.SetCreations, row.SetCacheHits, row.BufferBytesCopied, row.SharedBufferUploads, row.SharedBufferHits,
                       row.SharedGroupHitsKnown, row.SharedGroupHits, row.PublicationCountersKnown, row.DrawRecordFullSyncs, row.DrawRecordPrimitivesVisited, row.DrawRecordCopies, row.SceneCommits, row.SnapshotPublications, row.PendingResourcesObserved);
            fmt::print("PROFILE_REPORT_SERIALIZATION {{\"backend\":{:?},\"round\":{},\"frameSerial\":{},\"flightIndex\":{},\"enabled\":{},\"ns\":{}}}\n", backend, round, row.FrameSerial, row.FlightIndex, options.SerializeReport, row.ReportSerializationNs);
            const auto& submission = row.Submission;
            const bool submitKnown = submission.Available && submission.SubmitCallbackNs >= row.BeginNs && submission.SubmitCallbackNs != 0;
            const bool completionKnown = submission.Available && submission.CompletionCallbackNs >= row.BeginNs && submission.CompletionCallbackNs != 0;
            fmt::print("PROFILE_TIMELINE {{\"backend\":{:?},\"round\":{},\"frameSerial\":{},\"flightIndex\":{},\"submissionCallbacksAvailable\":{},\"submitCallbackNs\":{},\"completionCallbackNs\":{},\"completionSucceeded\":{},\"retireObservedNs\":{},\"inputToSubmitNs\":{},\"preparationTotalNs\":null,\"gtRenderPreparationNs\":null,\"drawWorkBuildNs\":null,\"workerActiveNs\":null}}\n",
                       backend, round, row.FrameSerial, row.FlightIndex, submission.Available,
                       submitKnown ? fmt::format("{}", submission.SubmitCallbackNs) : "null",
                       completionKnown ? fmt::format("{}", submission.CompletionCallbackNs) : "null",
                       completionKnown ? (submission.CompletionSucceeded ? "true" : "false") : "null", row.RetireObservedNs,
                       submitKnown ? fmt::format("{}", submission.SubmitCallbackNs - row.BeginNs) : "null");
            if (warmup) continue;
            const array<uint64_t, 6> times{row.AuthoringNs, row.PrepareNs, row.ComposeNs, row.ExecuteNs, row.ReportSerializationNs, row.RetireObservedNs - row.BeginNs};
            for (size_t i = 0; i < times.size(); ++i) phases[i].push_back(times[i]);
            if (submitKnown) phases[6].push_back(submission.SubmitCallbackNs - row.BeginNs);
            if (completionKnown) phases[7].push_back(submission.CompletionCallbackNs - row.BeginNs);
        }
        for (size_t i = 0; i < phases.size(); ++i) {
            if (phases[i].empty()) continue;
            const auto p50 = QuantileMs(phases[i], 50);
            medians[i].push_back(p50);
            fmt::print("PROFILE {{\"fixture\":\"three-view-forward\",\"backend\":{:?},\"round\":{},\"stage\":{:?},\"samples\":{},\"p50Ms\":{},\"p95Ms\":{},\"p99Ms\":{}}}\n", backend, round, names[i], phases[i].size(), p50, QuantileMs(phases[i], 95), QuantileMs(phases[i], 99));
        }
    }
    for (size_t i = 0; i < medians.size(); ++i) {
        if (medians[i].empty()) continue;
        const auto cv = CoefficientOfVariation(medians[i]);
        fmt::print("PROFILE_STABILITY {{\"fixture\":\"three-view-forward\",\"backend\":{:?},\"stage\":{:?},\"rounds\":{},\"p50Cv\":{},\"stable\":{}}}\n", backend, names[i], medians[i].size(), cv, medians[i].size() >= 5 && cv <= .05);
    }
}

}  // namespace radray::profile
