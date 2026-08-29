#include <radray/render/backend/pipeline_layout_types.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <optional>

namespace radray::render {
namespace {

bool GetVertexFormatShape(
    VertexFormat format,
    uint32_t& componentType,
    uint32_t& componentCount) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::UINT16X2:
        case VertexFormat::UINT32X2:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 2;
            return true;
        case VertexFormat::UINT32X3:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 3;
            return true;
        case VertexFormat::UINT8X4:
        case VertexFormat::UINT16X4:
        case VertexFormat::UINT32X4:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 4;
            return true;
        case VertexFormat::UINT32:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::UnsignedInteger);
            componentCount = 1;
            return true;
        case VertexFormat::SINT8X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::SINT32X2:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 2;
            return true;
        case VertexFormat::SINT32X3:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 3;
            return true;
        case VertexFormat::SINT8X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::SINT32X4:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 4;
            return true;
        case VertexFormat::SINT32:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::SignedInteger);
            componentCount = 1;
            return true;
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::FLOAT32X2:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 2;
            return true;
        case VertexFormat::FLOAT32X3:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 3;
            return true;
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::FLOAT32X4:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 4;
            return true;
        case VertexFormat::FLOAT32:
            componentType = static_cast<uint32_t>(shader::ShaderVertexComponentType::Float);
            componentCount = 1;
            return true;
        default:
            return false;
    }
}

}  // namespace

Nullable<const BackendBindingName*> FindBackendBindingRecord(
    std::span<const BackendBindingName> records,
    uint32_t generation,
    BindingHandle handle) noexcept {
    if (generation == 0 || !handle.IsValid() ||
        BindingHandleAccess::Generation(handle) != generation) {
        return nullptr;
    }
    const uint32_t index = BindingHandleAccess::RecordIndex(handle);
    if (index >= records.size()) {
        return nullptr;
    }
    return &records[index];
}

uint32_t NextBackendBindingGeneration() noexcept {
    static std::atomic<uint32_t> nextBindingGeneration{1};
    uint32_t generation = nextBindingGeneration.fetch_add(1, std::memory_order_relaxed);
    if (generation == 0) {
        generation = nextBindingGeneration.fetch_add(1, std::memory_order_relaxed);
    }
    return generation;
}

bool ValidateVertexInputStateAgainstArtifact(
    const VertexInputState& state,
    const shader::ShaderArtifactView& artifact) noexcept {
    if (!ValidateVertexInputState(state) ||
        state.Attributes.size() != artifact.VertexInputs().size()) {
        return false;
    }
    for (const shader::WireVertexInputRecord& expected : artifact.VertexInputs()) {
        const std::optional<std::string_view> semantic = artifact.GetName(expected.Semantic);
        if (!semantic.has_value()) {
            return false;
        }
        const auto attribute = std::find_if(
            state.Attributes.begin(),
            state.Attributes.end(),
            [&](const VertexAttribute& value) noexcept {
                return value.Semantic == semantic.value() &&
                       value.SemanticIndex == expected.SemanticIndex;
            });
        if (attribute == state.Attributes.end() || attribute->Location != expected.Location) {
            return false;
        }
        uint32_t componentType = 0;
        uint32_t componentCount = 0;
        if (!GetVertexFormatShape(attribute->Format, componentType, componentCount) ||
            componentType != expected.ComponentType || componentCount != expected.ComponentCount) {
            return false;
        }
    }
    return true;
}

}  // namespace radray::render
