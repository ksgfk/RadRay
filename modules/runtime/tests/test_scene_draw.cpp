#include "test_scene_draw.h"
#include "runtime_test_support.h"
#include "gpu_runtime_test_support.h"
#include "scene_draw.h"
#include <radray/runtime/world_manager.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/static_mesh_component.h>

namespace radray::test {

class SceneDrawApp final : public Application {
public:
    uint32_t Views{1};
    bool Drop{false};
    uint32_t Verified{0}, Dropped{0}, ZeroViews{0}, CameraOnlyVerified{0};

protected:
    void OnInit() override {
        auto* device = GetGpuSystem()->GetDevice();
        const auto worldId = GetWorldManager()->CreateWorld();
        _world = GetWorldManager()->GetWorld(worldId).Get();
        GetWorldManager()->RequestRenderConnection(worldId, true);
        auto cube = example::CreateCube(*GetGpuSystem().Get());
        ASSERT_TRUE(cube);
        _mesh = GetAssetManager()->AddReady<StaticMesh>(AssetId{19, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(cube));
        _object = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
        _object->SetStaticMesh(_mesh);
        _cameraActor = _world->SpawnActor();
        _camera = _cameraActor->AddComponent<CameraComponent>();
        _frames.resize(GetGpuSystem()->GetFlightDataCount());
        const render::RenderPassColorAttachmentDescriptor attachment{render::TextureFormat::RGBA8_UNORM, 1, render::LoadAction::Clear, render::StoreAction::Store};
        _pass = device->CreateRenderPass({std::span{&attachment, 1}, {}}).Unwrap();
        _draw = make_unique<example::SceneDraw>();
        ASSERT_TRUE(_draw->Initialize(*GetRenderSystem().Get(), device, _pass.get(), render::TextureFormat::RGBA8_UNORM, static_cast<uint32_t>(_frames.size())));
        _pitch = Align(uint64_t{192} * 4, device->GetDetail().TextureDataPitchAlignment);
        for (auto& frame : _frames) {
            auto image = render::test::MakeRenderTarget(device, render::TextureFormat::RGBA8_UNORM, 192, 96, render::TextureUse::RenderTarget | render::TextureUse::CopySource);
            ASSERT_TRUE(image);
            frame.Image = std::move(*image);
            auto* view = frame.Image.View.get();
            frame.Framebuffer = device->CreateFramebuffer({_pass.get(), std::span{&view, 1}, nullptr, 192, 96, 1}).Unwrap();
            frame.Readback = device->CreateBuffer({.Size = _pitch * 96, .Memory = render::MemoryType::ReadBack, .Usage = render::BufferUse::CopyDestination | render::BufferUse::MapRead}).Unwrap();
        }
    }
    void OnUpdate(const AppUpdateContext& context) override {
        _updateFlight = context.FlightIndex;
        ++_updates;
        if (Drop && _updates == 2) {
            _oldShape = _object->GetShapeId();
            _world->DestroyActor(_object->GetOwner().Get());
            _object = nullptr;
        }
        if (Drop && _updates == 3) {
            _object = _world->SpawnActor()->AddComponent<StaticMeshComponent>();
            _object->SetStaticMesh(_mesh);
            EXPECT_EQ(_object->GetShapeId().Index, _oldShape.Index);
            EXPECT_NE(_object->GetShapeId().Generation, _oldShape.Generation);
        }
        if (_object && _updates <= 11) {
            _object->SetRelativeLocation({_updates % 2 ? -0.65f : 0.65f, 0.45f, 4});
            _object->SetRelativeScale({_updates % 3 ? 1.0f : -1.0f, 1, 1});
        }
        if (_camera) _camera->SetRelativeLocation({float(_updates % 3) * 0.15f, 0, 0});
        if (_updates == 8) {
            _world->DestroyActor(_cameraActor.Get());
            _camera = nullptr;
        }
        if (_updates == 9) {
            _world->RequestReconnect();
            _cameraActor = _world->SpawnActor();
            _camera = _cameraActor->AddComponent<CameraComponent>();
        }
        if (Drop && (_updates == 6 || _updates == 7)) {
            auto* window = GetWindowManager()->GetMainWindow();
            ::ShowWindow(static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler()), _updates == 6 ? SW_MINIMIZE : SW_RESTORE);
            if (_updates == 7) _tasks.Spawn(Resize());
        }
        if (_updates >= 18) RequestExit();
    }
    void OnCollectRenderViews(SceneViewCollector& collector) override {
        if (!_camera || !_object || _updates == 5) return;
        _frames[_updateFlight].ExpectedView = _camera->ComputeViewMatrix();
        _frames[_updateFlight].ExpectedScene = *_world->GetRenderSceneId();
        _frames[_updateFlight].ExpectStationaryObjects = _updates >= 13;
        for (uint32_t i = 0; i < Views; ++i) ASSERT_TRUE(collector.Add(*_camera.Get(), {float(i) / Views, 0, 1.0f / Views, 1}));
    }
    void OnRender(AppFrameContext& context) override {
        std::optional<AppFrameTarget> target;
        if (Drop) {
            target = context.AcquireWindow(GetWindowManager()->GetMainWindow());
            if (!target) return;
        }
        auto* renderer = GetRenderSystem().Get();
        const auto views = renderer->GetFrameViewsRT(context.FlightIndex());
        if (views.empty()) {
            ++ZeroViews;
            if (target) {
                auto* commands = context.AllocateCommandBuffer();
                const render::ResourceBarrierDescriptor present = render::BarrierTextureDescriptor{.Target = target->BackBuffer,
                                                                                                   .Before = target->Window->GetBackBufferState(target->BackBufferIndex),
                                                                                                   .After = render::TextureState::Present};
                commands->ResourceBarrier(std::span{&present, 1});
                context.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
            }
            return;
        }
        auto& frame = _frames[context.FlightIndex()];
        frame.Serial = context.FrameSerial();
        frame.Points.clear();
        array<std::optional<SceneGpuView>, 3> objects;
        const auto beforeBytes = context.GetHostWrites().GetStats().CommittedBytes;
        for (uint32_t i = 0; i < views.size(); ++i) {
            EXPECT_TRUE(views[i].View.isApprox(frame.ExpectedView));
            EXPECT_EQ(views[i].Scene, frame.ExpectedScene);
            objects[i] = renderer->PrepareSceneGpuRT(views[i].Scene, context);
            ASSERT_TRUE(objects[i]);
            EXPECT_EQ(objects[0]->Objects.Target, objects[i]->Objects.Target);
            auto scene = renderer->GetSceneRT(views[i].Scene);
            ASSERT_EQ(scene->GetStaticMeshes().size(), 1u);
            const auto object = scene->GetStaticMesh(scene->GetStaticMeshes()[0]);
            auto resolved = ResolveSceneView(views[i], 192, 96, context.GetDevice()->GetBackend());
            ASSERT_TRUE(resolved);
            const Eigen::Vector4f clip = resolved->ViewProjection * object->LocalToWorld * Eigen::Vector4f{0, 0, 0, 1};
            frame.Points.push_back({int(resolved->Scissor.X + (clip.x() / clip.w() * 0.5f + 0.5f) * resolved->Scissor.Width),
                                    int((0.5f - clip.y() / clip.w() * 0.5f) * 96)});
        }
        EXPECT_EQ(context.GetHostWrites().GetStats().CommittedBytes - beforeBytes, frame.ExpectStationaryObjects ? 0u : 64u);
        auto* commands = context.AllocateCommandBuffer();
        const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{.Target = frame.Image.Tex.get(),
                                                                                          .Before = frame.Executed ? render::TextureState::CopySource : render::TextureState::Undefined,
                                                                                          .After = render::TextureState::RenderTarget};
        commands->ResourceBarrier(std::span{&before, 1});
        const render::ColorClearValue clear{{0, 0, 0, 1}};
        auto encoder = commands->BeginRenderPass({_pass.get(), frame.Framebuffer.get(), std::span{&clear, 1}}).Unwrap();
        for (uint32_t i = 0; i < views.size(); ++i) ASSERT_TRUE(_draw->Draw(*renderer, context, encoder.get(), views[i], *objects[i], i, 192, 96));
        commands->EndRenderPass(std::move(encoder));
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{.Target = frame.Image.Tex.get(), .Before = render::TextureState::RenderTarget, .After = render::TextureState::CopySource};
        commands->ResourceBarrier(std::span{&after, 1});
        commands->CopyTextureToBuffer(frame.Readback.get(), 0, frame.Image.Tex.get(), {0, 1, 0, 1});
        context.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}});
        if (Drop) {
            auto* window = GetWindowManager()->GetMainWindow();
            ASSERT_TRUE(target);
            auto* present = context.AllocateCommandBuffer();
            const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{.Target = target->BackBuffer, .Before = window->GetBackBufferState(target->BackBufferIndex), .After = render::TextureState::Present};
            present->ResourceBarrier(std::span{&barrier, 1});
            context.ReturnCommandBuffers({.CmdBuffers = std::span{&present, 1}}, std::move(target));
            if (context.FrameSerial() == 1 || context.FrameSerial() == 4) {
                auto hwnd = static_cast<HWND>(window->GetNativeWindow()->GetNativeHandler());
                ::ShowWindow(hwnd, SW_HIDE);
                context.SubmitFrame();
                ::ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            }
        }
    }
    void OnRenderFrameComplete(const FlightCompletion& completion) override {
        auto& frame = _frames[completion.FlightIndex];
        if (frame.Serial != completion.FrameSerial) return;
        if (!completion.GpuWorkCompleted) {
            ++Dropped;
            return;
        }
        frame.Executed = true;
        ScopedBufferMap map{frame.Readback.get(), {0, _pitch * 96}};
        ASSERT_TRUE(map);
        const auto* pixels = map.DataAs<uint8_t>();
        for (const auto& point : frame.Points) {
            ASSERT_GE(point.x(), 0);
            ASSERT_LT(point.x(), 192);
            ASSERT_GE(point.y(), 0);
            ASSERT_LT(point.y(), 96);
            const auto* pixel = pixels + _pitch * point.y() + point.x() * 4;
            // The front face has blue = 0.7; reversed winding exposes the blue = 1.0 back face.
            EXPECT_NEAR(pixel[2], 179, 2) << "serial=" << frame.Serial << " at " << point.x() << "," << point.y();
            const auto* opposite = pixels + _pitch * (95 - point.y()) + point.x() * 4;
            EXPECT_EQ(opposite[2], 0) << "image must remain asymmetric";
        }
        ++Verified;
        if (frame.ExpectStationaryObjects) ++CameraOnlyVerified;
    }
    void OnShutdown() override {
        _draw.reset();
        _frames.clear();
        _pass.reset();
        _mesh.Reset();
        if (Drop) EXPECT_TRUE(_resized);
    }

