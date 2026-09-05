#include "atrium_pipeline.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <radray/json.h>
#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/primitive_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/static_mesh_scene_proxy.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/window_manager.h>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#endif

namespace radray::atrium {
namespace {

struct Options {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    bool Multithread{false}, Validation{false}, Tour{false}, SkyTest{false};
    uint32_t Frames{0}, Width{1280}, Height{800};
    std::filesystem::path Captures;
};

bool ParseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--multithread")
            options.Multithread = true;
        else if (argument == "--valid-layer")
            options.Validation = true;
        else if (argument == "--tour")
            options.Tour = true;
        else if (argument == "--sky-test")
            options.SkyTest = true;
        else if (argument == "--d3d12")
            options.Backend = render::RenderBackend::D3D12;
        else if (argument == "--vulkan")
            options.Backend = render::RenderBackend::Vulkan;
        else if (argument == "--backend" && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            if (value == "d3d12")
                options.Backend = render::RenderBackend::D3D12;
            else if (value == "vulkan")
                options.Backend = render::RenderBackend::Vulkan;
            else
                return false;
        } else if (argument == "--capture-dir" && i + 1 < argc)
            options.Captures = argv[++i];
        else if ((argument == "--frames" || argument == "--width" || argument == "--height") && i + 1 < argc) {
            const std::string_view value{argv[++i]};
            uint32_t number = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
            if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || !number) return false;
            if (argument == "--frames")
                options.Frames = number;
            else if (argument == "--width")
                options.Width = number;
            else
                options.Height = number;
        } else
            return false;
    }
    if (options.Width < 640 || options.Height < 480 || options.Width > 7680 || options.Height > 4320) return false;
    if (options.Tour && options.SkyTest) return false;
    if (options.Tour && !options.Frames) options.Frames = 360;
    if (options.SkyTest && !options.Frames) options.Frames = 82;
    if (!options.Captures.empty()) {
        std::error_code error;
        std::filesystem::create_directories(options.Captures, error);
        if (error) return false;
    }
    return true;
}

std::filesystem::path AssetsRoot() {
    if (const char* env = std::getenv("RADRAY_ASSETS_DIR"); env && *env) return env;
    return RADRAY_ASSETS_DIR_DEFAULT;
}

Eigen::Quaternionf Rotation(const Eigen::Vector3f& degrees) {
    return Eigen::Quaternionf{Eigen::AngleAxisf{Radian(degrees.y()), Eigen::Vector3f::UnitY()} *
                              Eigen::AngleAxisf{Radian(degrees.x()), Eigen::Vector3f::UnitX()} * Eigen::AngleAxisf{Radian(degrees.z()), Eigen::Vector3f::UnitZ()}};
}

class DisplayProxy final : public PrimitiveSceneProxy {
public:
    DisplayProxy(StreamingAssetRef<StaticMesh> mesh, vector<Nullable<Material*>> materials, const Eigen::Matrix4f& transform, uint32_t layer)
        : _mesh(std::move(mesh), std::move(materials), transform), _layer(layer) {}
    void CollectAssetReferences(vector<StreamingAssetRefAny>& out) const override { _mesh.CollectAssetReferences(out); }
    Eigen::Matrix4f GetLocalToWorld() const noexcept override { return _mesh.GetLocalToWorld(); }
    AxisAlignedBounds GetLocalBounds() const noexcept override { return _mesh.GetLocalBounds(); }
    uint32_t GetLayerMask() const noexcept override { return _layer; }
    MeshDrawArgs GetDrawArgs(uint32_t section) const noexcept override { return _mesh.GetDrawArgs(section); }
    uint32_t GetSectionCount() const noexcept override { return _mesh.GetSectionCount(); }
    Nullable<Material*> GetMaterial(uint32_t section) const noexcept override { return _mesh.GetMaterial(section); }

private:
    StaticMeshSceneProxy _mesh;
    uint32_t _layer;
};

class DisplayComponent final : public PrimitiveComponent {
public:
    DisplayComponent(StreamingAssetRef<StaticMesh> mesh, vector<Nullable<Material*>> materials, uint32_t layer)
        : _mesh(std::move(mesh)), _materials(std::move(materials)), _layer(layer) {}
    bool ShouldCreateRenderState() const override { return _mesh.IsReady() && !_materials.empty(); }
    unique_ptr<PrimitiveSceneProxy> CreateSceneProxy() override { return make_unique<DisplayProxy>(_mesh, _materials, GetWorldMatrix(), _layer); }

private:
    StreamingAssetRef<StaticMesh> _mesh;
    vector<Nullable<Material*>> _materials;
    uint32_t _layer;
};

