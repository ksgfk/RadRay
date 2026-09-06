#include <atomic>
#include <charconv>
#include <cmath>
#include <radray/json.h>
#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/spot_light_component.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_manager.h>
#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::probe {
namespace {
struct Options {
#ifdef RADRAY_ENABLE_IMGUI
    bool ImGui{true};
#endif
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    ForwardPipelineSettings Settings{ForwardPipelineSettings::Temporal()};
    uint32_t Frames{120}, Width{1280}, Height{800};
    bool Validation{true}, Multithread{false}, Split{false}, Observer{false}, Tour{false}, DumpGraph{false};
    std::filesystem::path Captures;
};
[[maybe_unused]] bool Parse(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
#ifdef RADRAY_ENABLE_IMGUI
        if (arg == "--imgui" || arg == "--no-imgui") {
            options.ImGui = arg == "--imgui";
            continue;
        }
#endif
        if (arg == "--vulkan")
            options.Backend = render::RenderBackend::Vulkan;
        else if (arg == "--d3d12")
            options.Backend = render::RenderBackend::D3D12;
        else if (arg == "--backend" && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            if (value == "d3d12")
                options.Backend = render::RenderBackend::D3D12;
            else if (value == "vulkan")
                options.Backend = render::RenderBackend::Vulkan;
            else
                return false;
        } else if (arg == "--msaa" || arg == "--temporal" || arg == "--profile") {
            const auto profile = arg == "--profile" && i + 1 < argc ? std::string_view{argv[++i]} : arg;
            const bool msaa = profile == "msaa" || profile == "--msaa";
            if (!msaa && profile != "temporal" && profile != "--temporal") return false;
            options.Settings.Antialiasing = msaa ? ForwardAntialiasing::Msaa4 : ForwardAntialiasing::Temporal;
            options.Settings.AmbientOcclusion = !msaa;
        } else if (arg == "--split")
            options.Split = true;
        else if (arg == "--observer")
            options.Observer = true;
        else if (arg == "--tour")
            options.Tour = true;
        else if (arg == "--dump-graph")
            options.DumpGraph = true;
        else if (arg == "--fireflies")
            options.Settings.Fireflies = true;
        else if (arg == "--multithread")
            options.Multithread = true;
        else if (arg == "--valid-layer")
            options.Validation = true;
        else if (arg == "--no-validation")
            options.Validation = false;
        else if (arg == "--capture-dir" && i + 1 < argc)
            options.Captures = argv[++i];
        else if ((arg == "--frames" || arg == "--width" || arg == "--height") && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            uint32_t number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || number == 0) return false;
            if (arg == "--frames")
                options.Frames = number;
            else if (arg == "--width")
                options.Width = number;
            else
                options.Height = number;
        } else if (arg == "--scale" && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            if (value == "0.5")
                options.Settings.RenderScale = .5f;
            else if (value == "0.75")
                options.Settings.RenderScale = .75f;
            else if (value == "1")
                options.Settings.RenderScale = 1;
            else
                return false;
        } else
            return false;
    }
    if (options.DumpGraph && options.Captures.empty()) options.Captures = "captures";
    return options.Settings.IsValid() && options.Width >= 320 && options.Height >= 240 && options.Width <= 7680 && options.Height <= 4320;
}
Eigen::Quaternionf Rotation(const Eigen::Vector3f& degrees) {
    return Eigen::Quaternionf{Eigen::AngleAxisf{Radian(degrees.y()), Eigen::Vector3f::UnitY()} *
                              Eigen::AngleAxisf{Radian(degrees.x()), Eigen::Vector3f::UnitX()} * Eigen::AngleAxisf{Radian(degrees.z()), Eigen::Vector3f::UnitZ()}};
}