private:
    task<void> Resize() {
        const auto result = co_await GetWindowManager()->SetSize(GetWindowManager()->GetMainWindow()->GetHandle(), 224, 112);
        EXPECT_EQ(result, WindowOperationStatus::Completed);
        _resized = result == WindowOperationStatus::Completed;
    }
    struct Frame {
        render::test::RenderTarget Image;
        unique_ptr<render::Framebuffer> Framebuffer;
        unique_ptr<render::Buffer> Readback;
        vector<Eigen::Vector2i> Points;
        Eigen::Matrix4f ExpectedView{Eigen::Matrix4f::Identity()};
        SceneId ExpectedScene;
        bool ExpectStationaryObjects{false};
        uint64_t Serial{0};
        bool Executed{false};
    };
    Nullable<World*> _world{nullptr};
    Nullable<Actor*> _cameraActor{nullptr};
    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<StaticMeshComponent*> _object{nullptr};
    unique_ptr<example::SceneDraw> _draw;
    unique_ptr<render::RenderPass> _pass;
    vector<Frame> _frames;
    uint64_t _pitch{0};
    uint32_t _updates{0};
    uint32_t _updateFlight{0};
    StreamingAssetRef<StaticMesh> _mesh;
    TaskScope _tasks;
    bool _resized{false};
    ShapeId _oldShape;
};

