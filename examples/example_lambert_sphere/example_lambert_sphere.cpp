#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <span>
#include <string_view>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/window_manager.h>
#ifdef RADRAY_ENABLE_IMGUI
#include <radray/runtime/imgui/imgui_system.h>
#endif
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/forward_pipeline/forward_pipeline.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>
#ifdef RADRAY_PLATFORM_WINDOWS
#include <radray/platform/win32_headers.h>
#endif

namespace {

using namespace radray;

struct ExampleOptions {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    bool Multithreaded{false};
    bool EnableValidation{false};
    bool ImGui{true};
    uint32_t Frames{0};
};

[[maybe_unused]] bool ParseOptions(int argc, char** argv, ExampleOptions* options) {
    if (options == nullptr) {
        return false;
    }
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument =
            argv[index] != nullptr ? argv[index] : "";
        if (argument == "--vulkan") {
            options->Backend = render::RenderBackend::Vulkan;
        } else if (argument == "--d3d12") {
            options->Backend = render::RenderBackend::D3D12;
        } else if (argument == "--multithread") {
            options->Multithreaded = true;
        } else if (argument == "--valid-layer") {
            options->EnableValidation = true;
        } else if (argument == "--imgui" || argument == "--no-imgui") {
            options->ImGui = argument == "--imgui";
        } else if (argument == "--frames" && index + 1 < argc) {
            const std::string_view value{argv[++index]};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options->Frames);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !options->Frames) return false;
        } else {
            RADRAY_WARN_LOG("unknown example argument: {}", argument);
        }
    }
    return true;
}

[[maybe_unused]] std::filesystem::path FindAssetsRoot() {
    if (const char* environment = std::getenv("RADRAY_ASSETS_DIR");
        environment != nullptr && environment[0] != '\0') {
        return std::filesystem::path{environment};
    }
    return std::filesystem::path{RADRAY_ASSETS_DIR_DEFAULT};
}

[[maybe_unused]] std::filesystem::path FindProjectRoot() {
    return std::filesystem::path{RADRAY_PROJECT_DIR_DEFAULT};
}

class LambertApplication final : public Application {
public:
    explicit LambertApplication(ExampleOptions options) : _options(options) {}

protected:
#ifdef RADRAY_ENABLE_IMGUI
    void ConfigureImGui(ImGuiSystemDescriptor& descriptor) override { descriptor.Enabled = _options.ImGui; }
    void OnImGui() override {
        ImGui::SetNextWindowSize({310, 170}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Lambert sphere")) {
            ImGui::TextUnformatted(_meshAssigned ? "Mesh ready" : "Loading mesh...");
            if (_material && ImGui::ColorEdit4("Base color", _color.data())) _material->SetFloat4("BaseColor", _color);
            if (_material && ImGui::Checkbox("Wireframe", &_wireframe)) {
                auto state = _material->GetPipelineState();
                state.Primitive.Poly = _wireframe ? render::PolygonMode::Line : render::PolygonMode::Fill;
                _material->GetPipelineState() = state;
                _material->SetPassPipelineState("DepthOnly", state);
            }
        }
        ImGui::End();
    }
#endif
    void OnInit() override {
        if (GetWorld() == nullptr || GetRenderSystem() == nullptr ||
            GetAssetManager() == nullptr || GetGpuSystem() == nullptr) {
            RADRAY_ERR_LOG("example_lambert_sphere: runtime services are incomplete");
            return;
        }

        _mesh = GetAssetManager()->Load<StaticMesh>(
            "example_lambert_sphere/lambert_sphere.obj");
        _texture = GetAssetManager()->Load<TextureAsset>(
            "example_lambert_sphere/checker.png");
        if (!_mesh.IsValid() || !_texture.IsValid()) {
            RADRAY_ERR_LOG("example_lambert_sphere: asset requests failed");
            return;
        }

        ShaderProgramRequest request{
            .SourceName = "pipelines/forward/forward.hlsl",
            .LayoutRecipe = ForwardPipeline::GetLayoutRecipe()};
        request.Assignments.push_back(shader::KeywordAssignment{.Name = "QUALITY", .Value = "high"});
        const Nullable<ShaderProgram*> program =
            GetRenderSystem()->GetOrCreateShaderProgram(request);
        if (!program.HasValue()) {
            RADRAY_ERR_LOG("example_lambert_sphere: forward shader program creation failed");
            return;
        }
        vector<MaterialPassDesc> passes{{"ForwardLit", program.Get(), "ForwardMaterial", {}}};
        const auto depth = GetRenderSystem()->GetOrCreateShaderProgram({.SourceName = "pipelines/forward/depth_only.hlsl", .LayoutRecipe = ForwardPipeline::GetDepthOnlyLayoutRecipe()});
        if (depth)
            passes.push_back({"DepthOnly", depth.Get(), "", {}});
        else
            RADRAY_WARN_LOG("example_lambert_sphere: DepthOnly unavailable; using Forward opaque rendering");
        auto technique = MaterialTechnique::Create(std::move(passes), "ForwardLit");
        if (!technique) return;
        _technique = technique.Release();
        Nullable<unique_ptr<Material>> material = Material::Create(_technique.get());
        if (!material.HasValue()) {
            RADRAY_ERR_LOG("example_lambert_sphere: material creation failed");
            return;
        }
        _material = material.Release();
        const render::SamplerDescriptor sampler{
            .AddressS = render::AddressMode::Repeat,
            .AddressT = render::AddressMode::Repeat,
            .AddressR = render::AddressMode::Repeat,
            .MinFilter = render::FilterMode::Linear,
            .MagFilter = render::FilterMode::Linear,
            .MipmapFilter = render::FilterMode::Linear,
            .LodMin = 0.0f,
            .LodMax = 1000.0f};
        if (!_material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()) ||
            !_material->SetTexture("AlbedoTexture", _texture) ||
            !_material->SetSampler("LinearSampler", sampler)) {
            RADRAY_ERR_LOG("example_lambert_sphere: material parameter setup failed");
            return;
        }

