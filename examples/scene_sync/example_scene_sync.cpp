#include "example_scene_sync.h"

#include <charconv>
#include <cmath>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/world_manager.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/game_framework/actor.h>

namespace radray::example {

void SceneSyncApp::OnInit() {
    auto cube = CreateCube(*GetGpuSystem().Get());
    if (!cube) {
        Failed = true;
        RequestExit();
        return;
    }
    auto mesh = GetAssetManager()->AddReady<StaticMesh>(AssetId{18, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(cube));
    auto worldId = GetWorldManager()->CreateWorld();
    auto world = GetWorldManager()->GetWorld(worldId);
    GetWorldManager()->RequestRenderConnection(worldId, true);
    auto* root = world->SpawnActor();
    _parent = root->AddComponent<SceneComponent>();
    _camera = world->SpawnActor()->AddComponent<CameraComponent>();
    const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(float(InstanceCount))));
    _distance = std::max(8.0f, float(side) * 2.8f);
    _camera->SetPerspective(Radian(60.0f), 0.1f, _distance * 4);
    _camera->SetRelativeLocation({0, 0, -_distance});
    _cameraControl.MaxDistance = _distance * 2;
    _cameraControl.SetOrbitTarget(Eigen::Vector3f::Zero());
    _cameraControl.UpdateDistance(_camera->GetRelativeLocation());
    for (uint32_t i = 0; i < InstanceCount; ++i) {
        auto* object = world->SpawnActor()->AddComponent<StaticMeshComponent>();
        object->SetStaticMesh(mesh);
        if (i % 3 == 0) object->RequestReparent(_parent.Get());
        object->SetRelativeLocation({(float(i % side) - float(side - 1) * 0.5f) * 1.6f,
                                     (float(i) / side - float(side - 1) * 0.5f) * 1.6f, 0});
        if (i % 7 == 0) object->SetRelativeScale({-1, 1, 1});
    }
    auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
    _inputConnections[0] = window->EventTouch().connect([this](int x, int y, MouseButton button, Action action) {
        _cameraControl.CurrentMousePos = {float(x), float(y)};
        if (action == Action::PRESSED || action == Action::RELEASED) {
            const bool pressed = action == Action::PRESSED;
            if (button == MouseButton::BUTTON_LEFT) _cameraControl.IsOrbiting = pressed;
            if (button == MouseButton::BUTTON_RIGHT) _rightMouseDown = pressed;
            if (button == MouseButton::BUTTON_MIDDLE) _middleMouseDown = pressed;
            _cameraControl.IsPanning = _rightMouseDown || _middleMouseDown;
            _cameraControl.LastMousePos = _cameraControl.CurrentMousePos;
        }
    });
    _inputConnections[1] = window->EventScroll().connect([this](float, float vertical) {
        _cameraControl.WheelDelta += vertical;
    });
    _inputConnections[2] = window->EventFocused().connect([this](bool focused) {
        if (!focused) ResetCameraInput();
    });
    _inputConnections[3] = window->EventCaptureLost().connect([this]() { ResetCameraInput(); });
    _fpsFrames.store(0, std::memory_order_relaxed);
    _fpsSampleStart = std::chrono::steady_clock::now();
}
void SceneSyncApp::OnUpdate(const AppUpdateContext& context) {
    RADRAY_PROFILE_PLOT("SceneSync/LastResolvedGpuMs", double(GetGpuSystem()->GetLastGpuTimeMs()));
    if (!_camera) return;
    _time += context.DeltaTime.count();
    _parent->SetRelativeLocation({std::sin(_time) * 0.6f, std::cos(_time) * 0.3f, 0});
    auto position = _camera->GetRelativeLocation();
    auto rotation = _camera->GetRelativeRotation();
    if (_cameraControl.IsOrbiting)
        _cameraControl.Orbit(position, rotation);
    else
        _cameraControl.Pan(position, rotation);
    // CameraComponent faces +Z; CameraControl::Dolly uses -Z. Rotate its local forward by 180 degrees around Y.
    _cameraControl.Dolly(position, rotation * Eigen::Quaternionf{0, 0, 1, 0});
    _cameraControl.LastMousePos = _cameraControl.CurrentMousePos;
    _camera->SetRelativeLocation(position);
    _camera->SetRelativeRotation(rotation);

    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - _fpsSampleStart).count();
    constexpr double kFpsUpdateInterval = 0.5;
    if (elapsed >= kFpsUpdateInterval) {
        const auto frames = _fpsFrames.exchange(0, std::memory_order_relaxed);
        GetWindowManager()->GetMainWindow()->GetNativeWindow()->SetTitle(fmt::format("RadRay Scene Sync | FPS: {:.1f}", frames / elapsed));
        _fpsSampleStart = now;
    }
    if (FrameLimit && ++_updates >= FrameLimit) RequestExit();
}
void SceneSyncApp::OnCollectRenderViews(SceneViewCollector& collector) {
    if (!_camera) return;
    for (uint32_t i = 0; i < ViewCount; ++i)
        collector.Add(*_camera.Get(), {float(i) / ViewCount, 0, 1.0f / ViewCount, 1});
}
void SceneSyncApp::OnRender(AppFrameContext& frame) {
    auto* window = GetWindowManager()->GetMainWindow();
    auto target = frame.AcquireWindow(window);
    if (!target) return;
    auto* renderer = GetRenderSystem().Get();
    const auto desc = target->BackBuffer->GetDesc();
    const render::RenderPassColorAttachmentDescriptor attachment{desc.Format, desc.SampleCount, render::LoadAction::Clear, render::StoreAction::Store};
    auto pass = renderer->GetRenderPassRegistry()->GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
    auto* targetView = target->BackBufferView;
    auto framebuffer = renderer->GetRenderPassRegistry()->GetOrCreateFramebuffer({pass.Get(), std::span{&targetView, 1}, nullptr, desc.Width, desc.Height, 1});
    if (!_draw) {
        _draw = make_unique<SceneDraw>();
        if (!_draw->Initialize(*renderer, frame.GetDevice(), pass.Get(), desc.Format, GetGpuSystem()->GetFlightDataCount())) {
            Failed = true;
            RequestExit();
        }
    }
    const auto views = renderer->GetFrameViewsRT(frame.FlightIndex());
    array<std::optional<SceneGpuView>, 3> objects;
    if (!Failed)
        for (size_t i = 0; i < views.size() && i < objects.size(); ++i) objects[i] = renderer->PrepareSceneGpuRT(views[i].Scene, frame);
    auto* commands = frame.AllocateCommandBuffer();
    const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{.Target = target->BackBuffer,
                                                                                      .Before = window->GetBackBufferState(target->BackBufferIndex),
                                                                                      .After = render::TextureState::RenderTarget};
    commands->ResourceBarrier(std::span{&before, 1});
    const render::ColorClearValue clear{{0.025f, 0.035f, 0.055f, 1}};
    commands->PushDebugGroup("SceneSyncDrawGPU");
    auto encoder = commands->BeginRenderPass({pass.Get(), framebuffer.Get(), std::span{&clear, 1}}).Unwrap();
    for (uint32_t i = 0; i < views.size() && i < objects.size(); ++i) {
        if (objects[i] && !_draw->Draw(*renderer, frame, encoder.get(), views[i], *objects[i], i, desc.Width, desc.Height)) {
            Failed = true;
            RequestExit();
        }
    }
    commands->EndRenderPass(std::move(encoder));
    commands->PopDebugGroup();
    const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{.Target = target->BackBuffer,
                                                                                     .Before = render::TextureState::RenderTarget,
                                                                                     .After = render::TextureState::Present};
    commands->ResourceBarrier(std::span{&after, 1});
    frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
    if (!Failed) _fpsFrames.fetch_add(1, std::memory_order_relaxed);
}
void SceneSyncApp::OnShutdown() {
    for (auto& connection : _inputConnections) connection.disconnect();
    ResetCameraInput();
    _draw.reset();
    _camera = nullptr;
    _parent = nullptr;
}