Nullable<unique_ptr<render::Buffer>> UploadBuffer(render::Device& device, std::span<const byte> bytes, render::BufferUses usage) {
    auto buffer = device.CreateBuffer({bytes.size(), render::MemoryType::Upload, usage | render::BufferUse::MapWrite, {}});
    if (!buffer) return nullptr;
    ScopedBufferMap map{buffer.Get(), {0, bytes.size()}};
    if (!map.Data()) return nullptr;
    std::memcpy(map.Data(), bytes.data(), bytes.size());
    return buffer;
}

StreamingAssetRef<StaticMesh> MakeSplitMesh(Application& app, const StaticMesh& source) {
    const auto& cpu = source.GetMeshResource();
    if (cpu.Primitives.size() != 1) return {};
    const auto& primitive = cpu.Primitives[0];
    vector<float> positions(primitive.VertexCount * 3), normalUv(primitive.VertexCount * 5);
    uint32_t found = 0;
    for (const auto& entry : primitive.VertexBuffers) {
        uint32_t count = 0, offset = 0, stride = 0;
        Nullable<float*> destination{nullptr};
        if (entry.Semantic == VertexSemantics::POSITION) {
            destination = positions.data();
            count = 3;
            stride = 3;
            found |= 1;
        }
        if (entry.Semantic == VertexSemantics::NORMAL) {
            destination = normalUv.data();
            count = 3;
            stride = 5;
            found |= 2;
        }
        if (entry.Semantic == VertexSemantics::TEXCOORD) {
            destination = normalUv.data();
            count = 2;
            offset = 3;
            stride = 5;
            found |= 4;
        }
        if (!destination) continue;
        if (entry.Type != VertexDataType::FLOAT || entry.ComponentCount != count || entry.BufferIndex >= cpu.Bins.size()) return {};
        const auto bytes = cpu.Bins[entry.BufferIndex].GetData();
        if (uint64_t{entry.Offset} + uint64_t{primitive.VertexCount - 1} * entry.Stride + count * 4 > bytes.size()) return {};
        for (uint32_t i = 0; i < primitive.VertexCount; ++i) std::memcpy(destination.Get() + i * stride + offset, bytes.data() + entry.Offset + i * entry.Stride, count * 4);
    }
    if (found != 7 || !primitive.IndexBuffer.IndexCount) return {};
    const auto& indices = primitive.IndexBuffer;
    const auto bytes = cpu.Bins[indices.BufferIndex].GetData().subspan(indices.Offset, uint64_t{indices.IndexCount} * indices.Stride);
    auto pos = UploadBuffer(*app.GetDevice(), std::as_bytes(std::span{positions}), render::BufferUse::Vertex);
    auto attributes = UploadBuffer(*app.GetDevice(), std::as_bytes(std::span{normalUv}), render::BufferUse::Vertex);
    auto index = UploadBuffer(*app.GetDevice(), bytes, render::BufferUse::Index);
    if (!pos || !attributes || !index) return {};
    GpuMesh gpu;
    GpuMesh::DrawData draw;
    draw.VertexBuffers = {{3, {pos.Get(), 0, positions.size() * 4}}, {7, {attributes.Get(), 0, normalUv.size() * 4}}};
    draw.Ibv = {index.Get(), 0, indices.Stride};
    draw.VertexLayout.Buffers = {{3, 12, render::VertexStepMode::Vertex}, {7, 20, render::VertexStepMode::Vertex}};
    draw.VertexLayout.Attributes = {{"POSITION", 0, 3, 0, render::VertexFormat::FLOAT32X3}, {"NORMAL", 0, 7, 0, render::VertexFormat::FLOAT32X3}, {"TEXCOORD", 0, 7, 12, render::VertexFormat::FLOAT32X2}};
    gpu.Draws.push_back(std::move(draw));
    gpu.Buffers.push_back(pos.Release());
    gpu.Buffers.push_back(attributes.Release());
    gpu.Buffers.push_back(index.Release());
    const uint32_t half = (indices.IndexCount / 6) * 3;
    vector<StaticMeshSection> sections{{0, 0, half, 0, primitive.VertexCount - 1}, {0, half, indices.IndexCount - half, 0, primitive.VertexCount - 1}};
    return app.GetAssetManager()->AddReady<StaticMesh>(AssetId{0x741da17, 0x9182, 0x4376, 0x83, 0x14, 0x91, 0x2, 0x22, 0x43, 0x18, 0x65},
                                                       make_unique<StaticMesh>(cpu, std::move(sections), source.GetBoundsMin(), source.GetBoundsMax(), std::move(gpu)));
}

