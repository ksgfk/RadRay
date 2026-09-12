#include <radray/runtime/render_framework/primitive_vertex_layout.h>
#include "render_memory_measure.h"

#include <algorithm>
#include <atomic>
#include <tuple>
#include <utility>

#include <radray/hash.h>
#include <radray/logger.h>

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

size_t PrimitiveVertexLayoutHash::operator()(const PrimitiveVertexLayout& layout) const noexcept {
    HashCode hash;
    hash.Add(layout.Buffers.size());
    for (const auto& buffer : layout.Buffers) {
        hash.Add(buffer.Binding);
        hash.Add(buffer.ArrayStride);
        hash.Add(static_cast<uint32_t>(buffer.StepMode));
    }
    hash.Add(layout.Attributes.size());
    for (const auto& attribute : layout.Attributes) {
        hash.Add(attribute.Semantic);
        hash.Add(attribute.SemanticIndex);
        hash.Add(attribute.BufferBinding);
        hash.Add(attribute.Offset);
        hash.Add(static_cast<uint32_t>(attribute.Format));
    }
    return hash.ToHashCode();
}

RenderMemoryStats PrimitiveVertexLayoutRegistry::GetMemoryStats() const noexcept {
    RenderMemoryStats result;
    result.ObjectBytes = sizeof(*this);
    result.LiveEntries = _layouts.size();
    detail::MeasureMap(result, _layouts);
    detail::MeasureMap(result, _byId);
    for (const auto& [layout, entry] : _layouts) {
        detail::MeasureVector(result, layout.Buffers, true);
        detail::MeasureVector(result, layout.Attributes, true);
        for (const auto& attribute : layout.Attributes) detail::MeasureString(result, attribute.Semantic);
        result.DependencyEdges += entry.References;
    }
    return result;
}

PrimitiveVertexLayoutRegistry::PrimitiveVertexLayoutRegistry(const PrimitiveVertexLayoutRegistry& other) : _layouts(other._layouts) {
    RebuildIndex();
}

PrimitiveVertexLayoutRegistry& PrimitiveVertexLayoutRegistry::operator=(const PrimitiveVertexLayoutRegistry& other) {
    if (this == &other) return *this;
    _layouts = other._layouts;
    RebuildIndex();
    return *this;
}

void PrimitiveVertexLayoutRegistry::RebuildIndex() {
    _byId.clear();
    _byId.reserve(_layouts.size());
    for (const auto& [layout, entry] : _layouts)
        if (entry.References) _byId.emplace(entry.Id.Value, &layout);
}

PrimitiveVertexLayoutRegistry::Entry& PrimitiveVertexLayoutRegistry::FindOrAdd(const PrimitiveVertexLayout& layout, bool retain) {
    PrimitiveVertexLayout normalized = layout;
    std::sort(normalized.Buffers.begin(), normalized.Buffers.end(), [](const auto& a, const auto& b) {
        return std::tie(a.Binding, a.ArrayStride, a.StepMode) < std::tie(b.Binding, b.ArrayStride, b.StepMode);
    });
    std::sort(normalized.Attributes.begin(), normalized.Attributes.end(), [](const auto& a, const auto& b) {
        return std::tie(a.Semantic, a.SemanticIndex, a.BufferBinding, a.Offset, a.Format) <
               std::tie(b.Semantic, b.SemanticIndex, b.BufferBinding, b.Offset, b.Format);
    });
    const auto [entry, inserted] = _layouts.try_emplace(std::move(normalized));
    if (inserted) {
        static std::atomic<uint64_t> next{1};
        entry->second.Id.Value = next.fetch_add(1, std::memory_order_relaxed);
        if (!entry->second.Id.IsValid()) RADRAY_ABORT("Primitive vertex layout identity space exhausted");
    }
    if (retain && entry->second.References == 0) _byId.emplace(entry->second.Id.Value, &entry->first);
    return entry->second;
}

PrimitiveVertexLayoutId PrimitiveVertexLayoutRegistry::Intern(const PrimitiveVertexLayout& layout) {
    auto& entry = FindOrAdd(layout, false);
    entry.Pinned = true;
    return entry.Id;
}

PrimitiveVertexLayoutId PrimitiveVertexLayoutRegistry::Acquire(const PrimitiveVertexLayout& layout) {
    auto& entry = FindOrAdd(layout, true);
    if (entry.References == SIZE_MAX) RADRAY_ABORT("Primitive vertex layout reference count exhausted");
    ++entry.References;
    return entry.Id;
}

bool PrimitiveVertexLayoutRegistry::Release(PrimitiveVertexLayoutId id) noexcept {
    const auto indexed = _byId.find(id.Value);
    if (indexed == _byId.end()) return false;
    const auto entry = _layouts.find(*indexed->second);
    if (entry->second.References == 0) return false;
    if (--entry->second.References == 0) {
        _byId.erase(indexed);
        if (!entry->second.Pinned) _layouts.erase(entry);
    }
    return true;
}

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
    ResolvedPrimitiveVertexLayout result;
    result._semantics.reserve(artifact.VertexInputs().size());
    result._attributes.reserve(artifact.VertexInputs().size());
    for (const auto& input : artifact.VertexInputs()) {
        const auto semantic = artifact.GetName(input.Semantic);
        if (!semantic) return std::nullopt;
        const auto matches = [&](const PrimitiveVertexAttribute& value) {
            return value.Semantic == *semantic && value.SemanticIndex == input.SemanticIndex;
        };
        if (std::count_if(layout.Attributes.begin(), layout.Attributes.end(), matches) != 1) return std::nullopt;
        const auto& source = *std::find_if(layout.Attributes.begin(), layout.Attributes.end(), matches);
        result._semantics.push_back(source.Semantic);
        result._attributes.push_back(render::VertexAttribute{
            .BufferBinding = source.BufferBinding, .Offset = source.Offset, .Semantic = {}, .SemanticIndex = source.SemanticIndex, .Format = source.Format, .Location = input.Location});
    }
    for (const auto& buffer : layout.Buffers) {
        if (std::any_of(result._attributes.begin(), result._attributes.end(), [&](const auto& attribute) { return attribute.BufferBinding == buffer.Binding; }))
            result._buffers.push_back(buffer);
    }
    result.RebindSemantics();
    if (!render::ValidateVertexInputStateAgainstArtifact(result.GetState(), artifact)) {
        return std::nullopt;
    }
    return result;
}

}  // namespace radray
