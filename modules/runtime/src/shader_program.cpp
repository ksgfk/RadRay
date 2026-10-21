#include <radray/runtime/shader_program.h>

#include <cstdint>
#include <utility>

#include <radray/logger.h>

namespace radray {
namespace {

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

Nullable<unique_ptr<ShaderProgram>> ShaderProgram::Create(
    render::Device* device,
    render::BackendShaderArtifact artifact) noexcept {
    if (device == nullptr || artifact.Layout == nullptr) {
        RADRAY_ERR_LOG("ShaderProgram::Create requires a device and pipeline layout");
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

    return make_unique<ShaderProgram>(
        device,
        std::move(artifact),
        std::move(vertexShader),
        vertex.has_value() ? std::move(vertex->first) : string{},
        std::move(pixelShader),
        pixel.has_value() ? std::move(pixel->first) : string{},
        std::move(computeShader),
        compute.has_value() ? std::move(compute->first) : string{});
}

ShaderProgram::ShaderProgram(
    render::Device* device,
    render::BackendShaderArtifact artifact,
    unique_ptr<render::Shader> vertexShader,
    string vertexEntry,
    unique_ptr<render::Shader> pixelShader,
    string pixelEntry,
    unique_ptr<render::Shader> computeShader,
    string computeEntry) noexcept
    : _device(device),
      _artifact(std::move(artifact)),
      _vertexShader(std::move(vertexShader)),
      _vertexEntry(std::move(vertexEntry)),
      _pixelShader(std::move(pixelShader)),
      _pixelEntry(std::move(pixelEntry)),
      _computeShader(std::move(computeShader)),
      _computeEntry(std::move(computeEntry)) {}

ShaderProgram::~ShaderProgram() noexcept = default;

std::optional<render::ShaderEntry> ShaderProgram::GetStage(shader::ShaderStage stage) const noexcept {
    switch (stage) {
        case shader::ShaderStage::Vertex:
            if (_vertexShader) return render::ShaderEntry{_vertexShader.get(), _vertexEntry};
            break;
        case shader::ShaderStage::Pixel:
            if (_pixelShader) return render::ShaderEntry{_pixelShader.get(), _pixelEntry};
            break;
        case shader::ShaderStage::Compute:
            if (_computeShader) return render::ShaderEntry{_computeShader.get(), _computeEntry};
            break;
        default:
            break;
    }
    return std::nullopt;
}

bool ShaderProgram::IsBufferDynamic(std::string_view declarationName) const noexcept {
    return _artifact.IsBindingDynamic(declarationName);
}

}  // namespace radray