class AtriumApplication final : public Application {
public:
    explicit AtriumApplication(Options options) : _options(std::move(options)) {}
    bool Failed() const noexcept { return _failed; }

protected:
    void OnInit() override {
        _window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
        _keyboard = _window->EventKeyboard().connect([this](KeyCode key, Action action) { Key(key, action); });
        _mouse = _window->EventTouch().connect([this](int, int, MouseButton button, Action action) {
            if (button == MouseButton::BUTTON_RIGHT && action != Action::REPEATED) CaptureMouse(action == Action::PRESSED);
        });
        _focus = _window->EventFocused().connect([this](bool focused) { if (!focused) { _keys.clear(); CaptureMouse(false); } });
        for (const auto name : {"block", "panel", "sphere", "ring", "spire", "vase"})
            _meshes.emplace(name, GetAssetManager()->Load<StaticMesh>(fmt::format("tidal_atrium/{}.obj", name)));
        for (const auto name : {"white", "basalt", "limestone", "sampler_grid", "font", "tidal_mural", "contact", "welcome", "light", "glass", "material", "signal", "observatory", "nearest", "linear", "streams"})
            _textures.emplace(name, GetAssetManager()->Load<TextureAsset>(fmt::format("tidal_atrium/{}.png", name)));
        _camera = Spawn<CameraComponent>();
        _camera->SetPerspective(Radian(58.f), .1f, 250.f);
        Station(0);
        _window->SetTitle("Tidal Atrium | Loading original gallery assets...");
        if (!CreateMaterials()) {
            _failed = true;
            Close();
            return;
        }
        auto pipeline = make_unique<AtriumPipeline>(this, GetWorld()->GetScene(), _camera.Get(), _textures.at("font"), _options.Captures);
        if (!pipeline->IsValid()) {
            _failed = true;
            Close();
            return;
        }
        _pipeline = pipeline.get();
        GetRenderSystem()->SetPipeline(std::move(pipeline));
    }

    void OnUpdate(const AppUpdateContext& ctx) override {
        const float dt = std::clamp(ctx.DeltaTime.count(), .0001f, .05f);
        if (!_ready) {
            bool loaded = true;
            const auto check = [&](const auto& collection) {
                for (const auto& [name, asset] : collection) {
                    if (!asset.IsValid() || asset.IsFaulted() || asset.IsCanceled()) {
                        RADRAY_ERR_LOG("Atrium asset failed: {}", name);
                        _failed = true;
                    }
                    loaded &= asset.IsReady();
                }
            };
            check(_meshes);
            check(_textures);
            _loadingTime += std::max(0.f, ctx.DeltaTime.count());
            if (_failed || _loadingTime > 90) {
                _failed = true;
                Close();
                return;
            }
            if (!loaded) return;
            if (!CreateScene()) {
                _failed = true;
                Close();
                return;
            }
            _ready = true;
            _pipeline->GameSettings.Ready = true;
            RADRAY_INFO_LOG("Tidal Atrium ready: {} scene actors, {} materials; WASD + right mouse, 1-5 viewpoints, H help", _actors.size(), _materials.size());
        }
        if (_pipeline->Failed()) {
            _failed = true;
            Close();
            return;
        }
        auto& state = _pipeline->GameSettings;
        state.CaptureName.clear();
        ++state.Frame;
        state.Fps = state.Fps * .95f + .05f / std::max(ctx.DeltaTime.count(), .001f);
        if (_options.Tour) Tour(state.Frame);
        if (_options.SkyTest) SkyTest(state.Frame);
        state.CameraCut = _cutRequested;
        _cutRequested = false;
        if (!_options.SkyTest) Move(_options.Tour ? 1.f / 60.f : dt);
        if (!state.Paused) state.Time += _options.Tour ? 1.f / 60.f : dt;
        if (!_materials.at("core")->SetFloat4("BaseColor", {.4f + .12f * std::sin(state.Time * 1.2f), 1, .86f, 1})) _failed = true;
        for (const auto& item : _animated) {
            if (item.Motion == "core")
                item.Component->SetWorldLocation(item.Position + Eigen::Vector3f{0, .18f * std::sin(state.Time), 0});
            else
                item.Component->SetWorldRotation(Rotation(item.Rotation + Eigen::Vector3f{0, state.Time * (item.Motion == "specimen" ? 13.f : 9.f), 0}));
        }
        for (size_t i = 0; i < _points.size(); ++i) {
            const float a = state.Time * .4f + float(i) * 2.0944f;
            const Eigen::Vector3f position{4.2f * std::cos(a), 2.9f + .8f * std::sin(a * 1.7f), 5 + 4.2f * std::sin(a)};
            _points[i]->SetWorldLocation(position);
            _orbs[i]->SetWorldLocation(position);
        }
        if (state.Frame % 45 == 0) _window->SetTitle(fmt::format("Tidal Atrium | {} | {:.0f} fps | WASD + RMB / 1-5 viewpoints / H help", _options.Backend, state.Fps));
        if (!_options.Tour && !_options.SkyTest && _options.Frames && state.Frame == _options.Frames - 2) state.CaptureName = "atrium";
        if (_options.Frames && state.Frame >= _options.Frames) Close();
    }

