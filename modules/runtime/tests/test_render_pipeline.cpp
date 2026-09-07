#include "runtime_test_support.h"
#include "gpu_test_fixture.h"
#include "gpu_submission_gate.h"

#include <semaphore>
#include <thread>

#include <gtest/gtest.h>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include "forward_pipeline/forward_capture.h"

namespace radray {

struct HostResult {
    int Value{0};
    int UpdatedValue{0};
    uint32_t Unloaded{0};
    uint32_t Destroyed{0};
    uint32_t Prepared{0};
    uint32_t Completed{0};
    std::atomic<uint32_t> Rendered{0};
    std::thread::id GameThread;
    std::thread::id RenderThread;
    bool RetainedOnOtherFlight{false};
    bool ReleasedOnReuse{false};
    bool Initialized{false};
    size_t PassCount{0};
};

class FlightProbeAsset final : public Asset {
public:
    explicit FlightProbeAsset(HostResult* result) : _result(result) {}
    ~FlightProbeAsset() noexcept override {
        EXPECT_EQ(std::this_thread::get_id(), _result->GameThread);
        ++_result->Destroyed;
    }
    void OnUnload(AssetManager&) override {
        EXPECT_EQ(std::this_thread::get_id(), _result->GameThread);
        ++_result->Unloaded;
    }

private:
    HostResult* _result;
};

namespace {

class TickProbeActor final : public Actor {
public:
    explicit TickProbeActor(HostResult* result) : _result(result) {}
    void Tick(float) override { ++_result->Value; }

private:
    HostResult* _result;
};

class ClearPipeline final : public RenderPipeline {
public:
    ClearPipeline(HostResult* result, StreamingAssetRef<FlightProbeAsset> asset)
        : _result(result), _asset(std::move(asset)) {}

    void PrepareFrame(RenderPrepareContext& prepare) override {
        const auto& ctx = prepare.App;
        auto& retained = prepare.RetainedAssets;
        prepare.Workloads.AddPresentationOutputs();
        EXPECT_EQ(std::this_thread::get_id(), _result->GameThread);
        EXPECT_EQ(_result->Value, _result->UpdatedValue + 1);
        _values[ctx.FlightIndex] = _result->Value;
        ++_result->Prepared;
        if (_asset.IsValid()) {
            retained.push_back(_asset.AsAny());
            _asset.Reset();
        }
    }

    void Render(RenderPipelineContext& ctx) override {
        _result->RenderThread = std::this_thread::get_id();
        EXPECT_GT(_values[ctx.FlightIndex()], 0);
        EXPECT_TRUE(ctx.ViewFamilies().empty());
        ASSERT_FALSE(ctx.OutputSurfaces().empty());
        auto graph = ctx.CreateRenderGraph("Non-camera pipeline");
        struct Data {};
        for (const auto& surface : ctx.OutputSurfaces()) {
            const auto color = ctx.ImportOutput(graph, surface.Id);
            graph.AddRasterPass<Data>("clear", [=](Data&, RenderGraphRasterBuilder& builder) { builder.SetColorAttachment(0, color, {.Clear = {{.3f, .5f, .7f, 1}}}); }, +[](const Data&, RenderGraphRasterContext&) {});
        }
        EXPECT_TRUE(ctx.ExecuteGraph(graph).Success);
        ++_result->Rendered;
    }

private:
    HostResult* _result;
    StreamingAssetRef<FlightProbeAsset> _asset;
    array<int, 2> _values{};
};

class HostTestApp final : public Application {
public:
    HostTestApp(HostResult* result, bool pipeline, bool retain, bool delayed = false)
        : _result(result), _pipeline(pipeline), _retain(retain), _delayed(delayed) {}

protected:
    void OnInit() override {
        _result->Initialized = true;
        _result->GameThread = std::this_thread::get_id();
        unique_ptr<Actor> probe = make_unique<TickProbeActor>(_result);
        GetWorld()->SpawnActor(std::move(probe));
        if (_pipeline && !_delayed) {
            StreamingAssetRef<FlightProbeAsset> asset;
            if (_retain) {
                const AssetId id{0x2c85d8d2, 0x7d63, 0x4ca3, 0xb9, 0x42, 0x32, 0xf3, 0x98, 0xad, 0xaf, 0x25};
                asset = GetAssetManager()->AddReady<FlightProbeAsset>(id, make_unique<FlightProbeAsset>(_result));
            }
            GetRenderSystem()->SetPipeline(make_unique<ClearPipeline>(
                _result, std::move(asset)));
        }
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        _result->UpdatedValue = _result->Value;
        if (_delayed && _updates == 2) {
            EXPECT_TRUE(GetRenderSystem()->SetPipeline(make_unique<ClearPipeline>(_result, StreamingAssetRef<FlightProbeAsset>{})));
        }
        if (_retain && _updates == 1) {
            EXPECT_EQ(ctx.FlightIndex, 1u);
            _result->RetainedOnOtherFlight = GetAssetManager()->GetAssetCount() == 1 && _result->Unloaded == 0;
        }
        if (_retain && _updates == 2) {
            EXPECT_EQ(ctx.FlightIndex, 0u);
            _result->ReleasedOnReuse = GetAssetManager()->GetAssetCount() == 0 &&
                                       _result->Unloaded == 1 && _result->Destroyed == 1;
        }
        if (++_updates >= 8 || _result->Rendered.load() >= 4) {
            test::CloseMainWindow(*this);
        }
    }