// Continuous game-thread motion updates a live proxy; snapshots copy its transform.
class MovingProxy final : public PrimitiveSceneProxy {
public:
    MovingProxy(StreamingAssetRef<StaticMesh> mesh, Material* material, const Eigen::Matrix4f& transform)
        : Geometry(mesh, vector<Nullable<Material*>>(mesh.Get()->GetSections().size(), material), transform), Transform(transform) {}
    void CollectAssetReferences(vector<StreamingAssetRefAny>& out) const override { Geometry.CollectAssetReferences(out); }
    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return Transform; }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return Geometry.GetLocalBounds(); }
    MeshDrawArgs GetDrawArgs(uint32_t section) const noexcept override { return Geometry.GetDrawArgs(section); }
    uint32_t GetSectionCount() const noexcept override { return Geometry.GetSectionCount(); }
    Nullable<Material*> GetMaterial(uint32_t section) const noexcept override { return Geometry.GetMaterial(section); }
    StaticMeshSceneProxy Geometry;
    Eigen::Matrix4f Transform;
};
class MovingComponent final : public PrimitiveComponent {
public:
    MovingComponent(StreamingAssetRef<StaticMesh> mesh, Material* material) : Mesh(std::move(mesh)), Surface(material) {}
    bool ShouldCreateRenderState() const override { return Mesh.IsReady(); }
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override { return make_unique<MovingProxy>(Mesh, Surface, GetWorldMatrix()); }

protected:
    void OnTransformChanged() override {
        if (auto* proxy = dynamic_cast<MovingProxy*>(GetSceneProxy())) proxy->Transform = GetWorldMatrix();
    }

private:
    StreamingAssetRef<StaticMesh> Mesh;
    Material* Surface;
};