    void OnRenderFrameComplete(const AppRenderCompleteContext& ctx) override {
        if (_pipeline && ctx.GpuWorkCompleted) _pipeline->Complete(ctx.FlightIndex);
    }

    void OnShutdown() override {
        CaptureMouse(false);
        _keyboard.disconnect();
        _mouse.disconnect();
        _focus.disconnect();
        if (_pipeline) _failed |= _pipeline->Failed();
        for (auto* actor : _actors) GetWorld()->DestroyActor(actor);
        _actors.clear();
        _animated.clear();
        _points.clear();
        _orbs.clear();
        GetRenderSystem()->SetPipeline(nullptr);
        _pipeline = nullptr;
        _camera = nullptr;
        _materials.clear();
        _techniques.clear();
        _meshes.clear();
        _textures.clear();
        RADRAY_INFO_LOG("Tidal Atrium shutdown: {}", _failed ? "FAILED" : "clean");
    }

private:
    template <class T, class... Args>
    T* Spawn(Args&&... args) {
        auto* actor = GetWorld()->SpawnActor<Actor>();
        _actors.push_back(actor);
        auto* component = actor->AddComponent<T>(std::forward<Args>(args)...);
        actor->SetRootComponent(component);
        return component;
    }
    void Close() {
#if defined(RADRAY_PLATFORM_WINDOWS)
        if (_window) PostMessageW(static_cast<HWND>(_window->GetNativeHandler()), WM_CLOSE, 0, 0);
#else
        if (_window) _window->Destroy();
#endif
    }
    void CaptureMouse(bool capture) {
        if (capture == _captured || !_window) return;
        _captured = capture;
#if defined(RADRAY_PLATFORM_WINDOWS)
        if (capture) {
            SetCapture(static_cast<HWND>(_window->GetNativeHandler()));
            ShowCursor(FALSE);
            const auto origin = _window->ClientToScreen({0, 0}), size = _window->GetSize();
            RECT rect{origin.x(), origin.y(), origin.x() + size.x(), origin.y() + size.y()};
            ClipCursor(&rect);
            const auto center = _window->ClientToScreen(size / 2);
            SetCursorPos(center.x(), center.y());
        } else {
            ReleaseCapture();
            ClipCursor(nullptr);
            ShowCursor(TRUE);
        }
#endif
    }
    void Key(KeyCode key, Action action) {
        if (action == Action::RELEASED)
            _keys.erase(key);
        else
            _keys.insert(key);
        if (action != Action::PRESSED) return;
        if (key == KeyCode::ESCAPE) {
            if (_captured)
                CaptureMouse(false);
            else
                Close();
        }
        if (!_pipeline) return;
        auto& state = _pipeline->GameSettings;
        if (key >= KeyCode::NUM1 && key <= KeyCode::NUM5) Station(static_cast<uint32_t>(key) - static_cast<uint32_t>(KeyCode::NUM1));
        if (key == KeyCode::H || key == KeyCode::F1) state.Help = !state.Help;
        if (key == KeyCode::SPACE) state.Paused = !state.Paused;
        if (key == KeyCode::F2) state.Depth = !state.Depth;
        if (key == KeyCode::F3) state.Wireframe = !state.Wireframe;
        if (key == KeyCode::F4) state.Split = !state.Split;
        if (key == KeyCode::F5) state.Beacons = !state.Beacons;
        if (key == KeyCode::F6) state.History = !state.History;
        if (key == KeyCode::F7) state.RenderScale = state.RenderScale > 1 ? 1.f : 1.5f;
        if (key == KeyCode::TAB) state.ShowUi = !state.ShowUi;
    }
    void Station(uint32_t station) {
        const array<Eigen::Vector3f, 5> eyes{{{13, 8, -21}, {20, 3, -17}, {-10, 3, -13}, {0, 10, 5}, {27, 20, -8}}};
        const array<Eigen::Vector3f, 5> targets{{{0, 3, 7}, {15, 2.5f, -3}, {-16, 2, 0}, {0, 4, 24}, {0, 1, 7}}};
        const auto direction = (targets[station] - eyes[station]).normalized();
        _yaw = std::atan2(direction.x(), direction.z());
        _pitch = -std::asin(direction.y());
        _camera->SetWorldLocation(eyes[station]);
        ApplyRotation();
        if (_pipeline) _pipeline->GameSettings.Station = station;
        _cutRequested = true;
    }
    void ApplyRotation() { _camera->SetWorldRotation(Eigen::Quaternionf{Eigen::AngleAxisf{_yaw, Eigen::Vector3f::UnitY()} * Eigen::AngleAxisf{_pitch, Eigen::Vector3f::UnitX()}}); }
    void Move(float dt) {
        const auto held = [&](KeyCode key) { return _keys.contains(key) ? 1.f : 0.f; };
        _yaw += (held(KeyCode::RIGHT) - held(KeyCode::LEFT)) * dt * 1.3f;
        _pitch += (held(KeyCode::DOWN) - held(KeyCode::UP)) * dt * 1.3f;
#if defined(RADRAY_PLATFORM_WINDOWS)
        if (_captured && _window->IsFocused()) {
            const auto center = _window->ClientToScreen(_window->GetSize() / 2);
            POINT cursor{};
            if (GetCursorPos(&cursor)) {
                _yaw += (cursor.x - center.x()) * .0023f;
                _pitch += (cursor.y - center.y()) * .0023f;
                SetCursorPos(center.x(), center.y());
            }
        }
#endif
        _pitch = std::clamp(_pitch, -1.48f, 1.48f);
        ApplyRotation();
        const Eigen::Quaternionf orientation{Eigen::AngleAxisf{_yaw, Eigen::Vector3f::UnitY()} * Eigen::AngleAxisf{_pitch, Eigen::Vector3f::UnitX()}};
        Eigen::Vector3f direction = orientation * Eigen::Vector3f{held(KeyCode::D) - held(KeyCode::A), 0, held(KeyCode::W) - held(KeyCode::S)};
        direction.y() += held(KeyCode::E) - held(KeyCode::Q);
        if (direction.squaredNorm() > 0) {
            auto position = (_camera->GetEyePosition() + direction.normalized() * dt * (held(KeyCode::LEFT_SHIFT) || held(KeyCode::RIGHT_SHIFT) ? 18.f : 6.f)).eval();
            position.y() = std::clamp(position.y(), .65f, 60.f);
            _camera->SetWorldLocation(position);
        }
    }
    void SkyTest(uint32_t frame) {
        struct Probe {
            const char* Name;
            Eigen::Vector3f Angles;
            float Fov{58};
        };
        const array<Probe, 8> probes{{{"sky-level", {0, 0, 0}}, {"sky-up", {-20, 0, 0}}, {"sky-down", {20, 0, 0}}, {"sky-yaw", {0, 90, 0}}, {"sky-translated", {0, 0, 0}}, {"sky-roll", {0, 0, 25}}, {"sky-split", {0, 0, 0}}, {"sky-narrow", {-10, 0, 0}, 35}}};
        const uint32_t index = std::min((frame - 1) / 10, uint32_t(probes.size() - 1));
        const auto& probe = probes[index];
        auto& state = _pipeline->GameSettings;
        state.Paused = true;
        state.ShowUi = false;
        state.RenderScale = 1;
        state.Split = index == 6;
        _camera->SetPerspective(Radian(probe.Fov), .1f, 250);
        // Keep geometry outside the far plane so the captures measure only the sky.
        _camera->SetWorldLocation(index == 4 ? Eigen::Vector3f{1050, 60, 1100} : Eigen::Vector3f{1000, 50, 1000});
        _camera->SetWorldRotation(Rotation(probe.Angles));
        _cutRequested = frame % 10 == 1;
        if (frame % 10 == 8) state.CaptureName = probe.Name;
    }
    void Tour(uint32_t frame) {
        auto& state = _pipeline->GameSettings;
        const auto tap = [&](KeyCode key) { _window->EventKeyboard()(key,Action::PRESSED); _window->EventKeyboard()(key,Action::RELEASED); };
        if (frame == 10) {
            _moveStart = _camera->GetEyePosition();
            _window->EventKeyboard()(KeyCode::W, Action::PRESSED);
        }
        if (frame == 20) {
            _window->EventKeyboard()(KeyCode::W, Action::RELEASED);
            if ((_camera->GetEyePosition() - _moveStart).norm() < .5f) {
                _failed = true;
                RADRAY_ERR_LOG("Atrium tour movement check failed");
            }
            Station(0);
        }
#if defined(RADRAY_PLATFORM_WINDOWS)
        if (frame == 21) _window->Focus();
        if (frame == 22 && _window->IsFocused()) {
            _lookStart = _yaw;
            _lookChecked = true;
            _window->EventTouch()(0, 0, MouseButton::BUTTON_RIGHT, Action::PRESSED);
            const auto center = _window->ClientToScreen(_window->GetSize() / 2);
            SetCursorPos(center.x() + 25, center.y() + 12);
        }
        if (frame == 23 && _lookChecked) {
            _window->EventTouch()(0, 0, MouseButton::BUTTON_RIGHT, Action::RELEASED);
            if (std::abs(_yaw - _lookStart) < .01f || _captured) {
                _failed = true;
                RADRAY_ERR_LOG("Atrium mouse-look check failed");
            } else
                RADRAY_INFO_LOG("Atrium input verified: keyboard movement, captured mouse look, cursor release");
            Station(0);
        }
#endif
        if (frame == 30) state.CaptureName = "01-arrival";
        if (frame == 45) {
            tap(KeyCode::SPACE);
            _camera->SetWorldLocation({8, 5, -5});
            const auto d = Eigen::Vector3f{-8, -1.2f, 10}.normalized();
            _yaw = std::atan2(d.x(), d.z());
            _pitch = -std::asin(d.y());
            _cutRequested = true;
        }
        if (frame == 60) state.CaptureName = "02-light-court";
        if (frame == 70) tap(KeyCode::F2);
        if (frame == 80) state.CaptureName = "03-depth-disabled";
        if (frame == 90) {
            tap(KeyCode::SPACE);
            tap(KeyCode::F2);
            tap(KeyCode::NUM2);
        }
        if (frame == 110) state.CaptureName = "04-chromatic-walk";
        if (frame == 120) {
            tap(KeyCode::SPACE);
            tap(KeyCode::NUM3);
        }
        if (frame == 140) state.CaptureName = "05-material-library";
        if (frame == 150) tap(KeyCode::F3);
        if (frame == 160) state.CaptureName = "06-wireframe";
        if (frame == 170) {
            tap(KeyCode::SPACE);
            tap(KeyCode::F3);
            tap(KeyCode::NUM4);
        }
        if (frame == 195) state.CaptureName = "07-signal-garden";
        if (frame == 200) tap(KeyCode::F6);
        if (frame == 215) state.CaptureName = "08-history-disabled";
        if (frame == 225) {
            tap(KeyCode::F6);
            tap(KeyCode::NUM5);
            tap(KeyCode::F4);
        }
        if (frame == 245) state.CaptureName = "09-split-view";
        if (frame == 250) tap(KeyCode::F5);
        if (frame == 260) {
            state.CaptureName = "10-layers-disabled";
        }
        if (frame == 270) {
            tap(KeyCode::F4);
            tap(KeyCode::F5);
            tap(KeyCode::NUM1);
            _window->SetSize(1000, 700);
        }
        if (frame == 290) state.CaptureName = "11-resized";
        if (frame == 300) _window->SetSize(int(_options.Width), int(_options.Height));
        if (frame == 325) state.CaptureName = "12-restored";
        if (frame == 335) tap(KeyCode::SPACE);
        if (frame == 336) tap(KeyCode::F7);
        if (frame == 340) state.CaptureName = "13-native-resolution";
        if (frame == 345) {
            tap(KeyCode::F7);
            tap(KeyCode::TAB);
        }
        if (frame == 350) state.CaptureName = "14-gallery";
    }
    bool CreateMaterials();
    bool CreateScene();
    struct Animated {
        DisplayComponent* Component;
        Eigen::Vector3f Position, Rotation;
        string Motion;
    };
    Options _options;
    Nullable<NativeWindow*> _window{nullptr};
    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<AtriumPipeline*> _pipeline{nullptr};
    vector<Actor*> _actors;
    unordered_map<string, StreamingAssetRef<StaticMesh>> _meshes;
    unordered_map<string, StreamingAssetRef<TextureAsset>> _textures;
    vector<unique_ptr<MaterialTechnique>> _techniques;
    unordered_map<string, unique_ptr<Material>> _materials;
    vector<Animated> _animated;
    vector<PointLightComponent*> _points;
    vector<DisplayComponent*> _orbs;
    unordered_set<KeyCode> _keys;
    sigslot::scoped_connection _keyboard, _mouse, _focus;
    Eigen::Vector3f _moveStart{Eigen::Vector3f::Zero()};
    float _yaw{0}, _pitch{0}, _loadingTime{0}, _lookStart{0};
    bool _captured{false}, _failed{false}, _ready{false}, _cutRequested{true}, _lookChecked{false};
};