void RunDraw(render::RenderBackend backend, uint32_t views, bool threaded, bool drop) {
#if !defined(RADRAY_SCENE_JIT_AVAILABLE)
    GTEST_SKIP() << "Scene drawing requires RADRAY_ENABLE_SHADER_JIT";
#endif
    test::RuntimeLogCapture logs;
    SceneDrawApp app;
    app.Views = views;
    app.Drop = drop;
    auto result = test::RunApplication(app, {.Backend = backend, .EnableValidation = true, .Multithreaded = threaded, .EnableSynchronizationValidation = true, .ShaderSourceRoot = RADRAY_SCENE_EXAMPLE_DIR, .ShaderIncludePaths = {RADRAY_SHADERLIB_DIR}, .WindowWidth = 192, .WindowHeight = 96, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::FIFO, .Systems = (drop ? ApplicationSystems{ApplicationSystem::Window} : ApplicationSystems{}) | ApplicationSystem::Gpu | ApplicationSystem::Render | ApplicationSystem::World | ApplicationSystem::Asset, .EnableGpuFrameProfiler = false});
    if (test::CanSkipRuntimeStartup(backend, result.Startup)) GTEST_SKIP() << result.Startup.Reason;
    ASSERT_EQ(result.Startup.Status, RuntimeStartupStatus::Started) << result.Startup.Reason;
    EXPECT_EQ(result.ExitCode, 0);
    EXPECT_GE(app.Verified, 8u);
    EXPECT_GE(app.CameraOnlyVerified, 3u);
    EXPECT_GE(app.ZeroViews, 2u);
    EXPECT_EQ(app.Dropped, drop ? 2u : 0u);
    EXPECT_TRUE(logs.Errors().empty()) << logs.Errors();
}

TEST(SceneDraw, D3D12SingleView) { RunDraw(render::RenderBackend::D3D12, 1, false, false); }
TEST(SceneDraw, VulkanSingleView) { RunDraw(render::RenderBackend::Vulkan, 1, false, false); }
TEST(SceneDraw, D3D12ThreeViewsThreaded) { RunDraw(render::RenderBackend::D3D12, 3, true, false); }
TEST(SceneDraw, VulkanThreeViewsThreaded) { RunDraw(render::RenderBackend::Vulkan, 3, true, false); }
TEST(SceneDraw, D3D12LateDropRetriesLatest) { RunDraw(render::RenderBackend::D3D12, 3, false, true); }
TEST(SceneDraw, VulkanLateDropRetriesLatest) { RunDraw(render::RenderBackend::Vulkan, 3, false, true); }

}  // namespace radray::test
