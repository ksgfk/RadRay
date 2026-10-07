#include <radray/runtime/vertex_layout.h>
#include <algorithm>
#include <radray/logger.h>

namespace radray {
vector<render::VertexAttribute> GeometryVertexLayout::GetAttributes() const {
    vector<render::VertexAttribute> result;
    result.reserve(Attributes.size());
    for (const auto& a : Attributes) result.push_back({a.Binding, a.Offset, a.Semantic, a.SemanticIndex, a.Format, a.Location});
    return result;
}
bool ValidateGeometryVertexLayout(const GeometryVertexLayout& layout) noexcept {
    for (size_t i = 0; i < layout.Streams.size(); ++i) {
        const auto& stream = layout.Streams[i];
        if (!stream.ArrayStride || (stream.StepMode != render::VertexStepMode::Vertex && stream.StepMode != render::VertexStepMode::Instance)) return false;
        for (size_t j = 0; j < i; ++j)
            if (stream.Binding == layout.Streams[j].Binding) return false;
    }
    for (size_t i = 0; i < layout.Attributes.size(); ++i) {
        const auto& a = layout.Attributes[i];
        const auto stream = std::find_if(layout.Streams.begin(), layout.Streams.end(), [&](const auto& s) { return s.Binding == a.Binding; });
        const uint32_t size = render::GetVertexFormatSizeInBytes(a.Format);
        if (a.Semantic.empty() || !size || stream == layout.Streams.end() || a.Offset > stream->ArrayStride || size > stream->ArrayStride - a.Offset) return false;
        for (size_t j = 0; j < i; ++j)
            if (a.Semantic == layout.Attributes[j].Semantic && a.SemanticIndex == layout.Attributes[j].SemanticIndex) return false;
    }
    return true;
}
std::optional<GeometryVertexLayout> MatchVertexLayout(const GeometryVertexLayout& layout, const shader::ShaderArtifactView& shader) {
    if (!ValidateGeometryVertexLayout(layout)) return std::nullopt;
    GeometryVertexLayout result;
    result.Streams = layout.Streams;
    for (const auto& input : shader.VertexInputs()) {
        const auto name = shader.GetName(input.Semantic);
        if (!name) return std::nullopt;
        const auto found = std::find_if(layout.Attributes.begin(), layout.Attributes.end(), [&](const auto& a) { return a.Semantic == *name && a.SemanticIndex == input.SemanticIndex; });
        if (found == layout.Attributes.end() || input.ComponentCount < 1 || input.ComponentCount > 4) {
            RADRAY_ERR_LOG("missing geometry vertex input {}{}", *name, input.SemanticIndex);
            return std::nullopt;
        }
        const render::VertexFormat floats[]{render::VertexFormat::FLOAT32, render::VertexFormat::FLOAT32X2, render::VertexFormat::FLOAT32X3, render::VertexFormat::FLOAT32X4};
        const render::VertexFormat uints[]{render::VertexFormat::UINT32, render::VertexFormat::UINT32X2, render::VertexFormat::UINT32X3, render::VertexFormat::UINT32X4};
        const render::VertexFormat ints[]{render::VertexFormat::SINT32, render::VertexFormat::SINT32X2, render::VertexFormat::SINT32X3, render::VertexFormat::SINT32X4};
        if (input.ComponentType != uint32_t(shader::ShaderVertexComponentType::Float) &&
            input.ComponentType != uint32_t(shader::ShaderVertexComponentType::UnsignedInteger) &&
            input.ComponentType != uint32_t(shader::ShaderVertexComponentType::SignedInteger)) return std::nullopt;
        const auto expected = input.ComponentType == uint32_t(shader::ShaderVertexComponentType::Float) ? floats[input.ComponentCount - 1] : input.ComponentType == uint32_t(shader::ShaderVertexComponentType::UnsignedInteger) ? uints[input.ComponentCount - 1]
                                                                                                                                                                                                                                 : ints[input.ComponentCount - 1];
        if (found->Format != expected) {
            RADRAY_ERR_LOG("incompatible geometry vertex input {}{}", *name, input.SemanticIndex);
            return std::nullopt;
        }
        result.Attributes.push_back(*found);
        result.Attributes.back().Location = input.Location;
    }
    return result;
}
}  // namespace radray
