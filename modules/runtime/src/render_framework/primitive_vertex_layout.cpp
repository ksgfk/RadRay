#include <radray/runtime/render_framework/primitive_vertex_layout.h>

#include <algorithm>
#include <utility>

namespace radray {
namespace {

std::optional<render::VertexFormat> GetVertexFormat(
    VertexDataType type,
    uint16_t componentCount) noexcept {
    switch (type) {
        case VertexDataType::FLOAT:
            switch (componentCount) {
                case 1: return render::VertexFormat::FLOAT32;
                case 2: return render::VertexFormat::FLOAT32X2;
                case 3: return render::VertexFormat::FLOAT32X3;
                case 4: return render::VertexFormat::FLOAT32X4;
                default: return std::nullopt;
            }
        case VertexDataType::UINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::UINT32;
                case 2: return render::VertexFormat::UINT32X2;
                case 3: return render::VertexFormat::UINT32X3;
                case 4: return render::VertexFormat::UINT32X4;
                default: return std::nullopt;
            }
        case VertexDataType::SINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::SINT32;
                case 2: return render::VertexFormat::SINT32X2;
                case 3: return render::VertexFormat::SINT32X3;
                case 4: return render::VertexFormat::SINT32X4;
                default: return std::nullopt;
            }
    }
    return std::nullopt;
}

}  // namespace

std::optional<PrimitiveVertexLayout> PrimitiveVertexLayout::FromMeshPrimitive(
    const MeshPrimitive& primitive) noexcept {
    if (primitive.VertexBuffers.empty()) {
        return std::nullopt;
    }

    const uint32_t sourceBuffer = primitive.VertexBuffers.front().BufferIndex;
    const uint32_t stride = primitive.VertexBuffers.front().Stride;
    if (stride == 0) {
        return std::nullopt;
    }

    PrimitiveVertexLayout result;
    result.Buffers.push_back(render::VertexBufferLayout{
        .Binding = 0,
        .ArrayStride = stride,
        .StepMode = render::VertexStepMode::Vertex});
    result.Attributes.reserve(primitive.VertexBuffers.size());
    for (const VertexBufferEntry& entry : primitive.VertexBuffers) {
        const std::optional<render::VertexFormat> format =
            GetVertexFormat(entry.Type, entry.ComponentCount);
        const uint32_t elementSize =
            GetVertexDataSizeInBytes(entry.Type, entry.ComponentCount);
        if (entry.BufferIndex != sourceBuffer || entry.Stride != stride ||
            entry.Semantic.empty() || !format.has_value() || elementSize == 0 ||
            entry.Offset > stride || elementSize > stride - entry.Offset) {
            return std::nullopt;
        }
        const auto duplicate = std::find_if(
            result.Attributes.begin(),
            result.Attributes.end(),
            [&](const PrimitiveVertexAttribute& value) noexcept {
                return value.Semantic == entry.Semantic &&
                       value.SemanticIndex == entry.SemanticIndex;
            });
        if (duplicate != result.Attributes.end()) {
            return std::nullopt;
        }
        result.Attributes.push_back(PrimitiveVertexAttribute{
            .Semantic = entry.Semantic,
            .SemanticIndex = entry.SemanticIndex,
            .BufferBinding = 0,
            .Offset = entry.Offset,
            .Format = format.value()});
    }
    return result;
}

ResolvedPrimitiveVertexLayout::ResolvedPrimitiveVertexLayout(
    const ResolvedPrimitiveVertexLayout& other)
    : _buffers(other._buffers),
      _semantics(other._semantics),
      _attributes(other._attributes) {
    RebindSemantics();
}

ResolvedPrimitiveVertexLayout::ResolvedPrimitiveVertexLayout(
    ResolvedPrimitiveVertexLayout&& other) noexcept
    : _buffers(std::move(other._buffers)),
      _semantics(std::move(other._semantics)),
      _attributes(std::move(other._attributes)) {
    RebindSemantics();
}

ResolvedPrimitiveVertexLayout& ResolvedPrimitiveVertexLayout::operator=(
    const ResolvedPrimitiveVertexLayout& other) {
    if (this != &other) {
        _buffers = other._buffers;
        _semantics = other._semantics;
        _attributes = other._attributes;
        RebindSemantics();
    }
    return *this;
}

ResolvedPrimitiveVertexLayout& ResolvedPrimitiveVertexLayout::operator=(
    ResolvedPrimitiveVertexLayout&& other) noexcept {
    if (this != &other) {
        _buffers = std::move(other._buffers);
        _semantics = std::move(other._semantics);
        _attributes = std::move(other._attributes);
        RebindSemantics();
    }
    return *this;
}

render::VertexInputState ResolvedPrimitiveVertexLayout::GetState() const noexcept {
    return render::VertexInputState{
        .Buffers = _buffers,
        .Attributes = _attributes};
}

void ResolvedPrimitiveVertexLayout::RebindSemantics() noexcept {
    if (_semantics.size() != _attributes.size()) {
        return;
    }
    for (size_t index = 0; index < _attributes.size(); ++index) {
        _attributes[index].Semantic = _semantics[index];
    }
}

std::optional<ResolvedPrimitiveVertexLayout> ResolvePrimitiveVertexLayout(
    const PrimitiveVertexLayout& layout,
    const shader::ShaderArtifactView& artifact) noexcept {
    if (layout.Attributes.size() != artifact.VertexInputs().size()) {
        return std::nullopt;
    }

    ResolvedPrimitiveVertexLayout result;
    result._buffers = layout.Buffers;
    result._semantics.reserve(layout.Attributes.size());
    result._attributes.reserve(layout.Attributes.size());
    for (const PrimitiveVertexAttribute& source : layout.Attributes) {
        const auto input = std::find_if(
            artifact.VertexInputs().begin(),
            artifact.VertexInputs().end(),
            [&](const shader::WireVertexInputRecord& value) noexcept {
                const std::optional<std::string_view> semantic =
                    artifact.GetName(value.Semantic);
                return semantic.has_value() && semantic.value() == source.Semantic &&
                       value.SemanticIndex == source.SemanticIndex;
            });
        if (input == artifact.VertexInputs().end()) {
            return std::nullopt;
        }
        const bool duplicateLocation = std::any_of(
            result._attributes.begin(),
            result._attributes.end(),
            [&](const render::VertexAttribute& value) noexcept {
                return value.Location == input->Location;
            });
        if (duplicateLocation) {
            return std::nullopt;
        }
        result._semantics.push_back(source.Semantic);
        result._attributes.push_back(render::VertexAttribute{
            .BufferBinding = source.BufferBinding,
            .Offset = source.Offset,
            .Semantic = {},
            .SemanticIndex = source.SemanticIndex,
            .Format = source.Format,
            .Location = input->Location});
    }
    result.RebindSemantics();
    if (!render::ValidateVertexInputStateAgainstArtifact(result.GetState(), artifact)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace radray
