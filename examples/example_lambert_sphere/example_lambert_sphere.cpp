#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/render/rhi.h>
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/game_framework/actor.h>
#include <radray/runtime/game_framework/world.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/shader_jit.h>
#include <radray/runtime/window_manager.h>
#include <radray/shader/shader_artifact.h>
#include <radray/shader/shader_compiler_contract.h>
#include <radray/triangle_mesh.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif

#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace {

using namespace radray;

constexpr std::string_view kSourceName = "example_lambert_sphere.hlsl";
constexpr render::TextureFormat kColorFormat = render::TextureFormat::BGRA8_UNORM;
constexpr render::TextureFormat kDepthFormat = render::TextureFormat::D24_UNORM_S8_UINT;
constexpr float kLightPathPeriodSeconds = 6.0f;

struct LambertDirectionalLightCpu {
    Eigen::Vector4f Direction;
    Eigen::Vector4f Irradiance;
};

struct LambertFrameData {
    Eigen::Matrix4f ViewProj;
    Eigen::Matrix4f Model;
    Eigen::Vector4f Albedo;
};

static_assert(sizeof(Eigen::Vector4f) == 16);
static_assert(sizeof(Eigen::Matrix4f) == 64);
static_assert(offsetof(LambertDirectionalLightCpu, Irradiance) == 16);
static_assert(sizeof(LambertDirectionalLightCpu) == 32);
static_assert(sizeof(LambertFrameData) == 144);

std::optional<std::span<const radray::byte>> FindStageBytecode(
    const shader::ShaderArtifactView& artifact,
    shader::ShaderStage stage) {
    for (const shader::WireEntryRecord& entry : artifact.Entries()) {
        if (entry.Stage != static_cast<uint8_t>(stage)) {
            continue;
        }
        return artifact.Bytecode().subspan(entry.InterfaceOffset, entry.InterfaceSize);
    }
    return std::nullopt;
}

std::optional<render::VertexFormat> MakeVertexFormat(
    uint32_t componentType,
    uint32_t componentCount) {
    if (componentType != static_cast<uint32_t>(shader::ShaderVertexComponentType::Float)) {
        return std::nullopt;
    }
    switch (componentCount) {
        case 1:
            return render::VertexFormat::FLOAT32;
        case 2:
            return render::VertexFormat::FLOAT32X2;
        case 3:
            return render::VertexFormat::FLOAT32X3;
        case 4:
            return render::VertexFormat::FLOAT32X4;
        default:
            return std::nullopt;
    }
}

std::optional<std::filesystem::path> FindProjectRoot() {
    std::error_code error;
    const std::filesystem::path current = std::filesystem::current_path(error);
    if (error) {
        RADRAY_ERR_LOG("cannot determine the example working directory: {}", error.message());
        return std::nullopt;
    }
    return current;
}

#ifdef RADRAY_ENABLE_SHADER_JIT
struct ExampleOptions {
    render::RenderBackend Backend{render::RenderBackend::D3D12};
    bool Multithreaded{false};
    bool EnableValidation{false};
};

bool ParseOptions(int argc, char** argv, ExampleOptions* options) {
    if (options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i] != nullptr ? argv[i] : "";
        if (argument == "--vulkan") {
            options->Backend = render::RenderBackend::Vulkan;
        } else if (argument == "--d3d12") {
            options->Backend = render::RenderBackend::D3D12;
        } else if (argument == "--multithread") {
            options->Multithreaded = true;
        } else if (argument == "--valid-layer") {
            options->EnableValidation = true;
        } else {
            RADRAY_WARN_LOG("unknown example argument: {}", argument);
        }
    }
    return true;
}
#endif

class LambertPipeline;

class LambertPass final : public RenderPipelinePass {
public:
    explicit LambertPass(LambertPipeline* pipeline) noexcept;

    void Execute(RenderPipelineContext& ctx, const RenderCamera& camera) override;

private:
    LambertPipeline* _pipeline;
};

