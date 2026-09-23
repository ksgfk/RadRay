// Profiling workloads and interpretation: docs/guide/build-test.md
#include "example_framework_stress.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fmt/format.h>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/window_manager.h>
#include <radray/runtime/world_manager.h>

namespace radray::example {
namespace {

void PrintUsage() {
    fmt::print(
        "RadRay framework stress (Tracy)\n"
        "  --mode=world|sync|upload|draw  [upload]\n"
        "  --workload=idle|move|parent|tick|churn  [move]\n"
        "  --objects=N  [10000]  --changes=N  [100, clamped to objects]\n"
        "  --flights=1|2|3  [2]  --views=1|3  [1]\n"
        "  --warmup=N  [120 frames]  --seconds=N  [60; 0 = unlimited]\n"
        "  --frames=N  [0; nonzero overrides seconds, excludes warmup]\n"
        "  --d3d12 | --vulkan  [d3d12]\n"
        "  --multithread | --single-thread  [multithread]\n"
        "  --window  [otherwise offscreen, no swapchain or vsync]\n"
        "  --no-gpu-profiler  --validation  --help\n"
        "All modes use the same Application GPU runner and Ready cube asset.\n"
        "World leaves the World disconnected; sync adds CPU scene delivery;\n"
        "upload adds object buffers; draw adds the scene_sync unlit draw loop.\n"
        "Move changes evenly spread objects; parent moves one root of all objects;\n"
        "tick schedules empty Actor hooks; churn replaces changes objects/frame.\n");
}

bool ParseArguments(int argc, char** argv, FrameworkStressOptions& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--d3d12")
            options.Backend = render::RenderBackend::D3D12;
        else if (argument == "--vulkan")
            options.Backend = render::RenderBackend::Vulkan;
        else if (argument == "--multithread")
            options.Multithreaded = true;
        else if (argument == "--single-thread")
            options.Multithreaded = false;
        else if (argument == "--window")
            options.Window = true;
        else if (argument == "--validation")
            options.Validation = true;
        else if (argument == "--no-gpu-profiler")
            options.GpuProfiler = false;
        else {
            const auto equal = argument.find('=');
            if (equal == std::string_view::npos) return false;
            const auto name = argument.substr(0, equal);
            const auto value = argument.substr(equal + 1);
            if (name == "--mode") {
                if (value == "world")
                    options.Mode = StressMode::World;
                else if (value == "sync")
                    options.Mode = StressMode::Sync;
                else if (value == "upload")
                    options.Mode = StressMode::Upload;
                else if (value == "draw")
                    options.Mode = StressMode::Draw;
                else
                    return false;
            } else if (name == "--workload") {
                if (value == "idle")
                    options.Workload = StressWorkload::Idle;
                else if (value == "move")
                    options.Workload = StressWorkload::Move;
                else if (value == "parent")
                    options.Workload = StressWorkload::Parent;
                else if (value == "tick")
                    options.Workload = StressWorkload::Tick;
                else if (value == "churn")
                    options.Workload = StressWorkload::Churn;
                else
                    return false;
            } else {
                uint32_t number = 0;
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
                if (name == "--objects")
                    options.Objects = number;
                else if (name == "--changes")
                    options.Changes = number;
                else if (name == "--warmup")
                    options.Warmup = number;
                else if (name == "--frames")
                    options.Frames = number;
                else if (name == "--seconds")
                    options.Seconds = number;
                else if (name == "--flights" && number >= 1 && number <= 3)
                    options.Flights = number;
                else if (name == "--views" && (number == 1 || number == 3))
                    options.Views = number;
                else
                    return false;
            }
        }
    }
    options.Changes = std::min(options.Changes, options.Objects);
    return true;
}

std::string_view ModeName(StressMode mode) {
    switch (mode) {
        case StressMode::World: return "world";
        case StressMode::Sync: return "sync";
        case StressMode::Upload: return "upload";
        case StressMode::Draw: return "draw";
    }
    return "unknown";
}

std::string_view WorkloadName(StressWorkload workload) {
    switch (workload) {
        case StressWorkload::Idle: return "idle";
        case StressWorkload::Move: return "move";
        case StressWorkload::Parent: return "parent";
        case StressWorkload::Tick: return "tick";
        case StressWorkload::Churn: return "churn";
    }
    return "unknown";
}

}  // namespace