bool AtriumApplication::CreateMaterials() {
    const auto surface = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "pipelines/atrium/surface.hlsl", .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()});
    const auto depth = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "pipelines/forward/depth_only.hlsl", .LayoutRecipe = ForwardPipeline::GetDepthOnlyLayoutRecipe()});
    if (!surface || !depth) return false;
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.Primitive.UnclippedDepth = false;
    for (bool prepass : {true, false}) {
        vector<MaterialPassDesc> passes{{"ForwardLit", surface.Get(), "ForwardMaterial", state}};
        if (prepass) passes.push_back({"DepthOnly", depth.Get(), "", state});
        auto technique = MaterialTechnique::Create(std::move(passes), "ForwardLit");
        if (!technique) return false;
        _techniques.push_back(technique.Release());
    }
    struct Desc {
        const char* Name;
        const char* Texture;
        Eigen::Vector4f Color;
        float Emission, Scale, Gloss;
        bool Nearest{false}, NoDepth{false};
    };
    const vector<Desc> descriptions{
        {"dark", "basalt", {.06f, .1f, .14f, 1}, 0, 8, .1f}, {"stone", "basalt", {.28f, .38f, .42f, 1}, 0, 12, .2f}, {"navy", "white", {.035f, .075f, .11f, 1}, 0, 1, .4f}, {"limestone", "limestone", {.88f, .87f, .8f, 1}, 0, 2, .25f}, {"copper", "white", {.65f, .30f, .12f, 1}, 0, 1, .9f}, {"gold", "white", {.88f, .57f, .25f, 1}, .35f, 1, .8f}, {"cyan", "white", {.04f, .75f, .65f, 1}, 2.8f, 1, .4f}, {"warm", "white", {1, .52f, .18f, 1}, 3, 1, .2f}, {"core", "white", {.45f, 1, .86f, 1}, 4, 1, .6f}, {"pearl", "limestone", {.85f, .88f, .87f, 1}, 0, 1, .7f}, {"foliage", "white", {.045f, .24f, .18f, 1}, 0, 1, .25f}, {"glass_cyan", "white", {.05f, .65f, .68f, .28f}, .15f, 1, .8f}, {"glass_amber", "white", {.95f, .42f, .1f, .3f}, .15f, 1, .8f}, {"glass_rose", "white", {.7f, .12f, .25f, .3f}, .15f, 1, .8f}, {"grid_nearest", "sampler_grid", {1, 1, 1, 1}, .15f, 4, .1f, true}, {"grid_linear", "sampler_grid", {1, 1, 1, 1}, .15f, 4, .1f}, {"mural", "tidal_mural", {1, 1, 1, 1}, 1.25f, 1, 0, false, true}, {"contact", "contact", {1, 1, 1, .99f}, 0, 1, 0, false, true}};
    auto make = [&](const Desc& d) {
        auto material = Material::Create(_techniques[d.NoDepth ? 1 : 0].get());
        if (!material) return false;
        render::SamplerDescriptor sampler{};
        sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::Repeat;
        sampler.MinFilter = sampler.MagFilter = sampler.MipmapFilter = d.Nearest ? render::FilterMode::Nearest : render::FilterMode::Linear;
        sampler.LodMax = d.Nearest ? 0.f : 1000.f;
        if (!material->SetFloat4("BaseColor", d.Color) || !material->SetFloat4("Surface", {d.Emission, d.Scale, d.Gloss, 0}) ||
            !material->SetTexture("AlbedoTexture", _textures.at(d.Texture)) || !material->SetSampler("LinearSampler", sampler)) return false;
        if (std::string_view{d.Name} == "contact" && !material->SetFloat4("Surface", {0, 1, 0, 1})) return false;
        if (d.Color.w() < 1) {
            material->SetRenderQueue(RenderQueue::Transparent);
            auto& pipeline = material->GetPipelineState();
            pipeline.Blend = render::BlendState::Default();
            pipeline.Blend->Color = {render::BlendFactor::SrcAlpha, render::BlendFactor::OneMinusSrcAlpha, render::BlendOperation::Add};
            pipeline.DepthStencil.DepthWriteEnable = false;
        }
        _materials.emplace(d.Name, material.Release());
        return true;
    };
    for (const auto& d : descriptions)
        if (!make(d)) return false;
    for (const auto name : {"welcome", "light", "glass", "material", "signal", "observatory", "nearest", "linear", "streams"})
        if (!make({name, name, {1, 1, 1, 1}, 1, 1, 0, false, true})) return false;
    return true;
}

