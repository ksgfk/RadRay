#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include <radray/render/rhi.h>
#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_scene/scene_writer.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/world_manager.h>

namespace radray {
namespace {

ApplicationRuntimeDescriptor CpuOnly(std::optional<RenderOptions> render, std::optional<WorldOptions> world, std::optional<AssetOptions> asset, uint32_t flights = 2) {
    return {
        .FlightDataCount = flights,
        .Window = std::nullopt,
        .Gpu = std::nullopt,
        .Render = std::move(render),
        .World = std::move(world),
        .Asset = std::move(asset),
    };
}

class MinimalApplication final : public Application {
public:
    explicit MinimalApplication(bool exitInInit = false) : ExitInInit(exitInInit) {}
    bool ExitInInit;
    uint32_t Updates{0};
    uint32_t Completions{0};
    uint32_t Shutdowns{0};

protected:
    void OnInit() override {
        EXPECT_FALSE(GetWindowManager());
        EXPECT_FALSE(GetGpuSystem());
        EXPECT_FALSE(GetRenderSystem());
        EXPECT_FALSE(GetWorldManager());
        EXPECT_FALSE(GetAssetManager());
        if (ExitInInit) RequestExit();
    }
    void OnUpdate(const AppUpdateContext& ctx) override {
        EXPECT_EQ(ctx.FlightIndex, Updates % 2);
        if (Updates == 0) EXPECT_EQ(ctx.LastFrameLatency.count(), 0.0f);
        if (++Updates == 3) RequestExit();
    }
    void OnRender(AppFrameContext&) override { ADD_FAILURE() << "GPU callback in CPU loop"; }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_FALSE(completion.GpuWorkCompleted);
        EXPECT_EQ(completion.FrameSerial, ++Completions);
    }
    void OnShutdown() override { ++Shutdowns; }
};

void CheckEmptyFrameLoopAndExitFromInit() {
    for (bool exitInInit : {false, true}) {
        MinimalApplication app{exitInInit};
        ASSERT_EQ(app.Run(CpuOnly(std::nullopt, std::nullopt, std::nullopt)), 0);
        EXPECT_EQ(app.GetStartupResult().Status, RuntimeStartupStatus::Started) << app.GetStartupResult().Reason;
        EXPECT_EQ(app.Updates, exitInInit ? 0u : 3u);
        EXPECT_EQ(app.Completions, exitInInit ? 0u : 2u);
        EXPECT_EQ(app.Shutdowns, 1u);
    }
}

class WorldOnlyApplication final : public Application {
protected:
    void OnInit() override {
        ASSERT_TRUE(GetWorldManager());
        EXPECT_FALSE(GetRenderSystem());
        _world = GetWorldManager()->CreateWorld();
    }
    void OnUpdate(const AppUpdateContext&) override {
        EXPECT_TRUE(GetWorldManager()->GetWorld(_world));
        RequestExit();
    }
private:
    WorldId _world;
};

void CheckWorldOnly() {
    WorldOnlyApplication app;
    EXPECT_EQ(app.Run(CpuOnly(std::nullopt, WorldOptions{}, std::nullopt)), 0);
    EXPECT_EQ(app.GetStartupResult().Status, RuntimeStartupStatus::Started) << app.GetStartupResult().Reason;
}

class RenderOnlyApplication final : public Application {
protected:
    void OnInit() override {
        ASSERT_TRUE(GetRenderSystem());
        EXPECT_FALSE(GetWorldManager());
        _scene = GetRenderSystem()->CreateSceneGT();
        _shape = GetRenderSystem()->GetSceneWriterGT(_scene)->CreateShape();
    }
    void OnUpdate(const AppUpdateContext&) override {
        if (++_updates == 2) {
            EXPECT_TRUE(GetRenderSystem()->GetSceneRT(_scene)->ContainsShape(_shape));
            RequestExit();
        }
    }
private:
    uint32_t _updates{0};
    SceneId _scene;
    ShapeId _shape;
};

void CheckRenderOnlyUsesCpuSceneDelivery() {
    RenderOnlyApplication app;
    EXPECT_EQ(app.Run(CpuOnly(RenderOptions{}, std::nullopt, std::nullopt)), 0);
    EXPECT_EQ(app.GetStartupResult().Status, RuntimeStartupStatus::Started) << app.GetStartupResult().Reason;
}

class CpuFrameContractApplication final : public Application {
public:
    weak_ptr<int> Deferred;

protected:
    void OnInit() override {
        ASSERT_TRUE(GetAssetManager());
        EXPECT_FALSE(GetGpuSystem());
        auto payload = make_shared<int>(42);
        Deferred = payload;
        GetAssetManager()->DeferDestroy(std::move(payload));
    }
    void OnUpdate(const AppUpdateContext& ctx) override {
        if (_updates == 0) {
            EXPECT_FALSE(Deferred.expired());
            EXPECT_EQ(ctx.LastFrameLatency.count(), 0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        } else {
            EXPECT_TRUE(Deferred.expired());
            EXPECT_EQ(_completions, 1u);
            EXPECT_GE(ctx.LastFrameLatency, std::chrono::milliseconds{1});
            EXPECT_EQ(ctx.LastFrameLatency, GetFrameTimeline().GetLastFrameLatency());
            RequestExit();
        }
        ++_updates;
    }
    void OnRender(AppFrameContext&) override { ADD_FAILURE() << "GPU callback in CPU loop"; }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        EXPECT_FALSE(completion.GpuWorkCompleted);
        EXPECT_EQ(completion.FlightIndex, 0u);
        EXPECT_EQ(completion.FrameSerial, ++_completions);
    }

private:
    uint32_t _updates{0};
    uint64_t _completions{0};
};

void CheckAssetWaitMatchesFrameBoundary() {
    CpuFrameContractApplication app;
    EXPECT_EQ(app.Run(CpuOnly(std::nullopt, std::nullopt, AssetOptions{}, 1)), 0);
    EXPECT_EQ(app.GetStartupResult().Status, RuntimeStartupStatus::Started) << app.GetStartupResult().Reason;
    EXPECT_TRUE(app.Deferred.expired());
}

#if defined(RADRAY_PLATFORM_WINDOWS)
class WindowOnlyApplication final : public Application {
protected:
    void OnInit() override {
        ASSERT_TRUE(GetWindowManager());
        EXPECT_FALSE(GetGpuSystem());
        ASSERT_NE(GetWindowManager()->GetMainWindow(), nullptr);
        EXPECT_EQ(GetWindowManager()->GetMainWindow()->GetSwapChain(), nullptr);
    }
    void OnUpdate(const AppUpdateContext&) override { RequestExit(); }
};

void CheckWindowOnlyHasNoSwapChain() {
    WindowOnlyApplication app;
    const ApplicationRuntimeDescriptor desc{
        .Gpu = std::nullopt,
        .Render = std::nullopt,
        .World = std::nullopt,
        .Asset = std::nullopt,
    };
    EXPECT_EQ(app.Run(desc), 0);
    EXPECT_EQ(app.GetStartupResult().Status, RuntimeStartupStatus::Started) << app.GetStartupResult().Reason;
}
#endif

TEST(ApplicationSystems, SelectedSystemsUseOnlyTheirDependencies) {
    { SCOPED_TRACE("empty"); CheckEmptyFrameLoopAndExitFromInit(); }
    { SCOPED_TRACE("world"); CheckWorldOnly(); }
    { SCOPED_TRACE("render"); CheckRenderOnlyUsesCpuSceneDelivery(); }
    { SCOPED_TRACE("asset"); CheckAssetWaitMatchesFrameBoundary(); }
#if defined(RADRAY_PLATFORM_WINDOWS)
    { SCOPED_TRACE("window"); CheckWindowOnlyHasNoSwapChain(); }
#endif
}

class InvalidStartupApplication final : public Application {
public:
    uint32_t InitCalls{0};
protected:
    void OnInit() override { ++InitCalls; }
};

TEST(ApplicationSystems, InvalidDescriptorsFailBeforeOnInit) {
    InvalidStartupApplication app;
    EXPECT_NE(app.Run({.FlightDataCount = 0}), 0);
    EXPECT_EQ(app.GetStartupResult().Status, RuntimeStartupStatus::InvalidDescriptor) << app.GetStartupResult().Reason;
    EXPECT_EQ(app.InitCalls, 0u);
}

}  // namespace
}  // namespace radray
