#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <thread>

#include <gtest/gtest.h>

#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_system.h>

namespace radray {

struct HostResult {
    int Value{0};
    int UpdatedValue{0};
    uint32_t Unloaded{0};
    uint32_t Destroyed{0};
    uint32_t Prepared{0};
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
    HostTestApp(HostResult* result, bool pipeline, bool retain)
        : _result(result), _pipeline(pipeline), _retain(retain) {}

protected:
    void OnInit() override {
        _result->Initialized = true;
        _result->GameThread = std::this_thread::get_id();
        unique_ptr<Actor> probe = make_unique<TickProbeActor>(_result);
        GetWorld()->SpawnActor(std::move(probe));
        if (_pipeline) {
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

private:
    HostResult* _result;
    bool _pipeline;
    bool _retain;
    uint32_t _updates{0};
};

void RunHost(render::RenderBackend backend, bool threaded, bool pipeline, bool retain) {
    {
        render::test::DeviceContext device;
        if (!render::test::TryCreateDevice(backend, device)) {
            GTEST_SKIP() << "Backend unavailable";
        }
    }
    HostResult result;
    test::RuntimeLogCapture logs;
    HostTestApp app{&result, pipeline, retain};
    ASSERT_EQ(app.Run(ApplicationRuntimeDescriptor{
                  .Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .WindowTitle = "Runtime pipeline host test", .WindowWidth = 160, .WindowHeight = 120, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}),
              0);
    EXPECT_TRUE(result.Initialized);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    EXPECT_EQ(result.PassCount, 1u);
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
TEST(RadRayRuntimeRenderSystem, RetainedAssetLivesUntilFlightReuse) {
    RunHost(render::RenderBackend::D3D12, true, true, true);
}

}  // namespace
}  // namespace radray