class LambertPipeline final : public RenderPipeline {
public:
    LambertPipeline(Application* app, Scene* scene, CameraComponent* camera) noexcept
        : _app(app),
          _scene(scene),
          _camera(camera),
          _pass(make_unique<LambertPass>(this)) {}

    bool Initialize();

protected:
    void OnBeginFrame(RenderPipelineContext& ctx) override {
        _phase += ctx.Frame.DeltaTime().count();
        if (_phase >= kLightPathPeriodSeconds) {
            _phase = std::fmod(_phase, kLightPathPeriodSeconds);
        }
    }

    void OnBuildCameraList(RenderPipelineContext& ctx, RenderCameraList& cameras) override {
        cameras.Clear();
        if (_scene == nullptr || _camera == nullptr || ctx.Targets.empty()) {
            return;
        }
        cameras.Add(_scene, _camera, &ctx.Targets.front().Target);
    }

    void OnAddRenderPasses(RenderPipelineContext& ctx, const RenderCamera& camera) override {
        (void)ctx;
        if (_ready && camera.ViewCamera != nullptr && camera.Target) {
            EnqueuePass(_pass.get());
        }
    }

private:
    friend class LambertPass;

    bool BuildPipeline(
        const shader::ShaderArtifactView& artifact,
        std::span<const radray::byte> vertexBytecode,
        std::span<const radray::byte> pixelBytecode,
        render::ShaderBlobCategory category);
    bool EnsureMesh(AppFrameContext& frame);
    bool EnsureDepth(uint32_t width, uint32_t height);
    bool EnsureFlightResources(AppFrameContext& frame, size_t flightIndex);
    void ExecuteLambertPass(RenderPipelineContext& ctx, const RenderCamera& camera);

    struct FlightResources {
        unique_ptr<DynamicCBufferArena> Arena;
        unique_ptr<render::ShaderParameterSet> Parameters;
    };

    Application* _app{nullptr};
    Scene* _scene{nullptr};
    CameraComponent* _camera{nullptr};
    unique_ptr<LambertPass> _pass;

    render::Device* _device{nullptr};
    render::RenderPassRegistry* _registry{nullptr};
    render::RenderPass* _renderPass{nullptr};
    unique_ptr<render::PipelineLayout> _layout;
    unique_ptr<render::Shader> _vertexShader;
    unique_ptr<render::Shader> _pixelShader;
    unique_ptr<render::GraphicsPipelineState> _pipelineState;
    render::BindingHandle _frameBinding;
    render::BindingHandle _lightBinding;
    vector<render::VertexAttribute> _vertexAttributes;
    vector<FlightResources> _flights;

    unique_ptr<render::Texture> _depthTexture;
    unique_ptr<render::TextureView> _depthView;
    uint32_t _depthWidth{0};
    uint32_t _depthHeight{0};
    render::TextureStates _depthState{render::TextureState::Undefined};

    std::optional<GpuMesh> _mesh;
    uint32_t _indexCount{0};
    float _phase{0.0f};
    bool _ready{false};
};

LambertPass::LambertPass(LambertPipeline* pipeline) noexcept
    : RenderPipelinePass(RenderPassEvent::BeforeRenderingOpaques),
      _pipeline(pipeline) {}

void LambertPass::Execute(RenderPipelineContext& ctx, const RenderCamera& camera) {
    if (_pipeline != nullptr) {
        _pipeline->ExecuteLambertPass(ctx, camera);
    }
}

