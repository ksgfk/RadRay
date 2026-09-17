#include "runtime_test_support.h"
#include "gpu_test_fixture.h"

#include <semaphore>

#include <radray/scope_guard.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {

TEST(SceneDelivery, EmptyBatchesAcrossFlightsWithoutDrawing) {
    for (uint32_t count : {1u, 2u, 3u}) {
        SCOPED_TRACE(count);
        Application app;
        World world{&app};
        RenderSystem render{&app, count};
        const Scene* scene = &render.GetScene();
        for (uint32_t round = 0; round < 8; ++round) {
            for (uint32_t flight = 0; flight < count; ++flight) {
                render.PrepareFrameGT(world, {.FlightIndex = flight});
            }
            for (uint32_t flight = 0; flight < count; ++flight) {
                render.ConsumeRenderUpdates(flight);
                EXPECT_EQ(&render.GetScene(), scene);
                render.OnFlightCompletedGT({.FlightIndex = flight, .GpuWorkCompleted = false});
            }
        }
    }
}

TEST(SceneDelivery, AbandonDoesNotPublishOrRequireCompletion) {
    Application app;
    World world{&app};
    RenderSystem render{&app, 3};
    render.PrepareFrameGT(world, {.FlightIndex = 0});
    render.PrepareFrameGT(world, {.FlightIndex = 1});
    render.AbandonUnpublishedFramesGT();
    render.PrepareFrameGT(world, {.FlightIndex = 0});
    render.ConsumeRenderUpdates(0);
    render.OnFlightCompletedGT({.FlightIndex = 0});
    render.PrepareFrameGT(world, {.FlightIndex = 2});
    render.AbandonUnpublishedFrameGT(2);
    render.OnShutdown();
    render.OnShutdown();
}

TEST(SceneDeliveryDeathTest, RejectsInvalidFlightIndices) {
    Application app;
    World world{&app};
    RenderSystem render{&app, 1};
    EXPECT_DEATH(render.GetFrameUpdateBatchGT(1), "");
    EXPECT_DEATH(render.PrepareFrameGT(world, {.FlightIndex = 1}), "");
    EXPECT_DEATH(render.ConsumeRenderUpdates(1), "");
    EXPECT_DEATH(render.OnFlightCompletedGT({.FlightIndex = 1}), "");
    EXPECT_DEATH(render.AbandonUnpublishedFrameGT(1), "");
}

#if defined(_WIN32)
class SceneDeliveryApp final : public Application {
public:
    explicit SceneDeliveryApp(bool drainOnExit) : _drainOnExit(drainOnExit) {}
    uint64_t PublishedFrames{0};
    uint64_t Completed{0};
    uint64_t Dropped{0};
    uint64_t Recorded{0};

protected:
    void OnUpdate(const AppUpdateContext&) override {
        ++_updates;
        const auto count = GetGpuSystem()->GetFlightDataCount();
        const bool exit = _drainOnExit ? _updates == count : _updates == 13;
        if (!exit) return;
        if (_drainOnExit && count > 1) {
            // Runner closes window operations after setting its exit flag, before joining RT.
            _tasks.Spawn(ReleaseRenderOnCancellation());
        }
        auto* native = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        ::SendMessageW(static_cast<HWND>(native->GetNativeHandler()), WM_CLOSE, 0, 0);
    }

    void OnRender(AppFrameContext& ctx) override {
        ++Recorded;
        if (_drainOnExit && ctx.FrameSerial() == 1) {
            EXPECT_TRUE(_renderGate.try_acquire_for(std::chrono::seconds{10}));
        }
    }

    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_EQ(completion.FrameSerial, ++Completed);
        if (!completion.GpuWorkCompleted) ++Dropped;
    }

    void OnShutdown() override {
        PublishedFrames = GetGpuSystem()->GetFrameIndex();
        EXPECT_EQ(PublishedFrames, _updates - 1);
        EXPECT_EQ(Completed, PublishedFrames);
    }

private:
    task<void> ReleaseRenderOnCancellation() {
        auto release = MakeScopeGuard([this]() noexcept { _renderGate.release(); });
        co_await GetWindowManager()->SetSize(GetWindowManager()->GetMainWindow()->GetHandle(), 90, 70);
        ADD_FAILURE() << "Exit must cancel the unpublished window operation";
    }

    bool _drainOnExit;
    uint64_t _updates{0};
    std::binary_semaphore _renderGate{0};
    TaskScope _tasks;
};

void RunSceneDelivery(render::RenderBackend backend, bool threaded, bool drainOnExit) {
    {
        render::test::DeviceContext probe;
        if (!render::test::TryCreateDevice(backend, probe)) GTEST_SKIP() << probe.Reason;
    }
    for (uint32_t count : {1u, 2u, 3u}) {
        SCOPED_TRACE(count);
        test::RuntimeLogCapture logs;
        SceneDeliveryApp app{drainOnExit};
        ASSERT_EQ(app.Run({.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .WindowTitle = "Scene delivery", .WindowWidth = 80, .WindowHeight = 60, .FlightDataCount = count, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO}), 0);
        EXPECT_EQ(app.PublishedFrames, drainOnExit ? count - 1 : 12u);
        if (drainOnExit && count == 3) EXPECT_GE(app.Dropped, 1u);
        if (!drainOnExit) EXPECT_EQ(app.Recorded + app.Dropped, app.PublishedFrames);
        EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
    }
}

TEST(SceneDeliveryRunner, D3D12SingleThreadFlights) { RunSceneDelivery(render::RenderBackend::D3D12, false, false); }
TEST(SceneDeliveryRunner, D3D12ThreadedFlights) { RunSceneDelivery(render::RenderBackend::D3D12, true, false); }
TEST(SceneDeliveryRunner, VulkanSingleThreadFlights) { RunSceneDelivery(render::RenderBackend::Vulkan, false, false); }
TEST(SceneDeliveryRunner, VulkanThreadedFlights) { RunSceneDelivery(render::RenderBackend::Vulkan, true, false); }
TEST(SceneDeliveryRunner, D3D12ExitDrainsPublishedFlights) { RunSceneDelivery(render::RenderBackend::D3D12, true, true); }
TEST(SceneDeliveryRunner, VulkanExitDrainsPublishedFlights) { RunSceneDelivery(render::RenderBackend::Vulkan, true, true); }
#endif

}  // namespace
}  // namespace radray
