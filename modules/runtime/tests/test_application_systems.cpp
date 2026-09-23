#include <gtest/gtest.h>

#include <radray/render/rhi.h>
#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/render_scene/scene_writer.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/world_manager.h>

namespace radray {
namespace {

class MinimalApplication final : public Application {
public:
    explicit MinimalApplication(bool exitInInit = false) : ExitInInit(exitInInit) {}
    bool ExitInInit;
    uint32_t Updates{0};
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
        EXPECT_EQ(ctx.LastFrameLatency.count(), 0.0f);
        if (++Updates == 3) RequestExit();
    }
    void OnRender(AppFrameContext&) override { ADD_FAILURE() << "GPU callback in CPU loop"; }
    void OnRenderFrameComplete(const FlightCompletion&) override { ADD_FAILURE() << "GPU completion in CPU loop"; }
    void OnShutdown() override { ++Shutdowns; }
};

void CheckEmptyFrameLoopAndExitFromInit() {
    for (bool exitInInit : {false, true}) {
        MinimalApplication app{exitInInit};
        RuntimeStartupResult startup;
        ASSERT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .Systems = {}}, startup), 0);
        EXPECT_EQ(startup.Status, RuntimeStartupStatus::Started) << startup.Reason;
        EXPECT_EQ(app.Updates, exitInInit ? 0u : 3u);
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
    RuntimeStartupResult startup;
    EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .Systems = ApplicationSystem::World}, startup), 0);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::Started) << startup.Reason;
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
    RuntimeStartupResult startup;
    EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .Systems = ApplicationSystem::Render}, startup), 0);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::Started) << startup.Reason;
}

class AssetOnlyApplication final : public Application {
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
    void OnUpdate(const AppUpdateContext&) override {
        EXPECT_TRUE(Deferred.expired());
        RequestExit();
    }
};

void CheckAssetOnlyRetiresWithoutGpu() {
    AssetOnlyApplication app;
    RuntimeStartupResult startup;
    EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .Systems = ApplicationSystem::Asset}, startup), 0);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::Started) << startup.Reason;
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
    RuntimeStartupResult startup;
    EXPECT_EQ(app.Run({.Backend = render::RenderBackend::D3D12, .Systems = ApplicationSystem::Window}, startup), 0);
    EXPECT_EQ(startup.Status, RuntimeStartupStatus::Started) << startup.Reason;
}
#endif

TEST(ApplicationSystems, SelectedSystemsUseOnlyTheirDependencies) {
    { SCOPED_TRACE("empty"); CheckEmptyFrameLoopAndExitFromInit(); }
    { SCOPED_TRACE("world"); CheckWorldOnly(); }
    { SCOPED_TRACE("render"); CheckRenderOnlyUsesCpuSceneDelivery(); }
    { SCOPED_TRACE("asset"); CheckAssetOnlyRetiresWithoutGpu(); }
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
    ApplicationRuntimeDescriptor descriptors[]{
        {.Backend = render::RenderBackend::D3D12, .FlightDataCount = 0, .Systems = {}},
        {.Backend = render::RenderBackend::D3D12, .Multithreaded = true, .Systems = {}},
        {.Backend = render::RenderBackend::D3D12, .AssetRoot = "unused", .Systems = {}},
        {.Backend = render::RenderBackend::D3D12, .Systems = ApplicationSystems{uint8_t{0x80}}},
    };
    for (const auto& desc : descriptors) {
        InvalidStartupApplication app;
        RuntimeStartupResult startup;
        EXPECT_NE(app.Run(desc, startup), 0);
        EXPECT_EQ(startup.Status, RuntimeStartupStatus::InvalidDescriptor) << startup.Reason;
        EXPECT_EQ(app.InitCalls, 0u);
    }
}

}  // namespace
}  // namespace radray