bool LambertPipeline::Initialize() {
    if (_app == nullptr || _scene == nullptr || _camera == nullptr) {
        RADRAY_ERR_LOG("example_lambert_sphere: application scene or camera is missing");
        return false;
    }
    _device = _app->GetDevice();
    _registry = _app->GetRenderSystem() != nullptr
                    ? _app->GetRenderSystem()->GetRenderPassRegistry()
                    : nullptr;
    if (_device == nullptr || _registry == nullptr) {
        RADRAY_ERR_LOG("example_lambert_sphere: device or render pass registry is missing");
        return false;
    }

    const std::optional<std::filesystem::path> projectRoot = FindProjectRoot();
    if (!projectRoot.has_value()) {
        return false;
    }
    const std::filesystem::path sourcePath =
        projectRoot.value() / "examples" / "example_lambert_sphere" / "example_lambert_sphere.hlsl";
    const std::optional<vector<radray::byte>> source = ReadBinaryFile(sourcePath);
    if (!source.has_value()) {
        RADRAY_ERR_LOG("example_lambert_sphere: root shader source was not found: {}", sourcePath.string());
        return false;
    }

    const shader::ShaderTarget target = _device->GetBackend() == render::RenderBackend::Vulkan
                                            ? shader::ShaderTarget::SPIRV
                                            : shader::ShaderTarget::DXIL;
    ShaderJit jit{vector<std::filesystem::path>{projectRoot.value() / "shaderlib"}};
    if (!jit.IsAvailable()) {
        RADRAY_ERR_LOG("example_lambert_sphere: runtime shader JIT is unavailable");
        return false;
    }
    const std::span<const radray::byte> sourceSpan = source.value();
    const std::optional<shader::ContractHash> contract =
        jit.DiscoverContractHash(kSourceName, sourceSpan, target);
    if (!contract.has_value()) {
        RADRAY_ERR_LOG("example_lambert_sphere: shader contract discovery failed");
        return false;
    }
    const shader::CompileVariantRequest request{
        .SourceName = string{kSourceName},
        .RootSource = source.value(),
        .Defines = {},
        .Assignments = {},
        .Targets = static_cast<shader::ShaderTargetMask>(shader::ToTargetMask(target)),
        .ExpectedContract = contract.value()};
    const std::optional<ShaderJitArtifact> compiled = jit.Compile(request, target);
    if (!compiled.has_value()) {
        RADRAY_ERR_LOG("example_lambert_sphere: shader JIT compilation failed");
        return false;
    }

    const shader::ShaderArtifactDecodeOptions decodeOptions{
        .Target = target,
        .ExpectedGpuArtifact = compiled->ExpectedGpuArtifact};
    shader::ShaderArtifactDecodeError decodeError = shader::ShaderArtifactDecodeError::None;
    if (target == shader::ShaderTarget::DXIL) {
#if defined(RADRAY_ENABLE_D3D12)
        const std::optional<shader::DxilShaderArtifactView> artifact =
            shader::DecodeDxilShaderArtifact(compiled->Metadata, decodeOptions, &decodeError);
        if (!artifact.has_value()) {
            RADRAY_ERR_LOG("example_lambert_sphere: DXIL artifact decode failed: {}", static_cast<uint32_t>(decodeError));
            return false;
        }
        const std::optional<std::span<const radray::byte>> vertex =
            FindStageBytecode(artifact->Generic(), shader::ShaderStage::Vertex);
        const std::optional<std::span<const radray::byte>> pixel =
            FindStageBytecode(artifact->Generic(), shader::ShaderStage::Pixel);
        if (!vertex.has_value() || !pixel.has_value()) {
            RADRAY_ERR_LOG("example_lambert_sphere: DXIL artifact has no graphics stages");
            return false;
        }
        auto layout = static_cast<render::d3d12::DeviceD3D12*>(_device)->CreatePipelineLayout(*artifact);
        if (!layout.HasValue()) {
            RADRAY_ERR_LOG("example_lambert_sphere: D3D12 pipeline layout creation failed");
            return false;
        }
        _layout = layout.Release();
        _frameBinding = static_cast<render::d3d12::RootSigD3D12*>(_layout.get())->FindBinding("Frame");
        _lightBinding = static_cast<render::d3d12::RootSigD3D12*>(_layout.get())->FindBinding("Light");
        return BuildPipeline(
            artifact->Generic(),
            vertex.value(),
            pixel.value(),
            render::ShaderBlobCategory::DXIL);
#else
        RADRAY_ERR_LOG("example_lambert_sphere: D3D12 support is disabled");
        return false;
#endif
    }

#if defined(RADRAY_ENABLE_VULKAN)
    const std::optional<shader::SpirvShaderArtifactView> artifact =
        shader::DecodeSpirvShaderArtifact(compiled->Metadata, decodeOptions, &decodeError);
    if (!artifact.has_value()) {
        RADRAY_ERR_LOG("example_lambert_sphere: SPIR-V artifact decode failed: {}", static_cast<uint32_t>(decodeError));
        return false;
    }
    const std::optional<std::span<const radray::byte>> vertex =
        FindStageBytecode(artifact->Generic(), shader::ShaderStage::Vertex);
    const std::optional<std::span<const radray::byte>> pixel =
        FindStageBytecode(artifact->Generic(), shader::ShaderStage::Pixel);
    if (!vertex.has_value() || !pixel.has_value()) {
        RADRAY_ERR_LOG("example_lambert_sphere: SPIR-V artifact has no graphics stages");
        return false;
    }
    auto layout = static_cast<render::vulkan::DeviceVulkan*>(_device)->CreatePipelineLayout(*artifact);
    if (!layout.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: Vulkan pipeline layout creation failed");
        return false;
    }
    _layout = layout.Release();
    _frameBinding = static_cast<render::vulkan::PipelineLayoutVulkan*>(_layout.get())->FindBinding("Frame");
    _lightBinding = static_cast<render::vulkan::PipelineLayoutVulkan*>(_layout.get())->FindBinding("Light");
    return BuildPipeline(
        artifact->Generic(),
        vertex.value(),
        pixel.value(),
        render::ShaderBlobCategory::SPIRV);
#else
    RADRAY_ERR_LOG("example_lambert_sphere: Vulkan support is disabled");
    return false;
#endif
}

