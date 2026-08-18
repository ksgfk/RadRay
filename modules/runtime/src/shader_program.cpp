#include <radray/runtime/shader_program.h>

#include <bit>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <radray/logger.h>

namespace radray {
namespace {

template <typename T>
void AddEnum(HashCode& hash, T value) noexcept {
    hash.Add(static_cast<std::underlying_type_t<T>>(value));
}

void AddFloat(HashCode& hash, float value) noexcept {
    hash.Add(std::bit_cast<uint32_t>(value));
}

void AddStencilFace(HashCode& hash, const render::StencilFaceState& value) noexcept {
    AddEnum(hash, value.Compare);
    AddEnum(hash, value.FailOp);
    AddEnum(hash, value.DepthFailOp);
    AddEnum(hash, value.PassOp);
}

void AddStencil(HashCode& hash, const render::StencilState& value) noexcept {
    AddStencilFace(hash, value.Front);
    AddStencilFace(hash, value.Back);
    hash.Add(value.ReadMask);
    hash.Add(value.WriteMask);
}

void AddBlendComponent(HashCode& hash, const render::BlendComponent& value) noexcept {
    AddEnum(hash, value.Src);
    AddEnum(hash, value.Dst);
    AddEnum(hash, value.Op);
}

void AddBlend(HashCode& hash, const render::BlendState& value) noexcept {
    AddBlendComponent(hash, value.Color);
    AddBlendComponent(hash, value.Alpha);
}

std::optional<std::pair<string, std::span<const byte>>> FindStage(
    const shader::ShaderArtifactView& artifact,
    shader::ShaderStage stage) noexcept {
    for (const shader::WireEntryRecord& entry : artifact.Entries()) {
        if (entry.Stage != static_cast<uint8_t>(stage)) {
            continue;
        }
        const std::optional<std::string_view> name = artifact.GetName(entry.Name);
        const std::optional<std::span<const byte>> bytecode = artifact.FindStageBytecode(stage);
        if (!name.has_value() || !bytecode.has_value()) {
            return std::nullopt;
        }
        return std::pair<string, std::span<const byte>>{string{name.value()}, bytecode.value()};
    }
    return std::nullopt;
}

Nullable<unique_ptr<render::Shader>> CreateStageShader(
    render::Device* device,
    render::ShaderBlobCategory category,
    render::ShaderStage stage,
    std::span<const byte> bytecode) noexcept {
    return device->CreateShader(render::ShaderDescriptor{
        .Source = bytecode,
        .Category = category,
        .Stages = stage});
}

}  // namespace

GraphicsPassState::GraphicsPassState(
    vector<render::TextureFormat> colorFormats,
    std::optional<render::TextureFormat> depthStencilFormat,
    uint32_t sampleCount,
    render::RenderPass* compatibleRenderPass) noexcept
    : ColorFormats(std::move(colorFormats)),
      DepthStencilFormat(depthStencilFormat),
      SampleCount(sampleCount),
      CompatibleRenderPass(compatibleRenderPass) {}

bool GraphicsPassState::IsValid() const noexcept {
    if (CompatibleRenderPass == nullptr || SampleCount == 0 ||
        (ColorFormats.empty() && !DepthStencilFormat.has_value())) {
        return false;
    }
    for (const render::TextureFormat format : ColorFormats) {
        if (format == render::TextureFormat::UNKNOWN) {
            return false;
        }
    }
    return !DepthStencilFormat.has_value() ||
           DepthStencilFormat.value() != render::TextureFormat::UNKNOWN;
}

Nullable<unique_ptr<ShaderProgram>> ShaderProgram::Create(
    render::Device* device,
    render::BackendShaderArtifact artifact,
    const render::ShaderLayoutPolicy& layoutPolicy) noexcept {
    if (device == nullptr || artifact.Layout == nullptr) {
        RADRAY_ERR_LOG("ShaderProgram::Create requires a device and pipeline layout");
        return nullptr;
    }
    std::optional<ShaderParameterLayout> parameterLayout =
        ShaderParameterLayout::Create(artifact);
    if (!parameterLayout.has_value()) {
        RADRAY_ERR_LOG("ShaderProgram::Create could not build the parameter type-tree layout");
        return nullptr;
    }

    const shader::ShaderArtifactView& generic = artifact.Generic();
    const auto vertex = FindStage(generic, shader::ShaderStage::Vertex);
    const auto pixel = FindStage(generic, shader::ShaderStage::Pixel);
    const auto compute = FindStage(generic, shader::ShaderStage::Compute);
    if ((!vertex.has_value() && !compute.has_value()) ||
        (vertex.has_value() && compute.has_value())) {
        RADRAY_ERR_LOG("ShaderProgram::Create found an invalid graphics/compute stage combination");
        return nullptr;
    }

    unique_ptr<render::Shader> vertexShader;
    unique_ptr<render::Shader> pixelShader;
    unique_ptr<render::Shader> computeShader;
    if (vertex.has_value()) {
        Nullable<unique_ptr<render::Shader>> result = CreateStageShader(
            device, artifact.Category, render::ShaderStage::Vertex, vertex->second);
        if (!result.HasValue()) {
            RADRAY_ERR_LOG("ShaderProgram::Create failed to create the vertex shader");
            return nullptr;
        }
        vertexShader = result.Release();
    }
    if (pixel.has_value()) {
        Nullable<unique_ptr<render::Shader>> result = CreateStageShader(
            device, artifact.Category, render::ShaderStage::Pixel, pixel->second);
        if (!result.HasValue()) {
            RADRAY_ERR_LOG("ShaderProgram::Create failed to create the pixel shader");
            return nullptr;
        }
        pixelShader = result.Release();
    }
    if (compute.has_value()) {
        Nullable<unique_ptr<render::Shader>> result = CreateStageShader(
            device, artifact.Category, render::ShaderStage::Compute, compute->second);
        if (!result.HasValue()) {
            RADRAY_ERR_LOG("ShaderProgram::Create failed to create the compute shader");
            return nullptr;
        }
        computeShader = result.Release();
    }

    return unique_ptr<ShaderProgram>{new ShaderProgram(
        device,
        std::move(artifact),
        std::move(parameterLayout.value()),
        std::move(vertexShader),
        vertex.has_value() ? std::move(vertex->first) : string{},
        std::move(pixelShader),
        pixel.has_value() ? std::move(pixel->first) : string{},
        std::move(computeShader),
        compute.has_value() ? std::move(compute->first) : string{},
        vector<uint32_t>{
            layoutPolicy.DynamicBufferGroups.begin(),
            layoutPolicy.DynamicBufferGroups.end()})};
}

ShaderProgram::ShaderProgram(
    render::Device* device,
    render::BackendShaderArtifact artifact,
    ShaderParameterLayout parameterLayout,
    unique_ptr<render::Shader> vertexShader,
    string vertexEntry,
    unique_ptr<render::Shader> pixelShader,
    string pixelEntry,
    unique_ptr<render::Shader> computeShader,
    string computeEntry,
    vector<uint32_t> dynamicBufferGroups) noexcept
    : _device(device),
      _artifact(std::move(artifact)),
      _vertexShader(std::move(vertexShader)),
      _vertexEntry(std::move(vertexEntry)),
      _pixelShader(std::move(pixelShader)),
      _pixelEntry(std::move(pixelEntry)),
      _computeShader(std::move(computeShader)),
      _computeEntry(std::move(computeEntry)),
      _parameterLayout(std::move(parameterLayout)),
      _dynamicBufferGroups(std::move(dynamicBufferGroups)) {}

ShaderProgram::~ShaderProgram() noexcept = default;

bool ShaderProgram::IsBufferGroupDynamic(uint32_t group) const noexcept {
    return std::find(
               _dynamicBufferGroups.begin(),
               _dynamicBufferGroups.end(),
               group) != _dynamicBufferGroups.end();
}

namespace {

size_t HashPsoKeyParts(
    const MaterialPipelineState& materialState,
    const PrimitiveVertexLayout& vertexLayout,
    PrimitiveTopology topology,
    const GraphicsPassState& passState) noexcept;

}  // namespace

size_t ShaderProgram::PsoKeyHash::operator()(const PsoKey& value) const noexcept {
    return HashPsoKeyParts(
        value.MaterialState,
        value.VertexLayout,
        value.Topology,
        value.PassState);
}

size_t ShaderProgram::PsoKeyHash::operator()(const PsoKeyRef& value) const noexcept {
    return HashPsoKeyParts(
        *value.MaterialState,
        *value.VertexLayout,
        value.Topology,
        *value.PassState);
}

bool ShaderProgram::PsoKeyEqual::operator()(
    const PsoKey& lhs, const PsoKey& rhs) const noexcept {
    return lhs == rhs;
}

bool ShaderProgram::PsoKeyEqual::operator()(
    const PsoKeyRef& lhs, const PsoKey& rhs) const noexcept {
    return lhs.Topology == rhs.Topology &&
           *lhs.MaterialState == rhs.MaterialState &&
           *lhs.VertexLayout == rhs.VertexLayout &&
           *lhs.PassState == rhs.PassState;
}

bool ShaderProgram::PsoKeyEqual::operator()(
    const PsoKey& lhs, const PsoKeyRef& rhs) const noexcept {
    return operator()(rhs, lhs);
}

namespace {

size_t HashPsoKeyParts(
    const MaterialPipelineState& materialState,
    const PrimitiveVertexLayout& vertexLayout,
    PrimitiveTopology topology,
    const GraphicsPassState& passState) noexcept {
    HashCode hash;
    AddEnum(hash, materialState.Primitive.FaceClockwise);
    AddEnum(hash, materialState.Primitive.Cull);
    AddEnum(hash, materialState.Primitive.Poly);
    hash.Add(materialState.Primitive.UnclippedDepth);
    hash.Add(materialState.Primitive.Conservative);

    AddEnum(hash, materialState.DepthStencil.DepthCompare);
    hash.Add(materialState.DepthStencil.DepthBias.Constant);
    AddFloat(hash, materialState.DepthStencil.DepthBias.SlopScale);
    AddFloat(hash, materialState.DepthStencil.DepthBias.Clamp);
    hash.Add(materialState.DepthStencil.Stencil.has_value());
    if (materialState.DepthStencil.Stencil.has_value()) {
        AddStencil(hash, materialState.DepthStencil.Stencil.value());
    }
    hash.Add(materialState.DepthStencil.DepthTestEnable);
    hash.Add(materialState.DepthStencil.DepthWriteEnable);
    hash.Add(materialState.Blend.has_value());
    if (materialState.Blend.has_value()) {
        AddBlend(hash, materialState.Blend.value());
    }
    hash.Add(materialState.WriteMask.value());

    hash.Add(vertexLayout.Buffers.size());
    for (const render::VertexBufferLayout& buffer : vertexLayout.Buffers) {
        hash.Add(buffer.Binding);
        hash.Add(buffer.ArrayStride);
        AddEnum(hash, buffer.StepMode);
    }
    hash.Add(vertexLayout.Attributes.size());
    for (const PrimitiveVertexAttribute& attribute : vertexLayout.Attributes) {
        hash.Add(attribute.Semantic);
        hash.Add(attribute.SemanticIndex);
        hash.Add(attribute.BufferBinding);
        hash.Add(attribute.Offset);
        AddEnum(hash, attribute.Format);
    }
    AddEnum(hash, topology);

    hash.Add(passState.ColorFormats.size());
    for (const render::TextureFormat format : passState.ColorFormats) {
        AddEnum(hash, format);
    }
    hash.Add(passState.DepthStencilFormat.has_value());
    if (passState.DepthStencilFormat.has_value()) {
        AddEnum(hash, passState.DepthStencilFormat.value());
    }
    hash.Add(passState.SampleCount);
    hash.Add(reinterpret_cast<uintptr_t>(passState.CompatibleRenderPass));
    return hash.ToHashCode();
}

}  // namespace

Nullable<render::GraphicsPipelineState*> ShaderProgram::GetOrCreateGraphicsPipelineState(
    const MaterialPipelineState& materialState,
    const PrimitiveVertexLayout& vertexLayout,
    PrimitiveTopology topology,
    const GraphicsPassState& passState) noexcept {
    if (_device == nullptr || _artifact.Layout == nullptr || _vertexShader == nullptr ||
        !passState.IsValid()) {
        return nullptr;
    }

    const PsoKeyRef lookup{
        .MaterialState = &materialState,
        .VertexLayout = &vertexLayout,
        .Topology = topology,
        .PassState = &passState};
    const auto existing = _graphicsPipelineStates.find(lookup);
    if (existing != _graphicsPipelineStates.end()) {
        return existing->second.get();
    }

    std::optional<ResolvedPrimitiveVertexLayout> resolved =
        ResolvePrimitiveVertexLayout(vertexLayout, _artifact.Generic());
    if (!resolved.has_value()) {
        return nullptr;
    }

    render::PrimitiveState primitive{
        .Topology = topology,
        .FaceClockwise = materialState.Primitive.FaceClockwise,
        .Cull = materialState.Primitive.Cull,
        .Poly = materialState.Primitive.Poly,
        .StripIndexFormat = std::nullopt,
        .UnclippedDepth = materialState.Primitive.UnclippedDepth,
        .Conservative = materialState.Primitive.Conservative};
    std::optional<render::DepthStencilState> depthStencil;
    if (passState.DepthStencilFormat.has_value()) {
        depthStencil = render::DepthStencilState{
            .Format = passState.DepthStencilFormat.value(),
            .DepthCompare = materialState.DepthStencil.DepthCompare,
            .DepthBias = materialState.DepthStencil.DepthBias,
            .Stencil = materialState.DepthStencil.Stencil,
            .DepthTestEnable = materialState.DepthStencil.DepthTestEnable,
            .DepthWriteEnable = materialState.DepthStencil.DepthWriteEnable};
    }
    vector<render::ColorTargetState> colorTargets;
    colorTargets.reserve(passState.ColorFormats.size());
    for (const render::TextureFormat format : passState.ColorFormats) {
        colorTargets.push_back(render::ColorTargetState{
            .Format = format,
            .Blend = materialState.Blend,
            .WriteMask = materialState.WriteMask});
    }
    render::MultiSampleState multiSample = render::MultiSampleState::Default();
    multiSample.Count = passState.SampleCount;

    const render::VertexInputState vertexInput = resolved->GetState();
    Nullable<unique_ptr<render::GraphicsPipelineState>> result =
        _device->CreateGraphicsPipelineState(render::GraphicsPipelineStateDescriptor{
            .PipelineLayout = _artifact.Layout.get(),
            .VS = render::ShaderEntry{_vertexShader.get(), _vertexEntry},
            .PS = _pixelShader != nullptr
                      ? std::optional<render::ShaderEntry>{
                            render::ShaderEntry{_pixelShader.get(), _pixelEntry}}
                      : std::nullopt,
            .VertexInput = vertexInput,
            .Primitive = primitive,
            .DepthStencil = depthStencil,
            .MultiSample = multiSample,
            .ColorTargets = colorTargets,
            .CompatibleRenderPass = passState.CompatibleRenderPass});
    if (!result.HasValue()) {
        return nullptr;
    }
    render::GraphicsPipelineState* output = result.Get();
    _graphicsPipelineStates.emplace(
        PsoKey{
            .MaterialState = materialState,
            .VertexLayout = vertexLayout,
            .Topology = topology,
            .PassState = passState},
        result.Release());
    return output;
}

}  // namespace radray