    void OnShutdown() override {
        _result->PassCount = GetRenderSystem()->GetRenderPassRegistry()->GetRenderPassCount();
    }

    void OnRenderFrameComplete(const FlightCompletion&) override {
        EXPECT_EQ(std::this_thread::get_id(), _result->GameThread);
        ++_result->Completed;
    }

private:
    HostResult* _result;
    bool _pipeline;
    bool _retain;
    bool _delayed;
    uint32_t _updates{0};
};

void RunHost(render::RenderBackend backend, bool threaded, bool pipeline, bool retain, bool delayed = false) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) {
            GTEST_SKIP() << "Backend unavailable";
        }
    }
    HostResult result;
    test::RuntimeLogCapture logs;
    HostTestApp app{&result, pipeline, retain, delayed};
    ASSERT_EQ(app.Run(ApplicationRuntimeDescriptor{
                  .Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .WindowTitle = "Runtime pipeline host test", .WindowWidth = 160, .WindowHeight = 120, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}),
              0);
    EXPECT_TRUE(result.Initialized);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    EXPECT_EQ(result.PassCount, 1u);
    EXPECT_GT(result.Completed, 0u);
    if (pipeline) {
        EXPECT_GT(result.Prepared, 0u);
        EXPECT_GT(result.Rendered.load(), 0u);
        EXPECT_EQ(result.GameThread == result.RenderThread, !threaded);
    }
    if (retain) {
        EXPECT_TRUE(result.RetainedOnOtherFlight);
        EXPECT_TRUE(result.ReleasedOnReuse);
        EXPECT_EQ(result.Unloaded, 1u);
        EXPECT_EQ(result.Destroyed, 1u);
    }
}

class OutputSurfaceContractApp final : public Application {
    void OnInit() override {
        auto* actor = GetWorld()->SpawnActor<Actor>();
        auto* camera = actor->AddComponent<CameraComponent>();
        actor->SetRootComponent(camera);
        ForwardPipeline pipeline{this, GetWorld()->GetScene(), camera};
        ForwardOutputSurface surface{{10}, {20}};
        EXPECT_FALSE(pipeline.SetOutputSurfaces(std::span{&surface, 1}));
        EXPECT_TRUE(pipeline.SetSettings(ForwardPipelineSettings::Temporal()));
        EXPECT_TRUE(pipeline.SetOutputSurfaces(std::span{&surface, 1}));
        EXPECT_FALSE(pipeline.SetSettings({}));
        const array<ForwardOutputSurface, 3> cycle{{{{10}, {20}}, {{20}, {30}}, {{30}, {10}}}};
        EXPECT_FALSE(pipeline.SetOutputSurfaces(cycle));
        surface.Source = surface.Destination;
        EXPECT_FALSE(pipeline.SetOutputSurfaces(std::span{&surface, 1}));
        surface.Source = {10};
        surface.LocalToWorld(0, 0) = std::numeric_limits<float>::quiet_NaN();
        EXPECT_FALSE(pipeline.SetOutputSurfaces(std::span{&surface, 1}));
        surface.LocalToWorld = Eigen::Matrix4f::Identity();
        surface.Brightness = -1;
        EXPECT_FALSE(pipeline.SetOutputSurfaces(std::span{&surface, 1}));
        EXPECT_TRUE(pipeline.SetOutputSurfaces({}));
        EXPECT_TRUE(pipeline.SetSettings({}));
    }
    void OnUpdate(const AppUpdateContext&) override { test::CloseMainWindow(*this); }
};