bool LambertPipeline::BuildPipeline(
    const shader::ShaderArtifactView& artifact,
    std::span<const radray::byte> vertexBytecode,
    std::span<const radray::byte> pixelBytecode,
    render::ShaderBlobCategory category) {
    if (!_frameBinding.IsValid() || !_lightBinding.IsValid()) {
        RADRAY_ERR_LOG("example_lambert_sphere: frame or light binding is missing from shader layout");
        return false;
    }

    _vertexAttributes.clear();
    for (const shader::WireVertexInputRecord& input : artifact.VertexInputs()) {
        const std::optional<std::string_view> semantic = artifact.GetName(input.Semantic);
        const std::optional<render::VertexFormat> format =
            MakeVertexFormat(input.ComponentType, input.ComponentCount);
        if (!semantic.has_value() || !format.has_value()) {
            RADRAY_ERR_LOG("example_lambert_sphere: unsupported shader vertex input");
            return false;
        }

        uint32_t offset = 0;
        if (semantic.value() == VertexSemantics::POSITION && input.ComponentCount == 3) {
            offset = 0;
        } else if (semantic.value() == VertexSemantics::NORMAL && input.ComponentCount == 3) {
            offset = sizeof(Eigen::Vector3f);
        } else {
            RADRAY_ERR_LOG("example_lambert_sphere: unexpected vertex semantic: {}", semantic.value());
            return false;
        }
        _vertexAttributes.emplace_back(render::VertexAttribute{
            .BufferBinding = 0,
            .Offset = offset,
            .Semantic = semantic.value(),
            .SemanticIndex = input.SemanticIndex,
            .Format = format.value(),
            .Location = input.Location});
    }
    if (_vertexAttributes.size() != 2) {
        RADRAY_ERR_LOG("example_lambert_sphere: expected position and normal shader inputs");
        return false;
    }

    const render::RenderPassColorAttachmentDescriptor colorAttachment{
        .Format = kColorFormat,
        .SampleCount = 1,
        .Load = render::LoadAction::Clear,
        .Store = render::StoreAction::Store};
    const render::RenderPassDepthStencilAttachmentDescriptor depthAttachment{
        .Format = kDepthFormat,
        .SampleCount = 1,
        .DepthLoad = render::LoadAction::Clear,
        .DepthStore = render::StoreAction::Store,
        .StencilLoad = render::LoadAction::Clear,
        .StencilStore = render::StoreAction::Store};
    const render::RenderPassDescriptor renderPassDescriptor{
        .ColorAttachments = std::span{&colorAttachment, 1},
        .DepthStencilAttachment = depthAttachment};
    const Nullable<render::RenderPass*> renderPass =
        _registry->GetOrCreateRenderPass(renderPassDescriptor);
    if (!renderPass.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: render pass creation failed");
        return false;
    }
    _renderPass = renderPass.Get();

    Nullable<unique_ptr<render::Shader>> vertexShader = _device->CreateShader(render::ShaderDescriptor{
        .Source = vertexBytecode,
        .Category = category,
        .Stages = render::ShaderStage::Vertex});
    Nullable<unique_ptr<render::Shader>> pixelShader = _device->CreateShader(render::ShaderDescriptor{
        .Source = pixelBytecode,
        .Category = category,
        .Stages = render::ShaderStage::Pixel});
    if (!vertexShader.HasValue() || !pixelShader.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: native shader creation failed");
        return false;
    }
    _vertexShader = vertexShader.Release();
    _pixelShader = pixelShader.Release();

    const render::VertexBufferLayout vertexBuffer{
        .Binding = 0,
        .ArrayStride = sizeof(Eigen::Vector3f) * 2,
        .StepMode = render::VertexStepMode::Vertex};
    const render::VertexInputState vertexInput{
        .Buffers = std::span{&vertexBuffer, 1},
        .Attributes = _vertexAttributes};
    const render::ColorTargetState colorTarget = render::ColorTargetState::Default(kColorFormat);
    render::DepthStencilState depthState = render::DepthStencilState::Default();
    depthState.Format = kDepthFormat;
    const render::PrimitiveState primitive = render::PrimitiveState::Default();
    Nullable<unique_ptr<render::GraphicsPipelineState>> pipelineState =
        _device->CreateGraphicsPipelineState(render::GraphicsPipelineStateDescriptor{
            .PipelineLayout = _layout.get(),
            .VS = render::ShaderEntry{_vertexShader.get(), "VSMain"},
            .PS = render::ShaderEntry{_pixelShader.get(), "PSMain"},
            .VertexInput = vertexInput,
            .Primitive = primitive,
            .DepthStencil = depthState,
            .MultiSample = render::MultiSampleState::Default(),
            .ColorTargets = std::span{&colorTarget, 1},
            .CompatibleRenderPass = _renderPass});
    if (!pipelineState.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: graphics pipeline creation failed");
        return false;
    }
    _pipelineState = pipelineState.Release();
    _pipelineState->SetDebugName("example_lambert_sphere Lambert");
    _layout->SetDebugName("example_lambert_sphere Layout");
    _flights.resize(_app->GetGpuSystem()->GetFlightDataCount());
    _ready = true;
    return true;
}

