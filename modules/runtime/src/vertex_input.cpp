#include <radray/runtime/vertex_input.h>
#include <radray/gpu_impl/validation.h>

#include <algorithm>

namespace radray {

bool ValidateVertexInputLayout(const VertexInput& input) noexcept {
    for (size_t i = 0; i < input.Layouts.size(); ++i) {
        const auto& layout = input.Layouts[i];
        if (!layout.ArrayStride || !EnumContains(layout.StepMode)) return false;
        for (size_t j = 0; j < i; ++j)
            if (input.Layouts[j].Binding == layout.Binding) return false;
    }
    for (size_t i = 0; i < input.Attributes.size(); ++i) {
        const auto& attribute = input.Attributes[i];
        const auto size = gpu::GetVertexFormatSizeInBytes(attribute.Format);
        const auto layout = std::find_if(input.Layouts.begin(), input.Layouts.end(), [&](const auto& l) { return l.Binding == attribute.BufferBinding; });
        if (attribute.Semantic.empty() || !size || layout == input.Layouts.end() ||
            attribute.Offset > layout->ArrayStride || size > layout->ArrayStride - attribute.Offset) return false;
        for (size_t j = 0; j < i; ++j)
            if (input.Attributes[j].Semantic == attribute.Semantic && input.Attributes[j].SemanticIndex == attribute.SemanticIndex) return false;
    }
    return true;
}

namespace {

bool CompatibleVertexFormat(gpu::VertexFormat format, const shader::ShaderVertexAttributeSignatureDesc& signature) noexcept {
    using F = gpu::VertexFormat;
    shader::ShaderVertexComponentType kind = shader::ShaderVertexComponentType::Float;
    uint32_t count = 0;
    switch (format) {
        case F::UINT8X2:
        case F::UINT16X2:
        case F::UINT32X2:
            kind = shader::ShaderVertexComponentType::UnsignedInteger;
            count = 2;
            break;
        case F::UINT8X4:
        case F::UINT16X4:
        case F::UINT32X4:
            kind = shader::ShaderVertexComponentType::UnsignedInteger;
            count = 4;
            break;
        case F::UINT32:
            kind = shader::ShaderVertexComponentType::UnsignedInteger;
            count = 1;
            break;
        case F::UINT32X3:
            kind = shader::ShaderVertexComponentType::UnsignedInteger;
            count = 3;
            break;
        case F::SINT8X2:
        case F::SINT16X2:
        case F::SINT32X2:
            kind = shader::ShaderVertexComponentType::SignedInteger;
            count = 2;
            break;
        case F::SINT8X4:
        case F::SINT16X4:
        case F::SINT32X4:
            kind = shader::ShaderVertexComponentType::SignedInteger;
            count = 4;
            break;
        case F::SINT32:
            kind = shader::ShaderVertexComponentType::SignedInteger;
            count = 1;
            break;
        case F::SINT32X3:
            kind = shader::ShaderVertexComponentType::SignedInteger;
            count = 3;
            break;
        case F::UNORM8X2:
        case F::SNORM8X2:
        case F::UNORM16X2:
        case F::SNORM16X2:
        case F::FLOAT16X2:
        case F::FLOAT32X2: count = 2; break;
        case F::UNORM8X4:
        case F::SNORM8X4:
        case F::UNORM16X4:
        case F::SNORM16X4:
        case F::FLOAT16X4:
        case F::FLOAT32X4: count = 4; break;
        case F::FLOAT32: count = 1; break;
        case F::FLOAT32X3: count = 3; break;
        default: return false;
    }
    return signature.ComponentKind == kind && signature.ComponentCount == count;
}

}  // namespace

bool ResolveVertexInputState(const VertexInput& meshInput, std::span<const shader::ShaderVertexAttributeSignatureDesc> shaderInputs, ResolvedVertexInputState& output) {
    if (!ValidateVertexInputLayout(meshInput)) return false;
    ResolvedVertexInputState candidate;
    candidate.Layouts = meshInput.Layouts;
    for (const auto& input : shaderInputs) {
        const auto attribute = std::find_if(meshInput.Attributes.begin(), meshInput.Attributes.end(), [&](const auto& a) {
            return a.Semantic == input.Semantic && a.SemanticIndex == input.SemanticIndex;
        });
        if (attribute == meshInput.Attributes.end() || !CompatibleVertexFormat(attribute->Format, input)) return false;
        candidate.Attributes.push_back({attribute->BufferBinding, attribute->Offset, attribute->Semantic, attribute->SemanticIndex, attribute->Format, input.Location});
    }
    if (!gpu::ValidateVertexInputState({candidate.Layouts, candidate.Attributes})) return false;
    output = std::move(candidate);
    return true;
}

}  // namespace radray