FrameworkStressApp::FrameworkStressApp(FrameworkStressOptions options) : _options(options) {}

void FrameworkStressApp::Fail(std::string_view reason) {
    RADRAY_ERR_LOG("Framework stress: {}", reason);
    _failed.store(true, std::memory_order_relaxed);
    RequestExit();
}

void FrameworkStressApp::OnInit() {
    RADRAY_PROFILE_THREAD("RadRay Game");
    RADRAY_PROFILE_SCOPE_N("Stress::Initialize");
    const auto settings = fmt::format("backend={} mode={} workload={} objects={} changes={} F={} views={} threaded={} window={} warmup={} frames={} seconds={} gpu_profiler={} validation={}",
                                      _options.Backend, ModeName(_options.Mode), WorkloadName(_options.Workload), _options.Objects, _options.Changes,
                                      _options.Flights, _options.Views, _options.Multithreaded, _options.Window, _options.Warmup,
                                      _options.Frames, _options.Seconds, _options.GpuProfiler, _options.Validation);
    RADRAY_INFO_LOG("Framework stress: {}", settings);
    RADRAY_PROFILE_MESSAGE(settings);
#if !defined(RADRAY_ENABLE_PROFILER)
    RADRAY_WARN_LOG("Tracy instrumentation is disabled; rebuild with RADRAY_ENABLE_PROFILER=ON");
#endif
    auto cube = CreateCube(*GetGpuSystem().Get());
    if (!cube) return Fail("cube upload failed");
    _mesh = GetAssetManager()->AddReady<StaticMesh>(AssetId{20, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}, std::move(cube));
    const auto worldId = GetWorldManager()->CreateWorld();
    _world = GetWorldManager()->GetWorld(worldId);
    if (_options.Mode != StressMode::World) GetWorldManager()->RequestRenderConnection(worldId, true);
    if (_options.Workload == StressWorkload::Parent) _parent = _world->SpawnActor()->AddComponent<SceneComponent>();
    _side = std::max(1u, static_cast<uint32_t>(std::ceil(std::sqrt(double(_options.Objects)))));
    _camera = _world->SpawnActor()->AddComponent<CameraComponent>();
    const float distance = std::max(8.0f, float(_side) * 3.2f);
    _camera->SetPerspective(Radian(60.0f), 0.1f, distance * 4);
    _camera->SetRelativeLocation({0, 0, -distance});
    _objects.reserve(_options.Objects);
    for (uint32_t i = 0; i < _options.Objects; ++i) _objects.push_back(SpawnObject(i));
    _frames.resize(_options.Flights);
    if ((_options.Mode == StressMode::Draw || _options.Window) && !InitializeDrawing()) Fail("drawing initialization failed");
}

StaticMeshComponent* FrameworkStressApp::SpawnObject(uint32_t index) {
    auto* actor = _world->SpawnActor();
    auto* object = actor->AddSceneComponent<StaticMeshComponent>(_parent, AttachmentRule::KeepLocal);
    object->SetStaticMesh(_mesh);
    object->SetRelativeLocation({(float(index % _side) - float(_side - 1) * 0.5f) * 1.6f,
                                 (float(index / _side) - float(_side - 1) * 0.5f) * 1.6f, 0});
    if (index % 7 == 0) object->SetRelativeScale({-1, 1, 1});
    if (_options.Workload == StressWorkload::Tick && index < _options.Changes) actor->SetTickEnabled(true);
    return object;
}