bool LambertPipeline::EnsureMesh(AppFrameContext& frame) {
    if (_mesh.has_value()) {
        return true;
    }

    TriangleMesh sphere;
    sphere.InitAsUVSphere(1.0f, 64);
    sphere.UV0.clear();
    sphere.Tangents.clear();
    sphere.Color0.clear();

    MeshResource meshResource;
    sphere.ToSimpleMeshResource(&meshResource);
    if (meshResource.Primitives.size() != 1 || meshResource.Bins.empty()) {
        RADRAY_ERR_LOG("example_lambert_sphere: CPU sphere mesh conversion failed");
        return false;
    }
    const uint32_t indexCount = meshResource.Primitives.front().IndexBuffer.IndexCount;
    std::optional<GpuMesh> uploaded =
        frame.GetUploader().UploadMeshResource(frame.GetCommandBuffer(), meshResource);
    if (!uploaded.has_value() || uploaded->Draws.empty()) {
        RADRAY_ERR_LOG("example_lambert_sphere: sphere mesh upload recording failed");
        return false;
    }
    _indexCount = indexCount;
    _mesh = std::move(uploaded.value());
    return true;
}

bool LambertPipeline::EnsureDepth(uint32_t width, uint32_t height) {
    if (_depthTexture != nullptr && _depthView != nullptr &&
        _depthWidth == width && _depthHeight == height) {
        return true;
    }

    if (_registry != nullptr && _depthView != nullptr) {
        _registry->RemoveFramebuffersUsing(_depthView.get());
    }
    _depthView.reset();
    _depthTexture.reset();
    _depthWidth = width;
    _depthHeight = height;
    _depthState = render::TextureState::Undefined;

    Nullable<unique_ptr<render::Texture>> depthTexture = _device->CreateTexture(render::TextureDescriptor{
        .Dim = render::TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = kDepthFormat,
        .Memory = render::MemoryType::Device,
        .Usage = render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite,
        .Hints = render::ResourceHint::None});
    if (!depthTexture.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: D24S8 depth texture creation failed for {}x{}", width, height);
        return false;
    }
    _depthTexture = depthTexture.Release();
    Nullable<unique_ptr<render::TextureView>> depthView = _device->CreateTextureView(render::TextureViewDescriptor{
        .Target = _depthTexture.get(),
        .Dim = render::TextureDimension::Dim2D,
        .Format = kDepthFormat,
        .Range = render::SubresourceRange{0, 1, 0, 1},
        .Usage = render::TextureViewUsage::DepthWrite});
    if (!depthView.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: D24S8 depth view creation failed");
        _depthTexture.reset();
        return false;
    }
    _depthView = depthView.Release();
    return true;
}