bool AtriumApplication::CreateScene() {
    _meshes.emplace("split_vase", MakeSplitMesh(*this, *_meshes.at("vase").Get()));
    if (!_meshes.at("split_vase").IsReady()) return false;
    const auto doc = JsonDocument::ParseFile(AssetsRoot() / "tidal_atrium/scene.json");
    if (!doc || !doc->Root()["objects"].IsArray()) return false;
    const auto objects = doc->Root()["objects"];
    for (size_t i = 0; i < objects.Size(); ++i) {
        const auto object = objects.At(i);
        string mesh{object["mesh"].AsString()}, material{object["material"].AsString()};
        if (!_meshes.contains(mesh) || !_materials.contains(material)) return false;
        const auto vector3 = [&](std::string_view key, Eigen::Vector3f& out) {
            const auto value = object[key];
            if (!value.IsArray() || value.Size() != 3) return false;
            for (size_t j = 0; j < 3; ++j) {
                if (!value.At(j).IsNumber()) return false;
                out[Eigen::Index(j)] = float(value.At(j).AsDouble());
            }
            return out.allFinite();
        };
        Eigen::Vector3f position, scale, rotation;
        if (!vector3("p", position) || !vector3("s", scale) || !vector3("r", rotation) || (scale.array() == 0).any()) return false;
        const bool split = object["name"].AsString() == "Material specimen 4";
        if (split) mesh = "split_vase";
        auto& asset = _meshes.at(mesh);
        vector<Nullable<Material*>> materials(asset.Get()->GetSections().size(), _materials.at(material).get());
        if (split) {
            materials[0] = _materials.at("copper").get();
            materials[1] = _materials.at("pearl").get();
            position.y() = 1.1f;
        }
        auto* component = Spawn<DisplayComponent>(asset, std::move(materials), object["layer"].AsUint(1));
        component->SetWorldLocation(position);
        component->SetRelativeScale(scale);
        component->SetWorldRotation(Rotation(rotation));
        const string motion{object["motion"].AsString()};
        if (!motion.empty()) _animated.push_back({component, position, rotation, motion});
    }
    auto* sun = Spawn<DirectionalLightComponent>();
    sun->SetIntensity(3.5f);
    sun->SetLightColor({1, .84f, .64f});
    sun->SetWorldRotation(Rotation({42, -32, 0}));
    sun->SetCastShadow(false);
    const array<Eigen::Vector3f, 3> colors{{{.12f, 1, .85f}, {1, .35f, .1f}, {.2f, .4f, 1}}};
    for (size_t i = 0; i < 3; ++i) {
        auto* light = Spawn<PointLightComponent>();
        light->SetIntensity(180);
        light->SetLightColor(colors[i]);
        light->SetAttenuationRadius(15);
        light->SetCastShadow(false);
        _points.push_back(light);
        auto* orb = Spawn<DisplayComponent>(_meshes.at("sphere"), vector<Nullable<Material*>>{_materials.at(i == 1 ? "warm" : "cyan").get()}, 1u);
        orb->SetRelativeScale({.16f, .16f, .16f});
        _orbs.push_back(orb);
    }
    return true;
}

}  // namespace
}  // namespace radray::atrium