void SceneSyncApp::ResetCameraInput() noexcept {
    _cameraControl.Reset();
    _rightMouseDown = false;
    _middleMouseDown = false;
}

}  // namespace radray::example

int main(int argc, char** argv) {
    using namespace radray;
    example::SceneSyncApp app;
    ApplicationRuntimeDescriptor descriptor{
        .FlightDataCount = 2,
        .Window = WindowOptions{.Title = "RadRay Scene Sync"},
        .Gpu = GpuOptions{
            .Backend = render::RenderBackend::D3D12,
            .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
            .PresentMode = render::PresentMode::Immediate,
        },
        .Render = RenderOptions{.ShaderSourceRoot = RADRAY_SCENE_EXAMPLE_DIR, .ShaderIncludePaths = {RADRAY_SHADERLIB_DIR}},
    };
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--vulkan")
            descriptor.Gpu->Backend = render::RenderBackend::Vulkan;
        else if (argument == "--d3d12")
            descriptor.Gpu->Backend = render::RenderBackend::D3D12;
        else if (argument == "--multithread")
            descriptor.Gpu->Multithreaded = true;
        else if (argument == "--valid-layer") {
            descriptor.Gpu->EnableValidation = true;
            descriptor.Gpu->EnableSynchronizationValidation = true;
        } else {
            const auto equal = argument.find('=');
            if (equal == std::string_view::npos) return 2;
            uint32_t value = 0;
            const auto number = argument.substr(equal + 1);
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()) return 2;
            const auto name = argument.substr(0, equal);
            if (name == "--flights" && value >= 1 && value <= 3)
                descriptor.FlightDataCount = value;
            else if (name == "--views" && (value == 1 || value == 3))
                app.ViewCount = value;
            else if (name == "--instances" && value > 0)
                app.InstanceCount = value;
            else if (name == "--frames" && value > 0)
                app.FrameLimit = value;
            else
                return 2;
        }
    }
    const int result = app.Run(descriptor);
    return result != 0 ? result : app.Failed ? 1
                                             : 0;
}