bool LambertPipeline::EnsureFlightResources(AppFrameContext& frame, size_t flightIndex) {
    if (_device == nullptr || _layout == nullptr || flightIndex >= _flights.size()) {
        return false;
    }
    FlightResources& resources = _flights[flightIndex];
    if (resources.Arena == nullptr) {
        DynamicCBufferArena::Descriptor descriptor;
        descriptor.Alignment = std::max<uint64_t>(_device->GetDetail().CBufferAlignment, 1);
        descriptor.NamePrefix = "example_lambert_sphere_Frame";
        resources.Arena = make_unique<DynamicCBufferArena>(
            _device,
            &frame.GetHostWrites(),
            descriptor);
    }
    if (resources.Parameters == nullptr) {
        Nullable<unique_ptr<render::ShaderParameterSet>> parameters =
            _device->CreateShaderParameterSet(render::ShaderParameterSetDescriptor{
                .Layout = _layout.get(),
                .GroupIndex = 0});
        if (!parameters.HasValue()) {
            RADRAY_ERR_LOG("example_lambert_sphere: frame parameter set creation failed");
            return false;
        }
        resources.Parameters = parameters.Release();
    }
    resources.Arena->Reset();
    return resources.Arena->IsValid();
}

void LambertPipeline::ExecuteLambertPass(RenderPipelineContext& ctx, const RenderCamera& camera) {
    AppFrameTarget* target = camera.Target.Get();
    if (!_ready || target == nullptr || target->BackBuffer == nullptr ||
        target->BackBufferView == nullptr || camera.ViewCamera == nullptr) {
        return;
    }

    const render::TextureDescriptor backBuffer = target->BackBuffer->GetDesc();
    if (backBuffer.Format != kColorFormat || backBuffer.SampleCount != 1 ||
        backBuffer.Width == 0 || backBuffer.Height == 0 ||
        !EnsureDepth(backBuffer.Width, backBuffer.Height) ||
        !EnsureMesh(ctx.Frame) ||
        !EnsureFlightResources(ctx.Frame, ctx.Frame.FlightIndex())) {
        return;
    }

    FlightResources& resources = _flights[ctx.Frame.FlightIndex()];
    LambertFrameData frameData{
        .ViewProj = camera.ViewCamera->ComputeViewProjMatrix(
            static_cast<float>(backBuffer.Width) / static_cast<float>(backBuffer.Height)),
        .Model = Eigen::Matrix4f::Identity(),
        .Albedo = Eigen::Vector4f{0.72f, 0.36f, 0.12f, 1.0f}};
    LambertDirectionalLightCpu lightData{
        .Direction = Eigen::Vector4f{
            0.8f * std::cos(std::numbers::pi_v<float> * 2.0f * _phase / kLightPathPeriodSeconds),
            -0.6f,
            0.8f * std::sin(std::numbers::pi_v<float> * 2.0f * _phase / kLightPathPeriodSeconds),
            0.0f},
        .Irradiance = Eigen::Vector4f{3.0f, 3.0f, 3.0f, 0.0f}};
    DynamicCBufferArena::Reservation frameReservation = resources.Arena->Reserve(sizeof(frameData));
    DynamicCBufferArena::Reservation lightReservation = resources.Arena->Reserve(sizeof(lightData));
    if (!frameReservation.IsValid() || !lightReservation.IsValid()) {
        RADRAY_ERR_LOG("example_lambert_sphere: frame constant buffer reservation failed");
        return;
    }
    std::memcpy(frameReservation.Data(), &frameData, sizeof(frameData));
    std::memcpy(lightReservation.Data(), &lightData, sizeof(lightData));
    const DynamicCBufferArena::Allocation frameAllocation = frameReservation.Commit(sizeof(frameData));
    const DynamicCBufferArena::Allocation lightAllocation = lightReservation.Commit(sizeof(lightData));
    if (!frameAllocation.IsValid() || !lightAllocation.IsValid() || !resources.Parameters->Set(
            _frameBinding,
            0,
            render::ShaderBufferBinding{
                .Target = frameAllocation.Target,
                .Range = render::BufferRange{frameAllocation.Offset, frameAllocation.Size},
                .StructureByteStride = 0}) ||
        !resources.Parameters->Set(
            _lightBinding,
            0,
            render::ShaderBufferBinding{
                .Target = lightAllocation.Target,
                .Range = render::BufferRange{lightAllocation.Offset, lightAllocation.Size},
                .StructureByteStride = 0}) ||
        !resources.Parameters->FlushWrites()) {
        RADRAY_ERR_LOG("example_lambert_sphere: frame parameter upload failed");
        return;
    }

    if (_depthState != render::TextureState::DepthWrite) {
        const render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
            .Target = _depthTexture.get(),
            .Before = _depthState,
            .After = render::TextureState::DepthWrite};
        ctx.Frame.GetCommandBuffer()->ResourceBarrier(std::span{&barrier, 1});
        _depthState = render::TextureState::DepthWrite;
    }

    render::TextureView* colorView = target->BackBufferView;
    const Nullable<render::Framebuffer*> framebuffer = _registry->GetOrCreateFramebuffer(
        render::FramebufferDescriptor{
            .Pass = _renderPass,
            .ColorAttachments = std::span<render::TextureView* const>{&colorView, 1},
            .DepthStencilAttachment = _depthView.get(),
            .Width = backBuffer.Width,
            .Height = backBuffer.Height,
            .Layers = 1});
    if (!framebuffer.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: framebuffer creation failed");
        return;
    }

    const render::ColorClearValue colorClear{{0.025f, 0.030f, 0.040f, 1.0f}};
    const render::DepthStencilClearValue depthClear{1.0f, 0};
    Nullable<unique_ptr<render::GraphicsCommandEncoder>> encoder =
        ctx.Frame.GetCommandBuffer()->BeginRenderPass(render::RenderPassBeginDescriptor{
            .Pass = _renderPass,
            .Target = framebuffer.Get(),
            .ColorClearValues = std::span{&colorClear, 1},
            .DepthStencilClearValue = depthClear,
            .Name = "example_lambert_sphere"});
    if (!encoder.HasValue()) {
        RADRAY_ERR_LOG("example_lambert_sphere: begin render pass failed");
        return;
    }
    unique_ptr<render::GraphicsCommandEncoder> graphics = encoder.Release();
    const float width = static_cast<float>(backBuffer.Width);
    const float height = static_cast<float>(backBuffer.Height);
    if (_device->GetBackend() == render::RenderBackend::Vulkan) {
        graphics->SetViewport(Viewport{0.0f, height, width, -height, 0.0f, 1.0f});
    } else {
        graphics->SetViewport(Viewport{0.0f, 0.0f, width, height, 0.0f, 1.0f});
    }
    graphics->SetScissor(Rect{0, 0, backBuffer.Width, backBuffer.Height});
    graphics->BindGraphicsPipelineState(_pipelineState.get());
    graphics->BindShaderParameterSet(0, resources.Parameters.get());
    const GpuMesh::DrawData& draw = _mesh->Draws.front();
    const render::VertexBufferBinding vertexBinding{.Binding = 0, .View = draw.Vbv};
    graphics->BindVertexBuffers(std::span{&vertexBinding, 1});
    graphics->BindIndexBuffer(draw.Ibv);
    graphics->DrawIndexed(_indexCount, 1, 0, 0, 0);
    ctx.Frame.GetCommandBuffer()->EndRenderPass(std::move(graphics));

    for (RenderPipelineTarget& pipelineTarget : ctx.Targets) {
        if (&pipelineTarget.Target == target) {
            pipelineTarget.ContentDrawn = true;
            break;
        }
    }
}