        _cameraActor = GetWorld()->SpawnActor<Actor>();
        CameraComponent* camera =
            _cameraActor.Get()->AddComponent<CameraComponent>();
        _cameraActor.Get()->SetRootComponent(camera);
        camera->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        camera->SetPerspective(Radian(55.0f), 0.1f, 100.0f);

        _meshActor = GetWorld()->SpawnActor<Actor>();
        _meshComponent =
            _meshActor.Get()->AddComponent<StaticMeshComponent>();
        _meshActor.Get()->SetRootComponent(_meshComponent.Get());
        _meshComponent.Get()->SetMaterial(0, _material.get());

        _lightActor = GetWorld()->SpawnActor<Actor>();
        DirectionalLightComponent* light =
            _lightActor.Get()->AddComponent<DirectionalLightComponent>();
        _lightActor.Get()->SetRootComponent(light);
        light->SetIntensity(3.0f);
        light->SetCastShadow(false);

        GetRenderSystem()->SetPipeline(make_unique<ForwardPipeline>(
            this,
            GetWorld()->GetScene(),
            camera));
    }

    void OnUpdate(const AppUpdateContext&) override {
        if (_options.Frames && ++_frame >= _options.Frames) {
            auto* window = GetWindowManager()->GetMainWindow()->GetNativeWindow();
#ifdef RADRAY_PLATFORM_WINDOWS
            PostMessageW(static_cast<HWND>(window->GetNativeHandler()), WM_CLOSE, 0, 0);
#else
            window->Destroy();
#endif
        }
        if (!_meshAssigned && _mesh.IsReady() && _meshComponent.HasValue()) {
            _meshComponent.Get()->SetStaticMesh(_mesh);
            _meshAssigned = true;
        } else if (!_meshFailureReported &&
                   (_mesh.IsFaulted() || _mesh.IsCanceled())) {
            RADRAY_ERR_LOG("example_lambert_sphere: mesh loading failed");
            _meshFailureReported = true;
        }
        if (!_textureFailureReported &&
            (_texture.IsFaulted() || _texture.IsCanceled())) {
            RADRAY_ERR_LOG("example_lambert_sphere: texture loading failed");
            _textureFailureReported = true;
        }
    }

    void OnShutdown() override {
        World* world = GetWorld();
        if (world != nullptr) {
            if (_meshActor.HasValue()) {
                world->DestroyActor(_meshActor.Get());
            }
            if (_lightActor.HasValue()) {
                world->DestroyActor(_lightActor.Get());
            }
            if (_cameraActor.HasValue()) {
                world->DestroyActor(_cameraActor.Get());
            }
        }
        _meshActor = nullptr;
        _lightActor = nullptr;
        _cameraActor = nullptr;
        _meshComponent = nullptr;
        if (GetRenderSystem() != nullptr) {
            GetRenderSystem()->SetPipeline(nullptr);
        }
        _material.reset();
        _texture.Reset();
        _mesh.Reset();
    }

private:
    ExampleOptions _options;
    uint32_t _frame{0};
#ifdef RADRAY_ENABLE_IMGUI
    Eigen::Vector4f _color{Eigen::Vector4f::Ones()};
    bool _wireframe{false};
#endif
    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<TextureAsset> _texture;
    unique_ptr<MaterialTechnique> _technique;
    unique_ptr<Material> _material;
    Nullable<Actor*> _cameraActor{nullptr};
    Nullable<Actor*> _meshActor{nullptr};
    Nullable<Actor*> _lightActor{nullptr};
    Nullable<StaticMeshComponent*> _meshComponent{nullptr};
    bool _meshAssigned{false};
    bool _meshFailureReported{false};
    bool _textureFailureReported{false};
};

}  // namespace

int main(int argc, char** argv) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    (void)argc;
    (void)argv;
    RADRAY_ERR_LOG("example_lambert_sphere requires RADRAY_ENABLE_SHADER_JIT");
    return 1;
#else
    ExampleOptions options;
    if (!ParseOptions(argc, argv, &options)) {
        return 1;
    }
#if !defined(RADRAY_ENABLE_D3D12)
    if (options.Backend == render::RenderBackend::D3D12) {
        RADRAY_ERR_LOG("example_lambert_sphere: D3D12 backend is disabled");
        return 1;
    }
#endif
#if !defined(RADRAY_ENABLE_VULKAN)
    if (options.Backend == render::RenderBackend::Vulkan) {
        RADRAY_ERR_LOG("example_lambert_sphere: Vulkan backend is disabled");
        return 1;
    }
#endif

    const std::filesystem::path projectRoot = FindProjectRoot();
    const ApplicationRuntimeDescriptor descriptor{
        .Backend = options.Backend,
        .EnableValidation = options.EnableValidation,
        .Multithreaded = options.Multithreaded,
        .AppName = "example_lambert_sphere",
        .EngineName = "RadRay",
        .RenderCachePath = {},
        .AssetRoot = FindAssetsRoot(),
        .ShaderSourceRoot = projectRoot / "shaderlib",
        .ShaderIncludePaths = {projectRoot / "shaderlib"},
        .WindowTitle = "example_lambert_sphere",
        .WindowWidth = 1280,
        .WindowHeight = 720,
        .BackBufferCount = 3,
        .FlightDataCount = 2,
        .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
        .PresentMode = render::PresentMode::FIFO};
    LambertApplication application{options};
    return application.Run(descriptor);
#endif
}
