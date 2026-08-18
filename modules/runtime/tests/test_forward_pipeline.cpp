// Covers the ForwardPipeline orchestration end to end: draw collection, per-program
// view/object constant packing, material set preparation and the recorded draw. Every
// other test in the runtime drives the pieces directly, so this is the only place where
// PrepareCamera and the draw pass actually run against a live swapchain.
#include <radray/runtime/forward_pipeline/forward_pipeline.h>

#include <radray/logger.h>
#include <radray/runtime/application.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/directional_light_component.h>
#include <radray/runtime/components/point_light_component.h>
#include <radray/runtime/components/static_mesh_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_program.h>
#include <radray/runtime/static_mesh.h>
#include <radray/runtime/texture_asset.h>
#include <radray/runtime/window_manager.h>
#include <radray/window/native_window.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <span>

#if defined(RADRAY_PLATFORM_WINDOWS) || defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace radray {
namespace {

constexpr uint32_t kFrameCount = 4;
constexpr AssetId kMeshId{
    0x11111111, 0x2222, 0x3333, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
constexpr AssetId kTextureId{
    0x22222222, 0x3333, 0x4444, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc};

struct QuadVertex {
    float Position[3];
    float Normal[3];
    float UV[2];
};

// A unit quad facing -Z so the camera at -Z looks at its front face.
MeshResource MakeQuadMeshResource() {
    constexpr std::array<QuadVertex, 4> vertices{
        QuadVertex{{-1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},
        QuadVertex{{1.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
        QuadVertex{{1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
        QuadVertex{{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}}};
    constexpr std::array<uint32_t, 6> indices{0, 1, 2, 0, 2, 3};

    MeshResource resource;
    resource.Name = "forward-pipeline-test-quad";
    resource.Bins.emplace_back(std::span<const byte>{
        reinterpret_cast<const byte*>(vertices.data()), sizeof(vertices)});
    resource.Bins.emplace_back(std::span<const byte>{
        reinterpret_cast<const byte*>(indices.data()), sizeof(indices)});

    MeshPrimitive primitive;
    primitive.VertexCount = static_cast<uint32_t>(vertices.size());
    primitive.Topology = PrimitiveTopology::TriangleList;
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = string{VertexSemantics::POSITION},
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 3,
        .Offset = offsetof(QuadVertex, Position),
        .Stride = sizeof(QuadVertex)});
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = string{VertexSemantics::NORMAL},
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 3,
        .Offset = offsetof(QuadVertex, Normal),
        .Stride = sizeof(QuadVertex)});
    primitive.VertexBuffers.push_back(VertexBufferEntry{
        .Semantic = string{VertexSemantics::TEXCOORD},
        .SemanticIndex = 0,
        .BufferIndex = 0,
        .Type = VertexDataType::FLOAT,
        .ComponentCount = 2,
        .Offset = offsetof(QuadVertex, UV),
        .Stride = sizeof(QuadVertex)});
    primitive.IndexBuffer = IndexBufferEntry{
        .BufferIndex = 1,
        .IndexCount = static_cast<uint32_t>(indices.size()),
        .Offset = 0,
        .Stride = sizeof(uint32_t)};
    resource.Primitives.push_back(std::move(primitive));
    return resource;
}

Nullable<unique_ptr<TextureAsset>> MakeWhiteTexture(render::Device& device) {
    constexpr render::TextureFormat format = render::TextureFormat::RGBA8_UNORM;
    Nullable<unique_ptr<render::Texture>> texture = device.CreateTexture(
        render::TextureDescriptor{
            .Dim = render::TextureDimension::Dim2D,
            .Width = 1,
            .Height = 1,
            .DepthOrArraySize = 1,
            .MipLevels = 1,
            .SampleCount = 1,
            .Format = format,
            .Memory = render::MemoryType::Device,
            .Usage = render::TextureUse::Resource,
            .Hints = render::ResourceHint::None});
    if (!texture.HasValue()) {
        return nullptr;
    }
    unique_ptr<render::Texture> textureObject = texture.Release();
    Nullable<unique_ptr<render::TextureView>> view = device.CreateTextureView(
        render::TextureViewDescriptor{
            .Target = textureObject.get(),
            .Dim = render::TextureDimension::Dim2D,
            .Format = format,
            .Range = render::SubresourceRange{0, 1, 0, 1},
            .Usage = render::TextureViewUsage::Resource});
    if (!view.HasValue()) {
        return nullptr;
    }
    return make_unique<TextureAsset>(
        &device,
        "forward-pipeline-test-texture",
        std::move(textureObject),
        view.Release());
}

/// Result the test asserts on after the app loop returns.
struct ForwardPipelineRunResult {
    bool InitSucceeded{false};
    bool MeshAssigned{false};
    uint32_t FramesRun{0};
    size_t PipelineStateCount{0};
    bool MaterialSetResident{false};
    bool SawError{false};
    string FirstError;
};

class ForwardPipelineTestApp final : public Application {
public:
    explicit ForwardPipelineTestApp(ForwardPipelineRunResult* result) noexcept
        : _result(result) {}

protected:
    void OnInit() override {
        if (GetWorld() == nullptr || GetRenderSystem() == nullptr ||
            GetAssetManager() == nullptr || GetGpuSystem() == nullptr) {
            Fail("runtime services are incomplete");
            return;
        }

        Nullable<unique_ptr<TextureAsset>> texture = MakeWhiteTexture(*GetDevice());
        if (!texture.HasValue()) {
            Fail("texture creation failed");
            return;
        }
        _texture = GetAssetManager()->AddReady<TextureAsset>(
            kTextureId, texture.Release());
        _mesh = GetAssetManager()->Load<StaticMesh>(AssetLoadRequest{
            .Id = kMeshId,
            .Task = LoadStaticMesh(
                GetGpuSystem()->GetFrameUploadScheduler(),
                MakeQuadMeshResource()),
            .DebugName = "forward-pipeline-test-quad"});
        if (!_texture.IsValid() || !_mesh.IsValid()) {
            Fail("asset registration failed");
            return;
        }

        const BindingGroupPlan groups = ForwardPipeline::GetBindingGroupPlan();
        const uint32_t dynamicGroups[]{
            groups.ViewGroup,
            groups.MaterialGroup,
            groups.ObjectGroup};
        const shader::KeywordAssignment assignment{.Name = "QUALITY", .Value = "high"};
        const Nullable<ShaderProgram*> program =
            GetRenderSystem()->GetOrCreateShaderProgram(
                "pipelines/forward/forward.hlsl",
                std::span{&assignment, 1},
                render::ShaderLayoutPolicy{.DynamicBufferGroups = dynamicGroups});
        if (!program.HasValue()) {
            Fail("forward shader program creation failed");
            return;
        }
        _program = program.Get();

        Nullable<unique_ptr<Material>> material = Material::Create(
            _program,
            groups,
            GetGpuSystem()->GetFlightDataCount());
        if (!material.HasValue()) {
            Fail("material creation failed");
            return;
        }
        _material = material.Release();
        const render::SamplerDescriptor sampler{
            .MinFilter = render::FilterMode::Linear,
            .MagFilter = render::FilterMode::Linear};
        if (!_material->SetFloat4("BaseColor", Eigen::Vector4f::Ones()) ||
            !_material->SetTexture("AlbedoTexture", _texture) ||
            !_material->SetSampler("LinearSampler", sampler)) {
            Fail("material parameter setup failed");
            return;
        }

        _cameraActor = GetWorld()->SpawnActor<Actor>();
        CameraComponent* camera = _cameraActor.Get()->AddComponent<CameraComponent>();
        _cameraActor.Get()->SetRootComponent(camera);
        camera->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        camera->SetPerspective(Radian(55.0f), 0.1f, 100.0f);

        _meshActor = GetWorld()->SpawnActor<Actor>();
        _meshComponent = _meshActor.Get()->AddComponent<StaticMeshComponent>();
        _meshActor.Get()->SetRootComponent(_meshComponent.Get());
        _meshComponent.Get()->SetMaterial(0, _material.get());

        // One light of each kind so both loops in FillViewParameters run.
        _dirLightActor = GetWorld()->SpawnActor<Actor>();
        DirectionalLightComponent* dirLight =
            _dirLightActor.Get()->AddComponent<DirectionalLightComponent>();
        _dirLightActor.Get()->SetRootComponent(dirLight);
        dirLight->SetIntensity(3.0f);
        dirLight->SetCastShadow(false);

        _pointLightActor = GetWorld()->SpawnActor<Actor>();
        PointLightComponent* pointLight =
            _pointLightActor.Get()->AddComponent<PointLightComponent>();
        _pointLightActor.Get()->SetRootComponent(pointLight);
        pointLight->SetWorldLocation(Eigen::Vector3f{1.0f, 1.0f, -2.0f});
        pointLight->SetIntensity(2.0f);

        GetRenderSystem()->SetPipeline(
            make_unique<ForwardPipeline>(this, GetWorld()->GetScene(), camera));
        _result->InitSucceeded = true;
    }

    void OnUpdate(const AppUpdateContext&) override {
        if (!_result->MeshAssigned && _mesh.IsReady() && _meshComponent.HasValue()) {
            _meshComponent.Get()->SetStaticMesh(_mesh);
            _result->MeshAssigned = true;
        } else if (_mesh.IsFaulted() || _mesh.IsCanceled()) {
            Fail("mesh loading failed");
        }

        // Count only frames where the draw actually had geometry, then ask the window
        // to close. There is no headless mode, so this is how the loop terminates.
        if (_result->MeshAssigned) {
            ++_result->FramesRun;
        }
        if (_result->FramesRun >= kFrameCount || _result->SawError) {
            RequestClose();
        }
    }

    void OnShutdown() override {
        if (_program != nullptr) {
            _result->PipelineStateCount = _program->GetGraphicsPipelineStateCount();
        }
        if (_material != nullptr) {
            for (uint32_t flight = 0;
                 flight < GetGpuSystem()->GetFlightDataCount();
                 ++flight) {
                if (_material->GetResidentParameterSet(flight).HasValue()) {
                    _result->MaterialSetResident = true;
                    break;
                }
            }
        }
        World* world = GetWorld();
        if (world != nullptr) {
            for (Nullable<Actor*>* actor :
                 {&_meshActor, &_pointLightActor, &_dirLightActor, &_cameraActor}) {
                if (actor->HasValue()) {
                    world->DestroyActor(actor->Get());
                }
                *actor = nullptr;
            }
        }
        _meshComponent = nullptr;
        if (GetRenderSystem() != nullptr) {
            GetRenderSystem()->SetPipeline(nullptr);
        }
        _program = nullptr;
        _material.reset();
        _texture.Reset();
        _mesh.Reset();
    }

private:
    void Fail(std::string_view message) {
        if (!_result->SawError) {
            _result->SawError = true;
            _result->FirstError = string{message};
        }
        RequestClose();
    }

    void RequestClose() {
#if defined(_WIN32)
        WindowManager* windows = GetWindowManager();
        if (windows == nullptr) {
            return;
        }
        AppWindow* main = windows->GetMainWindow();
        if (main == nullptr || main->GetNativeWindow() == nullptr) {
            return;
        }
        auto handle = static_cast<HWND>(main->GetNativeWindow()->GetNativeHandler());
        if (handle != nullptr) {
            ::PostMessageW(handle, WM_CLOSE, 0, 0);
        }
#endif
    }

    ForwardPipelineRunResult* _result;
    StreamingAssetRef<StaticMesh> _mesh;
    StreamingAssetRef<TextureAsset> _texture;
    unique_ptr<Material> _material;
    ShaderProgram* _program{nullptr};
    Nullable<Actor*> _cameraActor{nullptr};
    Nullable<Actor*> _meshActor{nullptr};
    Nullable<Actor*> _dirLightActor{nullptr};
    Nullable<Actor*> _pointLightActor{nullptr};
    Nullable<StaticMeshComponent*> _meshComponent{nullptr};
};

void RunForwardPipeline(render::RenderBackend backend) {
    const std::filesystem::path projectRoot{RADRAY_PROJECT_DIR};
    ForwardPipelineRunResult result;
    ForwardPipelineTestApp app{&result};
    const ApplicationRuntimeDescriptor descriptor{
        .Backend = backend,
        .EnableValidation = false,
        .Multithreaded = false,
        .AppName = "test_forward_pipeline",
        .EngineName = "RadRay",
        .RenderCachePath = {},
        .AssetRoot = {},
        .ShaderSourceRoot = projectRoot / "shaderlib",
        .ShaderIncludePaths = {projectRoot / "shaderlib"},
        .WindowTitle = "test_forward_pipeline",
        .WindowWidth = 320,
        .WindowHeight = 240,
        .BackBufferCount = 3,
        .FlightDataCount = 2,
        .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
        .PresentMode = render::PresentMode::FIFO};
    ASSERT_EQ(app.Run(descriptor), 0);

    EXPECT_FALSE(result.SawError) << result.FirstError;
    EXPECT_TRUE(result.InitSucceeded);
    EXPECT_TRUE(result.MeshAssigned);
    EXPECT_GE(result.FramesRun, kFrameCount);
    // A PSO only exists if ValidateProgram accepted the program, the view and object
    // constants packed, the material set prepared and the draw loop reached
    // GetOrCreateGraphicsPipelineState. One key per (material, geometry, pass).
    EXPECT_EQ(result.PipelineStateCount, 1u);
    EXPECT_TRUE(result.MaterialSetResident);
}

}  // namespace

#if defined(RADRAY_ENABLE_D3D12)
TEST(RadRayRuntimeForwardPipeline, D3D12DrawsCollectedMeshThroughForwardPipeline) {
    RunForwardPipeline(render::RenderBackend::D3D12);
}
#endif

#if defined(RADRAY_ENABLE_VULKAN)
TEST(RadRayRuntimeForwardPipeline, VulkanDrawsCollectedMeshThroughForwardPipeline) {
    RunForwardPipeline(render::RenderBackend::Vulkan);
}
#endif

}  // namespace radray