class LambertApplication final : public Application {
public:
    LambertApplication() noexcept = default;

protected:
    void OnInit() override {
        if (GetWorld() == nullptr || GetRenderSystem() == nullptr) {
            RADRAY_ERR_LOG("example_lambert_sphere: runtime services are incomplete");
            return;
        }
        Actor* cameraActor = GetWorld()->SpawnActor<Actor>();
        if (cameraActor == nullptr) {
            RADRAY_ERR_LOG("example_lambert_sphere: camera actor creation failed");
            return;
        }
        CameraComponent* camera = cameraActor->AddComponent<CameraComponent>();
        if (camera == nullptr) {
            RADRAY_ERR_LOG("example_lambert_sphere: camera component creation failed");
            return;
        }
        cameraActor->SetRootComponent(camera);
        camera->SetWorldLocation(Eigen::Vector3f{0.0f, 0.0f, -3.0f});
        camera->SetPerspective(Radian(60.0f), 0.1f, 100.0f);

        auto pipeline = make_unique<LambertPipeline>(this, GetWorld()->GetScene(), camera);
        if (!pipeline->Initialize()) {
            RADRAY_ERR_LOG("example_lambert_sphere: pipeline initialization failed");
        }
        GetRenderSystem()->SetPipeline(std::move(pipeline));
    }

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

    const ApplicationRuntimeDescriptor descriptor{
        .Backend = options.Backend,
        .EnableValidation = options.EnableValidation,
        .Multithreaded = options.Multithreaded,
        .AppName = "example_lambert_sphere",
        .EngineName = "RadRay",
        .RenderCachePath = {},
        .WindowTitle = "example_lambert_sphere",
        .WindowWidth = 1280,
        .WindowHeight = 720,
        .BackBufferCount = 3,
        .FlightDataCount = 2,
        .BackBufferFormat = render::TextureFormat::BGRA8_UNORM,
        .PresentMode = render::PresentMode::FIFO};
    LambertApplication application;
    return application.Run(descriptor);
#endif
}