bool FrameworkStressApp::InitializeDrawing() {
    auto* device = GetGpuSystem()->GetDevice();
    const render::RenderPassColorAttachmentDescriptor attachment{render::TextureFormat::BGRA8_UNORM, 1, render::LoadAction::Clear, render::StoreAction::Store};
    auto pass = GetRenderSystem()->GetRenderPassRegistry()->GetOrCreateRenderPass({std::span{&attachment, 1}, {}});
    if (!pass) return false;
    _pass = pass;
    if (_options.Mode == StressMode::Draw) {
        _draw = make_unique<SceneDraw>();
        if (!_draw->Initialize(*GetRenderSystem().Get(), device, _pass.Get(), attachment.Format, _options.Flights)) return false;
    }
    if (_options.Window) return true;
    for (auto& frame : _frames) {
        auto image = device->CreateTexture({.Dim = render::TextureDimension::Dim2D, .Width = 1280, .Height = 720, .DepthOrArraySize = 1, .MipLevels = 1, .SampleCount = 1, .Format = attachment.Format, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::RenderTarget});
        if (!image) return false;
        frame.Image = image.Release();
        auto view = device->CreateTextureView({frame.Image.get(), render::TextureDimension::Dim2D, attachment.Format, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
        if (!view) return false;
        frame.View = view.Release();
        auto* viewPtr = frame.View.get();
        auto framebuffer = device->CreateFramebuffer({_pass.Get(), std::span{&viewPtr, 1}, nullptr, 1280, 720, 1});
        if (!framebuffer) return false;
        frame.Framebuffer = framebuffer.Release();
    }
    return true;
}

void FrameworkStressApp::MoveObjects() {
    RADRAY_PROFILE_SCOPE_N("Stress::MoveObjects");
    for (uint32_t i = 0; i < _options.Changes; ++i) {
        const auto index = static_cast<uint32_t>((_cursor + uint64_t{i} * _options.Objects / _options.Changes) % _options.Objects);
        auto* object = _objects[index];
        Eigen::Vector3f location = object->GetRelativeLocation();
        location.z() = location.z() > 0 ? -0.25f : 0.25f;
        object->SetRelativeLocation(location);
    }
}

void FrameworkStressApp::ReplaceObjects() {
    RADRAY_PROFILE_SCOPE_N("Stress::ReplaceObjects");
    for (uint32_t i = 0; i < _options.Changes; ++i) {
        const auto index = static_cast<uint32_t>((uint64_t{_cursor} + i) % _options.Objects);
        _world->DestroyActor(_objects[index]->GetOwner().Get());
        _objects[index] = SpawnObject(index);
    }
}

void FrameworkStressApp::OnUpdate(const AppUpdateContext& context) {
    RADRAY_PROFILE_SCOPE_N("Stress::Update");
    RADRAY_PROFILE_PLOT("Stress/LastCompletedFrameLatencyMs", double(context.LastFrameLatency.count()) * 1000.0);
    if (_options.GpuProfiler) RADRAY_PROFILE_PLOT("Stress/LastResolvedGpuMs", double(GetGpuSystem()->GetLastGpuTimeMs()));
    if (_updates == _options.Warmup) {
        _captureStart = std::chrono::steady_clock::now();
        RADRAY_INFO_LOG("Framework stress warmup complete; recording workload for {}", _options.Frames ? fmt::format("{} frames", _options.Frames) : _options.Seconds ? fmt::format("{} seconds", _options.Seconds)
                                                                                                                                                                      : "unlimited time");
        RADRAY_PROFILE_MESSAGE("Framework stress: GT warmup complete");
    }
    if (_updates >= _options.Warmup &&
        (_options.Frames ? _updates - _options.Warmup >= _options.Frames : _options.Seconds && std::chrono::steady_clock::now() - _captureStart >= std::chrono::seconds{_options.Seconds})) {
        RequestExit();
        return;
    }
    _frames[context.FlightIndex].UpdateIndex = _updates;
    switch (_options.Workload) {
        case StressWorkload::Move: MoveObjects(); break;
        case StressWorkload::Churn: ReplaceObjects(); break;
        case StressWorkload::Parent: {
            RADRAY_PROFILE_SCOPE_N("Stress::MoveParent");
            _parent->SetRelativeLocation({_updates % 2 ? -0.25f : 0.25f, 0, 0});
            break;
        }
        case StressWorkload::Idle:
        case StressWorkload::Tick: break;
    }
    if (_options.Objects && (_options.Workload == StressWorkload::Move || _options.Workload == StressWorkload::Churn)) {
        const uint32_t step = _options.Workload == StressWorkload::Churn ? _options.Changes : 1;
        _cursor = static_cast<uint32_t>((uint64_t{_cursor} + step) % _options.Objects);
    }
    ++_updates;
}

void FrameworkStressApp::OnCollectRenderViews(SceneViewCollector& collector) {
    RADRAY_PROFILE_SCOPE_N("Stress::CollectViews");
    if (_options.Mode == StressMode::World || !_camera) return;
    for (uint32_t i = 0; i < _options.Views; ++i) {
        if (!collector.Add(*_camera.Get(), {float(i) / _options.Views, 0, 1.0f / _options.Views, 1})) Fail("invalid scene view");
    }
}

void FrameworkStressApp::OnRender(AppFrameContext& frame) {
    RADRAY_PROFILE_SCOPE_N("Stress::Render");
    ++_renderCallbacks;
    auto& resources = _frames[frame.FlightIndex()];
    RADRAY_PROFILE_PLOT("Stress/Warmup", int64_t{resources.UpdateIndex < _options.Warmup});
    RADRAY_PROFILE_PLOT("Stress/Objects", int64_t{_options.Objects});
    if (resources.UpdateIndex == _options.Warmup) RADRAY_PROFILE_MESSAGE("Framework stress: RT warmup complete");
    std::optional<AppFrameTarget> target;
    if (_options.Window) {
        target = frame.AcquireWindow(GetWindowManager()->GetMainWindow());
        if (!target) return;
    }
    auto* renderer = GetRenderSystem().Get();
    const auto views = renderer->GetFrameViewsRT(frame.FlightIndex());
    array<std::optional<SceneGpuView>, 3> objects;
    const auto bytesBefore = frame.GetHostWrites().GetStats().CommittedBytes;
    if (_options.Mode == StressMode::Upload || _options.Mode == StressMode::Draw) {
        RADRAY_PROFILE_SCOPE_N("Stress::PrepareObjects");
        for (size_t i = 0; i < views.size(); ++i) {
            objects[i] = renderer->PrepareSceneGpuRT(views[i].Scene, frame);
            if (!objects[i] && _options.Objects) {
                const auto scene = renderer->GetSceneRT(views[i].Scene);
                Fail(fmt::format("object buffer preparation failed: serial={} view={}/{} scene={}:{} meshes={}", frame.FrameSerial(), i, views.size(), views[i].Scene.Index, views[i].Scene.Generation, scene ? scene->GetStaticMeshes().size() : 0));
            }
        }
    }
    RADRAY_PROFILE_PLOT("Stress/ObjectUploadBytes", static_cast<int64_t>(frame.GetHostWrites().GetStats().CommittedBytes - bytesBefore));
    RADRAY_PROFILE_PLOT("Stress/DrawCalls", _options.Mode == StressMode::Draw ? int64_t{_options.Objects} * _options.Views : int64_t{0});
    if (!_options.Window && _options.Mode != StressMode::Draw) return;

    auto* texture = target ? target->BackBuffer : resources.Image.get();
    const auto desc = texture->GetDesc();
    Nullable<render::Framebuffer*> framebuffer = resources.Framebuffer.get();
    if (target) {
        auto* view = target->BackBufferView;
        framebuffer = renderer->GetRenderPassRegistry()->GetOrCreateFramebuffer({_pass.Get(), std::span{&view, 1}, nullptr, desc.Width, desc.Height, 1});
    }
    auto* commands = frame.AllocateCommandBuffer();
    const render::ResourceBarrierDescriptor before = render::BarrierTextureDescriptor{
        .Target = texture,
        .Before = target ? target->Window->GetBackBufferState(target->BackBufferIndex) : resources.State,
        .After = render::TextureState::RenderTarget};
    commands->ResourceBarrier(std::span{&before, 1});
    if (framebuffer) {
        RADRAY_PROFILE_SCOPE_N("Stress::RecordPass");
        const render::ColorClearValue clear{{0.025f, 0.035f, 0.055f, 1}};
        auto encoder = commands->BeginRenderPass({_pass.Get(), framebuffer.Get(), std::span{&clear, 1}});
        if (encoder) {
            if (_draw) {
                RADRAY_PROFILE_SCOPE_N("Stress::DrawObjects");
                for (uint32_t i = 0; i < views.size(); ++i) {
                    if (objects[i] && !_draw->Draw(*renderer, frame, encoder.Get(), views[i], *objects[i], i, desc.Width, desc.Height)) Fail("draw recording failed");
                }
            }
            commands->EndRenderPass(encoder.Release());
        } else
            Fail("render pass recording failed");
    } else
        Fail("framebuffer creation failed");
    if (target) {
        const render::ResourceBarrierDescriptor after = render::BarrierTextureDescriptor{.Target = texture, .Before = render::TextureState::RenderTarget, .After = render::TextureState::Present};
        commands->ResourceBarrier(std::span{&after, 1});
    } else
        resources.State = render::TextureState::RenderTarget;
    frame.ReturnCommandBuffers({.CmdBuffers = std::span{&commands, 1}}, std::move(target));
}

void FrameworkStressApp::OnShutdown() {
    RADRAY_PROFILE_SCOPE_N("Stress::Shutdown");
    RADRAY_INFO_LOG("Framework stress finished: {} workload updates, {} render callbacks, failed={}", _updates, _renderCallbacks, Failed());
    _draw.reset();
    _frames.clear();
    _pass = nullptr;
    _objects.clear();
    _mesh.Reset();
    _parent = nullptr;
    _camera = nullptr;
    _world = nullptr;
}

}  // namespace radray::example

int main(int argc, char** argv) {
    using namespace radray;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view{argv[i]} == "--help") {
            example::PrintUsage();
            return 0;
        }
    }
    example::FrameworkStressOptions options;
    if (!example::ParseArguments(argc, argv, options)) {
        fmt::print(stderr, "Invalid framework stress arguments.\n");
        example::PrintUsage();
        return 2;
    }
    example::FrameworkStressApp app{options};
    const ApplicationRuntimeDescriptor descriptor{
        .Backend = options.Backend,
        .EnableValidation = options.Validation,
        .Multithreaded = options.Multithreaded,
        .EnableSynchronizationValidation = options.Validation,
        .AppName = "RadRay Framework Stress",
        .ShaderSourceRoot = RADRAY_SCENE_EXAMPLE_DIR,
        .ShaderIncludePaths = {RADRAY_SHADERLIB_DIR},
        .WindowTitle = "RadRay Framework Stress",
        .FlightDataCount = options.Flights,
        .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
        .PresentMode = render::PresentMode::Immediate,
        .Systems = (options.Window ? ApplicationSystems{ApplicationSystem::Window} : ApplicationSystems{}) |
                   ApplicationSystem::Gpu | ApplicationSystem::Render | ApplicationSystem::World | ApplicationSystem::Asset,
        .EnableGpuFrameProfiler = options.GpuProfiler};
    const int result = app.Run(descriptor);
    return result != 0 ? result : app.Failed() ? 1
                                               : 0;
}