TEST(RadRayRuntimeForwardPipeline, OutputSurfacesRejectCyclesInvalidValuesAndUnsupportedProfiles) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(render::RenderBackend::D3D12, device)) GTEST_SKIP() << device.Reason;
    device.Reset();
    test::RuntimeLogCapture logs;
    OutputSurfaceContractApp app;
    EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .EnableValidation = true, .WindowWidth = 160, .WindowHeight = 120,
                      .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(RadRayRuntimeRenderPipeline, PrepareFrameRunsAfterWorldTick) {
    RunHost(render::RenderBackend::D3D12, true, true, false);
}
TEST(RadRayRuntimeRenderPipeline, D3D12NonCameraPipelineUsesSameHost) {
    RunHost(render::RenderBackend::D3D12, false, true, false);
}
TEST(RadRayRuntimeRenderPipeline, VulkanNonCameraPipelineUsesSameHost) {
    RunHost(render::RenderBackend::Vulkan, true, true, false);
}
TEST(RadRayRuntimeRenderPipeline, NullPipelineClearsAcquiredTargets) {
    RunHost(render::RenderBackend::D3D12, false, false, false);
}
TEST(RadRayRuntimeRenderPipeline, D3D12FirstPipelineAfterLoadingSingleThread) {
    RunHost(render::RenderBackend::D3D12, false, true, false, true);
}
TEST(RadRayRuntimeRenderPipeline, D3D12FirstPipelineAfterLoadingThreaded) {
    RunHost(render::RenderBackend::D3D12, true, true, false, true);
}
TEST(RadRayRuntimeRenderPipeline, VulkanFirstPipelineAfterLoadingSingleThread) {
    RunHost(render::RenderBackend::Vulkan, false, true, false, true);
}
TEST(RadRayRuntimeRenderPipeline, VulkanFirstPipelineAfterLoadingThreaded) {
    RunHost(render::RenderBackend::Vulkan, true, true, false, true);
}
TEST(RadRayRuntimeRenderSystem, RetainedAssetLivesUntilFlightReuse) {
    RunHost(render::RenderBackend::D3D12, true, true, true);
}

TEST(RadRayRuntimeForwardPipeline, GraphSerializationRequiresAnExplicitCaptureRequest) {
    forward_detail::ForwardCapture capture;
    RenderGraphExecutionReport report;
    capture.CaptureReport(report);
    EXPECT_TRUE(capture.Report.empty());
    EXPECT_TRUE(capture.Dot.empty());
    capture.Directory = "unused-capture-directory";
    capture.CaptureReport(report);
    EXPECT_TRUE(capture.Report.empty());
    capture.Name = "requested-frame";
    capture.CaptureReport(report);
    EXPECT_EQ(capture.Report, report.ToJson());
    EXPECT_EQ(capture.Dot, report.ToDot());
    capture.Name.clear();
    capture.CaptureReport(report);
    EXPECT_TRUE(capture.Report.empty());
    EXPECT_TRUE(capture.Dot.empty());
}

struct OverlapProbe {
    std::binary_semaphore Recording{0}, ContinueRecord{0}, SecondRecord{0};
    std::atomic<uint32_t> Updates{0}, Destroyed{0};
    bool Overlapped{false}, FenceProtectedReuse{false}, GateReleased{false};
    uint32_t Completed{0};
    std::thread::id GameThread;
};