class ProbeApplication final : public Application {
public:
    explicit ProbeApplication(Options options) : _options(std::move(options)) {}
    bool Failed() const noexcept { return _failed; }

protected:
    template <class T, class... Args>
    T* Spawn(Args&&... args) {
        auto* actor = GetWorld()->SpawnActor<Actor>();
        _actors.push_back(actor);
        auto* component = actor->AddComponent<T>(std::forward<Args>(args)...);
        actor->SetRootComponent(component);
        return component;
    }
    void Close() {
        auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
#if defined(RADRAY_PLATFORM_WINDOWS)
        PostMessageW(static_cast<HWND>(window->GetNativeHandler()), WM_CLOSE, 0, 0);
#else
        window->Destroy();
#endif
    }
    void SwitchProfile() {
        auto settings = _pipeline->GetSettings();
        const bool temporal = settings.Antialiasing != ForwardAntialiasing::Temporal;
        settings.Antialiasing = temporal ? ForwardAntialiasing::Temporal : ForwardAntialiasing::Msaa4;
        settings.AmbientOcclusion = temporal;
        settings.DebugView = ForwardDebugView::Final;
        _pipeline->SetSettings(settings);
    }
#ifdef RADRAY_ENABLE_IMGUI
    void ConfigureImGui(ImGuiSystemDescriptor& descriptor) override { descriptor.Enabled = _options.ImGui; }
    void OnImGui() override {
        ImGui::SetNextWindowSize({320, 180}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Forward rendering")) {
            ImGui::TextUnformatted("UI is composed after scene tone mapping.");
            ImGui::Text("Frame %u", _frame);
            if (_pipeline) {
                auto settings = _pipeline->GetSettings();
                if (ImGui::SliderFloat("Exposure", &settings.Exposure, .1f, 5.0f)) _pipeline->SetSettings(settings);
                ImGui::Text("Scene scale %.2f", settings.RenderScale);
            }
        }
        ImGui::End();
    }
#endif
    void OnInit() override {
        for (const auto name : {"block", "panel", "sphere", "ring", "spire", "vase"}) _meshes.emplace(name, GetAssetManager()->Load<StaticMesh>(fmt::format("tidal_atrium/{}.obj", name)));
        _white = GetAssetManager()->Load<TextureAsset>("tidal_atrium/white.png");
        _contact = GetAssetManager()->Load<TextureAsset>("tidal_atrium/contact.png");
        _camera = Spawn<CameraComponent>();
        _camera->SetWorldLocation({0, 8, -22});
        _camera->SetWorldRotation(Rotation({13, 0, 0}));
        _camera->SetPerspective(Radian(55.f), .1f, 250.f);
        _keyboard = GetWindowManager()->GetMainWindow()->GetInput().EventInput().connect([this](const WindowInputEvent& event) {
            if (event.Type != WindowInputType::Key) return;
            const auto key = event.Key;
            const auto action = event.State;
            if (action == Action::PRESSED)
                _keys.insert(key);
            else if (action == Action::RELEASED)
                _keys.erase(key);
            if (action != Action::PRESSED || !_pipeline) return;
            if (key == KeyCode::ESCAPE) Close();
            if (key == KeyCode::F2) {
                auto s = _pipeline->GetSettings();
                s.Shadows = !s.Shadows;
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F3) {
                auto s = _pipeline->GetSettings();
                if (s.Antialiasing != ForwardAntialiasing::Msaa4) s.AmbientOcclusion = !s.AmbientOcclusion;
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F4) _options.Split = !_options.Split;
            if (key == KeyCode::F5) {
                auto s = _pipeline->GetSettings();
                do {
                    s.DebugView = static_cast<ForwardDebugView>((uint32_t(s.DebugView) + 1) % 11);
                } while (!s.IsValid());
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F6) {
                auto s = _pipeline->GetSettings();
                if (s.Antialiasing != ForwardAntialiasing::Msaa4) {
                    s.DebugView = ForwardDebugView::Final;
                    s.Antialiasing = s.Antialiasing == ForwardAntialiasing::Temporal ? ForwardAntialiasing::None : ForwardAntialiasing::Temporal;
                }
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F7) {
                auto s = _pipeline->GetSettings();
                s.RenderScale = s.RenderScale == 1 ? .75f : s.RenderScale == .75f ? .5f
                                                                                  : 1;
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F8) SwitchProfile();
            if (key == KeyCode::F9) {
                auto s = _pipeline->GetSettings();
                s.Bloom = !s.Bloom;
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F10) {
                auto s = _pipeline->GetSettings();
                s.ForwardPlus = !s.ForwardPlus;
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::F11) {
                auto s = _pipeline->GetSettings();
                s.Fireflies = !s.Fireflies;
                _pipeline->SetSettings(s);
            }
            if (key == KeyCode::P) _pauseSecond = !_pauseSecond;
            if (key == KeyCode::SPACE) _pauseMotion = !_pauseMotion;
        });
    }
    bool CreateMaterials() {
        auto* system = GetRenderSystem();
        const auto lit = system->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/pbr.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
        const auto depth = system->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/depth_normals_motion.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
        const auto shadow = system->GetOrCreateShaderProgram({.SourceName = "shaderlib/pipelines/forward/shadow_caster.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
        if (!lit || !depth || !shadow) return false;
        MaterialPipelineState state;
        state.Primitive.Cull = render::CullMode::None;
        auto technique = MaterialTechnique::Create({{"ForwardLit", lit.Get(), "ForwardMaterial", state}, {"DepthNormalsMotion", depth.Get(), "ForwardMaterial", state}, {"ShadowCaster", shadow.Get(), "ForwardMaterial", state}}, "ForwardLit");
        if (!technique) return false;
        _technique = technique.Release();
        return true;
    }
    Material* MaterialFor(std::string_view name, Eigen::Vector4f color = {.45f, .55f, .6f, 1}, float metal = 0, float roughness = .45f) {
        const auto existing = _materials.find(name);
        if (existing != _materials.end()) return existing->second.get();
        auto material = Material::Create(_technique.get());
        if (!material) return nullptr;
        float emission = 0, cutoff = 0;
        if (name == "copper" || name == "gold") {
            color = {.75f, .36f, .12f, 1};
            metal = 1;
            roughness = .2f;
        }
        if (name == "dark" || name == "navy") color = {.04f, .07f, .09f, 1};
        if (name == "limestone" || name == "pearl") color = {.8f, .78f, .68f, 1};
        if (name == "warm") {
            color = {1, .22f, .04f, 1};
            emission = 3;
        }
        if (name == "cyan" || name == "core") {
            color = {.04f, .8f, .65f, 1};
            emission = 3;
        }
        if (name.starts_with("glass")) {
            color = {.08f, .5f, .6f, .28f};
            metal = .2f;
            roughness = .1f;
        }
        if (name == "cutout") cutoff = .5f;
        render::SamplerDescriptor sampler;
        sampler.MinFilter = sampler.MagFilter = render::FilterMode::Linear;
        if (!material->SetFloat4("BaseColor", color) || !material->SetFloat4("Surface", {metal, roughness, cutoff, emission}) ||
            !material->SetFloat4("Transmission", {color.w() < 1 ? 4.f : 0.f, 0, 0, 0}) ||
            !material->SetTexture("AlbedoTexture", cutoff > 0 ? _contact : _white) || !material->SetSampler("LinearSampler", sampler)) return nullptr;
        if (color.w() < 1) {
            material->SetRenderQueue(RenderQueue::Transparent);
            auto& state = material->GetPipelineState();
            state.Blend = render::BlendState::Default();
            state.Blend->Color = {render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add};
            state.DepthStencil.DepthWriteEnable = false;
        }
        auto* result = material.Get();
        _materials.emplace(string{name}, material.Release());
        return result;
    }
    bool CreateScene() {
        if (!CreateMaterials()) return false;
        // Reuse Tidal Atrium's procedural scene description and mesh assets.
        auto document = JsonDocument::ParseFile(std::filesystem::path{RADRAY_PROJECT_DIR_DEFAULT} / "assets/tidal_atrium/scene.json");
        if (!document || !document->Root()["objects"].IsArray()) return false;
        const auto objects = document->Root()["objects"];
        for (size_t i = 0; i < objects.Size(); ++i) {
            const auto item = objects.At(i);
            auto* material = MaterialFor(item["material"].AsString());
            if (!material) return false;
            const string mesh{item["mesh"].AsString()};
            if (!_meshes.contains(mesh)) return false;
            const auto vector3 = [&](std::string_view key) { const auto a = item[key]; return Eigen::Vector3f{float(a.At(0).AsDouble()), float(a.At(1).AsDouble()), float(a.At(2).AsDouble())}; };
            auto* component = Spawn<MovingComponent>(_meshes.at(mesh), material);
            component->SetWorldLocation(vector3("p"));
            component->SetRelativeScale(vector3("s"));
            component->SetWorldRotation(Rotation(vector3("r")));
        }
        for (uint32_t y = 0; y < 3; ++y)
            for (uint32_t x = 0; x < 5; ++x) {
                auto* material = MaterialFor(fmt::format("pbr {} {}", x, y), {.65f, .25f, .08f, 1}, float(y) / 2, .06f + x * .22f);
                if (!material) return false;
                auto* sphere = Spawn<MovingComponent>(_meshes.at("sphere"), material);
                sphere->SetWorldLocation({-6 + x * 3.f, 1 + y * 2.1f, -4});
                sphere->SetRelativeScale({.8f, .8f, .8f});
                if (x == 2 && y == 2) _moving = sphere;
            }
        auto* cutout = MaterialFor("cutout");
        if (!cutout) return false;
        auto* panel = Spawn<MovingComponent>(_meshes.at("panel"), cutout);
        panel->SetWorldLocation({-7, 3, -2});
        panel->SetRelativeScale({4, 4, 1});
        auto* sun = Spawn<DirectionalLightComponent>();
        sun->SetWorldRotation(Rotation({42, -32, 0}));
        sun->SetIntensity(3.5f);
        sun->SetCastShadow(true);
        for (uint32_t i = 0; i < 96; ++i) {
            PointLightComponent* light = i % 3 == 0 ? static_cast<PointLightComponent*>(Spawn<SpotLightComponent>()) : Spawn<PointLightComponent>();
            light->SetWorldLocation({float(i % 12) * 2 - 11, 3.5f + float(i / 12) * .2f, float(i / 12) * 3 - 4});
            light->SetAttenuationRadius(8);
            light->SetIntensity(10);
            light->SetLightColor({.2f + (i % 3) * .35f, .3f + (i % 5) * .12f, .7f});
            if (i % 3 == 0) {
                auto* spot = static_cast<SpotLightComponent*>(light);
                spot->SetConeAngles(Radian(15.f), Radian(35.f));
                spot->SetWorldRotation(Rotation({65, float(i * 17), 0}));
            }
        }
        auto pipeline = make_unique<ForwardPipeline>(this, GetWorld()->GetScene(), _camera.Get());
        if (!pipeline->SetSettings(_options.Settings)) return false;
        _pipeline = pipeline.get();
        GetRenderSystem()->SetPipeline(std::move(pipeline));
        if (_options.Observer) {
            auto texture = GetDevice()->CreateTexture({render::TextureDimension::Dim2D, 512, 384, 1, 1, 1, render::TextureFormat::RGBA8_UNORM, render::MemoryType::Device, render::TextureUse::RenderTarget | render::TextureUse::Resource | render::TextureUse::CopySource, {}});
            if (!texture) return false;
            _observer = texture.Release();
            auto view = GetDevice()->CreateTextureView({_observer.get(), render::TextureDimension::Dim2D, render::TextureFormat::RGBA8_UNORM, {0, 1, 0, 1}, render::TextureViewUsage::RenderTarget});
            if (!view) return false;
            _observerRtv = view.Release();
            _observerId = GetRenderSystem()->GetOutputs().RegisterExternal({"Forward observer", _observer.get(), _observerRtv.get()});
            if (!_observerId.IsValid()) return false;
        }
        return true;
    }
    void OnUpdate(const AppUpdateContext& context) override {
        if (!_pipeline) {
            _loading += context.DeltaTime.count();
            bool ready = _white.IsReady() && _contact.IsReady();
            for (const auto& [name, asset] : _meshes) {
                ready &= asset.IsReady();
                if (asset.IsFaulted()) _failed = true;
            }
            if (_failed || _loading > 90) {
                _failed = true;
                Close();
                return;
            }
            if (!ready) return;
            if (!CreateScene()) {
                _failed = true;
                Close();
                return;
            }
        }
        if (_pipeline->Failed()) {
            _failed = true;
            Close();
            return;
        }
        ++_frame;
        if (!_pauseMotion) _motionTime += context.DeltaTime.count();
        if (_moving) _moving->SetWorldLocation({std::sin(_motionTime) * 2, 5.2f, -4});
        Eigen::Vector3f move = Eigen::Vector3f::Zero();
        if (_keys.contains(KeyCode::W)) move.z() += 1;
        if (_keys.contains(KeyCode::S)) move.z() -= 1;
        if (_keys.contains(KeyCode::A)) move.x() -= 1;
        if (_keys.contains(KeyCode::D)) move.x() += 1;
        if (_keys.contains(KeyCode::Q)) move.y() -= 1;
        if (_keys.contains(KeyCode::E)) move.y() += 1;
        const auto held = [&](KeyCode key) { return _keys.contains(key) ? 1.f : 0.f; };
        _yaw += (held(KeyCode::RIGHT) - held(KeyCode::LEFT)) * context.DeltaTime.count() * 60;
        _pitch = std::clamp(_pitch + (held(KeyCode::DOWN) - held(KeyCode::UP)) * context.DeltaTime.count() * 60, -85.f, 85.f);
        const auto rotation = Rotation({_pitch, _yaw, 0});
        _camera->SetWorldRotation(rotation);
        _camera->SetWorldLocation(_camera->GetEyePosition() + rotation * move * (context.DeltaTime.count() * 10));
        if (held(KeyCode::R) || held(KeyCode::F)) {
            auto s = _pipeline->GetSettings();
            s.Exposure = std::clamp(s.Exposure * std::exp2((held(KeyCode::R) - held(KeyCode::F)) * context.DeltaTime.count()), .01f, 32.f);
            _pipeline->SetSettings(s);
        }
        if (_options.Tour && _frame % 24 == 0) {
            const auto phase = (_frame / 24) % 8;
            auto s = _pipeline->GetSettings();
            if (phase == 1) {
                s.RenderScale = .5f;
                _pipeline->SetSettings(s);
            }
            if (phase == 2 || phase == 7) GetWindowManager()->GetMainWindow()->GetNativeWindow()->SetSize(int(_options.Width) + (phase == 2 ? 96 : 0), int(_options.Height) + (phase == 2 ? 64 : 0));
            if (phase == 3 || phase == 5) SwitchProfile();
            if (phase == 4) _pauseSecond = true;
            if (phase == 5) _pauseSecond = false;
            if (phase == 6 && s.Antialiasing == ForwardAntialiasing::Temporal) {
                s.DebugView = ForwardDebugView::HistoryHdr;
                _pipeline->SetSettings(s);
            }
            if (phase == 0) {
                s.DebugView = ForwardDebugView::Final;
                s.RenderScale = 1;
                _pipeline->SetSettings(s);
            }
            RADRAY_INFO_LOG("Forward tour phase {}: profile={} scale={} second-paused={}", phase, uint32_t(_pipeline->GetSettings().Antialiasing), _pipeline->GetSettings().RenderScale, _pauseSecond);
        }
        RenderViewDesc view;
        view.Name = "Forward main";
        view.StateId = _ids[0];
        view.WorldPosition = _camera->GetEyePosition();
        view.WorldToView = _camera->ComputeViewMatrix();
        view.Projection = PerspectiveProjectionDesc{_camera->GetFovY(), .1f, 250};
        vector<ForwardViewSource> views{{GetWindowManager()->GetMainWindow()->GetRenderOutputId(), view}};
        if (_options.Split) {
            views[0].View.ViewRect = views[0].View.ScissorRect = {0, 0, .5f, 1};
            view.Name = "Forward second";
            view.StateId = _ids[1];
            view.WorldPosition = {16, 10, -10};
            view.WorldToView = LookAtLH(view.WorldPosition, Eigen::Vector3f{0, 3, 5}, Eigen::Vector3f::UnitY().eval());
            view.ViewRect = view.ScissorRect = {.5f, 0, .5f, 1};
            if (!_pauseSecond) views.push_back({views[0].Output, view});
        }
        if (_observerId.IsValid()) {
            view.Name = "Forward observer";
            view.StateId = _ids[2];
            view.WorldPosition = {0, 32, 5};
            view.WorldToView = LookAtLH(view.WorldPosition, Eigen::Vector3f{0, 0, 5}, Eigen::Vector3f::UnitZ().eval());
            view.Projection = OrthographicProjectionDesc{48, .1f, 100};
            view.ViewRect = view.ScissorRect = {};
            views.push_back({_observerId, view});
        }
        if (!_pipeline->SetViews(views)) {
            _failed = true;
            Close();
            return;
        }
        if (_observerId.IsValid()) {
            const ForwardOutputOverlay overlay{_observerId, views[0].Output};
            if (!_pipeline->SetOutputOverlays(std::span{&overlay, 1})) {
                _failed = true;
                Close();
                return;
            }
        }
        if (!_options.Captures.empty() && _frame == (_options.Frames > 4 ? _options.Frames - 4 : 1)) _pipeline->RequestCapture(_options.Captures, fmt::format("forward-{}-{}", _options.Backend, _pipeline->GetSettings().Antialiasing == ForwardAntialiasing::Msaa4 ? "msaa" : "temporal"));
        if (_frame % 30 == 0) {
            const auto& s = _pipeline->GetSettings();
            GetWindowManager()->GetMainWindow()->GetNativeWindow()->SetTitle(fmt::format("Forward | {} | frame {} | AA {} debug {} exposure {:.2f} scale {} | F2-11 effects / arrows look / RF exposure / P view pause", _options.Backend, _frame, uint32_t(s.Antialiasing), uint32_t(s.DebugView), s.Exposure, s.RenderScale));
        }
        if (_frame >= _options.Frames) Close();
    }
    void OnRenderFrameComplete(const AppRenderCompleteContext& context) override {
        if (_pipeline && context.GpuWorkCompleted && !_pipeline->CompleteCaptures(context.FlightIndex)) _failed = true;
        if (_pipeline && context.GpuWorkCompleted && ++_completed % 60 == 0) {
            const auto& stats = _pipeline->GetStageBStats(context.FlightIndex);
            RADRAY_INFO_LOG("Forward completed frame {}: culls={} depth={} opaque={} transparent={} draws={} failures={}", _completed, stats.CullCalls, stats.DepthCommands, stats.OpaqueCommands, stats.TransparentCommands, stats.Execution.Draws, stats.Execution.PsoFailure + stats.Execution.BindingFailure);
        }
    }
    void OnShutdown() override {
        _keyboard.disconnect();
        if (_pipeline && _pipeline->Failed()) _failed = true;
        for (auto* actor : _actors) GetWorld()->DestroyActor(actor);
        _actors.clear();
        GetRenderSystem()->SetPipeline(nullptr);
        _pipeline = nullptr;
        if (_observerId.IsValid()) GetRenderSystem()->GetOutputs().Unregister(_observerId);
        if (_observerRtv) GetRenderSystem()->GetRenderPassRegistry()->RemoveFramebuffersUsing(_observerRtv.get());
        _observerRtv.reset();
        _observer.reset();
        _materials.clear();
        _technique.reset();
        _meshes.clear();
        _white = {};
        _contact = {};
        RADRAY_INFO_LOG("Forward probe completed {} frames: {}", _frame, _failed ? "FAILED" : "clean");
    }

private:
    Options _options;
    std::atomic_bool _failed{false};
    uint32_t _frame{0};
    float _loading{0};
    uint32_t _completed{0};
    float _motionTime{0}, _yaw{0}, _pitch{13};
    bool _pauseSecond{false}, _pauseMotion{false};
    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<MovingComponent*> _moving{nullptr};
    Nullable<ForwardPipeline*> _pipeline{nullptr};
    array<ViewStateId, 3> _ids{AllocateViewStateId(), AllocateViewStateId(), AllocateViewStateId()};
    vector<Actor*> _actors;
    unordered_map<string, StreamingAssetRef<StaticMesh>> _meshes;
    StreamingAssetRef<TextureAsset> _white, _contact;
    unique_ptr<MaterialTechnique> _technique;
    unordered_map<string, unique_ptr<Material>, StringHash, StringEqual> _materials;
    unordered_set<KeyCode> _keys;
    sigslot::scoped_connection _keyboard;
    unique_ptr<render::Texture> _observer;
    unique_ptr<render::TextureView> _observerRtv;
    RenderOutputId _observerId;
};
}  // namespace
}  // namespace radray::probe