int main(int argc, char** argv) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    RADRAY_ERR_LOG("Tidal Atrium requires RADRAY_ENABLE_SHADER_JIT");
    return 1;
#else
    using namespace radray;
    constexpr std::string_view usage = "Usage: example_tidal_atrium [--backend d3d12|vulkan] [--multithread] [--valid-layer] [--tour|--sky-test] [--frames N] [--capture-dir PATH] [--width N] [--height N]";
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
        RADRAY_INFO_LOG("{}", usage);
        return 0;
    }
    atrium::Options options;
    if (!atrium::ParseOptions(argc, argv, options)) {
        RADRAY_ERR_LOG("{}", usage);
        return 1;
    }
    const std::filesystem::path project{RADRAY_PROJECT_DIR_DEFAULT};
    const ApplicationRuntimeDescriptor descriptor{
        .Backend = options.Backend, .EnableValidation = options.Validation, .Multithreaded = options.Multithread, .AppName = "Tidal Atrium", .EngineName = "RadRay", .AssetRoot = atrium::AssetsRoot(), .ShaderSourceRoot = project / "shaderlib", .ShaderIncludePaths = {project / "shaderlib"}, .WindowTitle = "Tidal Atrium / RadRay", .WindowWidth = int(options.Width), .WindowHeight = int(options.Height), .BackBufferCount = 3, .FlightDataCount = 2, .BackBufferFormat = render::TextureFormat::BGRA8_UNORM, .PresentMode = render::PresentMode::Immediate};
    std::atomic_bool loggedFailure{false};
    SetLogCallback(+[](LogLevel level, std::string_view message, void* data) {
        if (level == LogLevel::Err || level == LogLevel::Critical ||
            (level == LogLevel::Warn && message.find("Validation Layer") != std::string_view::npos))
            static_cast<std::atomic_bool*>(data)->store(true); }, &loggedFailure);
    int result = 0;
    {
        atrium::AtriumApplication app{options};
        result = app.Run(descriptor);
        if (app.Failed()) result = 1;
    }
    ClearLogCallback();
    return loggedFailure ? 1 : result;
#endif
}