class OverlapPipeline final : public RenderPipeline {
public:
    OverlapPipeline(OverlapProbe& probe, render::Device& device, RenderOutputRegistry& outputs) : _probe(probe), _outputs(outputs) {
        _texture = device.CreateTexture({render::TextureDimension::Dim2D, 4, 4, 1, 1, 1, render::TextureFormat::RGBA8_UNORM,
            render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource, {}}).Release();
        if (!_texture) return;
        _view = device.CreateTextureView({_texture.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM,
            {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget}).Release();
        if (_view) _output = outputs.RegisterExternal({.Name = "Blocked pipeline-owned output", .Texture = _texture.get(), .ColorAttachmentView = _view.get()});
    }
    ~OverlapPipeline() override {
        if (_output.IsValid()) _outputs.Unregister(_output);
        ++_probe.Destroyed;
    }
    void PrepareFrame(RenderPrepareContext& ctx) override {
        _values[ctx.App.FlightIndex] = _probe.Updates.load();
        EXPECT_TRUE(ctx.Workloads.RequestOutput(_output));
    }
    void Render(RenderPipelineContext& ctx) override {
        EXPECT_NE(std::this_thread::get_id(), _probe.GameThread);
        const auto value = _values[ctx.FlightIndex()];
        if (_records++ == 0) {
            _probe.Recording.release();
            EXPECT_TRUE(_probe.ContinueRecord.try_acquire_for(std::chrono::seconds{5}));
            EXPECT_EQ(value, _values[ctx.FlightIndex()]);
        } else if (_records == 2) {
            _probe.SecondRecord.release();
        }
        auto graph = ctx.CreateRenderGraph("runner overlap");
        for (const auto& surface : ctx.OutputSurfaces()) {
            const auto color = ctx.ImportOutput(graph, surface.Id);
            struct Clear {};
            graph.AddRasterPass<Clear>("clear", [=](Clear&, RenderGraphRasterBuilder& builder) {
                builder.SetColorAttachment(0, color);
            }, +[](const Clear&, RenderGraphRasterContext&) {});
        }
        EXPECT_TRUE(ctx.ExecuteGraph(graph).Success);
    }
private:
    OverlapProbe& _probe;
    RenderOutputRegistry& _outputs;
    unique_ptr<render::Texture> _texture;
    unique_ptr<render::TextureView> _view;
    RenderOutputId _output;
    array<uint32_t, 2> _values{};
    uint32_t _records{0};
};

class OverlapApp final : public Application {
public:
    explicit OverlapApp(OverlapProbe& probe) : _probe(probe) {}
protected:
    void OnInit() override {
        _probe.GameThread = std::this_thread::get_id();
        _gate = make_unique<test::GpuSubmissionGate>(*GetDevice(), *GetGpuSystem()->GetMainQueue());
        ASSERT_TRUE(_gate->Fence);
        _marker = GetDevice()->CreateFence().Release();
        _command = GetDevice()->CreateCommandBuffer(GetGpuSystem()->GetMainQueue()).Release();
        ASSERT_TRUE(_marker);
        ASSERT_TRUE(_command);
        _command->Begin();
        _command->End();
        auto* command = _command.get();
        auto* wait = _gate->Fence.get();
        auto* signal = _marker.get();
        uint64_t value = 1;
        GetGpuSystem()->GetMainQueue()->Submit({.CmdBuffers = std::span{&command, 1}, .SignalFences = std::span{&signal, 1},
            .SignalValues = std::span{&value, 1}, .WaitFences = std::span{&wait, 1}, .WaitValues = std::span{&value, 1}});
        ASSERT_TRUE(GetRenderSystem()->SetPipeline(make_unique<OverlapPipeline>(_probe, *GetDevice(), GetRenderSystem()->GetOutputs())));
    }
    void OnUpdate(const AppUpdateContext& ctx) override {
        const auto update = _probe.Updates.fetch_add(1);
        if (update == 1) {
            EXPECT_EQ(ctx.FlightIndex, 1u);
            _probe.Overlapped = _probe.Recording.try_acquire_for(std::chrono::seconds{5});
            auto* installed = GetRenderSystem()->GetPipeline();
            EXPECT_FALSE(GetRenderSystem()->SetPipeline(nullptr));
            EXPECT_EQ(installed, GetRenderSystem()->GetPipeline());
            EXPECT_EQ(_probe.Destroyed.load(), 0u);
            _probe.ContinueRecord.release();
            _observer = std::thread([this] {
                const bool recorded = _probe.SecondRecord.try_acquire_for(std::chrono::seconds{5});
                std::this_thread::sleep_for(std::chrono::milliseconds{100});
                _probe.FenceProtectedReuse = recorded && _probe.Updates.load() == 2 && _marker->GetCompletedValue() == 0;
                _probe.GateReleased = _gate->Release(1);
            });
        }
        if (update >= 4) test::CloseMainWindow(*this);
    }
    void OnRenderFrameComplete(const FlightCompletion&) override {
        EXPECT_EQ(std::this_thread::get_id(), _probe.GameThread);
        ++_probe.Completed;
    }
    void OnShutdown() override {
        if (_observer.joinable()) _observer.join();
        EXPECT_TRUE(GetRenderSystem()->SetPipeline(nullptr));
        _gate.reset();
        _marker.reset();
        _command.reset();
    }
private:
    OverlapProbe& _probe;
    unique_ptr<test::GpuSubmissionGate> _gate;
    unique_ptr<render::Fence> _marker;
    unique_ptr<render::CommandBuffer> _command;
    std::thread _observer;
};

void RunOverlap(render::RenderBackend backend) {
    render::test::DeviceContext device;
    if (!render::test::TryCreateDevice(backend, device)) GTEST_SKIP() << device.Reason;
    device.Reset();
    OverlapProbe probe;
    test::RuntimeLogCapture logs;
    OverlapApp app{probe};
    ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = true,
        .WindowTitle = "Real runner overlap and GPU lifetime", .WindowWidth = 160, .WindowHeight = 120,
        .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
    EXPECT_TRUE(probe.Overlapped);
    EXPECT_TRUE(probe.FenceProtectedReuse);
    EXPECT_TRUE(probe.GateReleased);
    EXPECT_GT(probe.Completed, 0u);
    EXPECT_EQ(probe.Destroyed.load(), 1u);
    EXPECT_EQ(logs.Errors(), "Pipeline replacement requires initialization or the host's GPU-idle shutdown phase\n");
}

TEST(RadRayRuntimeRenderPipeline, D3D12ActualRunnerOverlapsUpdateAndPreservesBlockedGpuOwners) { RunOverlap(render::RenderBackend::D3D12); }
TEST(RadRayRuntimeRenderPipeline, VulkanActualRunnerOverlapsUpdateAndPreservesBlockedGpuOwners) { RunOverlap(render::RenderBackend::Vulkan); }

}  // namespace
}  // namespace radray