int main(int argc, char** argv) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    (void)argc;
    (void)argv;
    RADRAY_ERR_LOG("Forward probe requires RADRAY_ENABLE_SHADER_JIT");
    return 1;
#else
    using namespace radray;
    probe::Options options;
    if (!probe::Parse(argc, argv, options)) {
        RADRAY_ERR_LOG("Usage: example_pipeline_probe --backend d3d12|vulkan --profile temporal|msaa [--frames N] [--scale 0.5|0.75|1] [--split] [--observer] [--fireflies] [--tour] [--multithread] [--capture-dir PATH] [--dump-graph]");
        return 1;
    }
    const std::filesystem::path root{RADRAY_PROJECT_DIR_DEFAULT};
    const ApplicationRuntimeDescriptor descriptor{.Backend = options.Backend, .EnableValidation = options.Validation, .Multithreaded = options.Multithread, .AppName = "Forward pipeline probe", .AssetRoot = root / "assets", .ShaderSourceRoot = root, .ShaderIncludePaths = {root / "shaderlib"}, .WindowTitle = "RadRay Forward pipeline probe", .WindowWidth = int(options.Width), .WindowHeight = int(options.Height), .BackBufferCount = 3, .FlightDataCount = 3, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate, .EnableSynchronizationValidation = options.Validation};
    std::atomic_bool errors{false};
    SetLogCallback(+[](LogLevel level, std::string_view message, void* data) {
        if (level == LogLevel::Err || level == LogLevel::Critical || (level == LogLevel::Warn && message.find("Validation Layer") != std::string_view::npos)) static_cast<std::atomic_bool*>(data)->store(true); }, &errors);
    int result;
    {
        probe::ProbeApplication application{options};
        result = application.Run(descriptor);
        if (application.Failed()) result = 1;
    }
    ClearLogCallback();
    return errors ? 1 : result;
#endif
}
