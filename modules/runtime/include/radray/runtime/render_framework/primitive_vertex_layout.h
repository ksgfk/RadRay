#pragma once

#include <optional>

#include <radray/render/backend/pipeline_layout_types.h>
#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>
#include <radray/types.h>
#include <radray/vertex_data.h>

namespace radray {

struct PrimitiveVertexAttribute {
    string Semantic;
    uint32_t SemanticIndex{0};
    uint32_t BufferBinding{0};
    uint32_t Offset{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};

    friend bool operator==(const PrimitiveVertexAttribute&, const PrimitiveVertexAttribute&) noexcept = default;
};

/// Geometry-owned physical vertex layout. Shader locations are resolved only when a
/// concrete artifact is used to create a PSO.
class PrimitiveVertexLayout {
public:
    vector<render::VertexBufferLayout> Buffers;
    vector<PrimitiveVertexAttribute> Attributes;

    static std::optional<PrimitiveVertexLayout> FromMeshPrimitive(
        const MeshPrimitive& primitive) noexcept;

    friend bool operator==(const PrimitiveVertexLayout&, const PrimitiveVertexLayout&) noexcept = default;
};

class ResolvedPrimitiveVertexLayout {
public:
    ResolvedPrimitiveVertexLayout() noexcept = default;
    ResolvedPrimitiveVertexLayout(const ResolvedPrimitiveVertexLayout& other);
    ResolvedPrimitiveVertexLayout(ResolvedPrimitiveVertexLayout&& other) noexcept;
    ResolvedPrimitiveVertexLayout& operator=(const ResolvedPrimitiveVertexLayout& other);
    ResolvedPrimitiveVertexLayout& operator=(ResolvedPrimitiveVertexLayout&& other) noexcept;
    ~ResolvedPrimitiveVertexLayout() noexcept = default;

    render::VertexInputState GetState() const noexcept;

private:
    friend std::optional<ResolvedPrimitiveVertexLayout> ResolvePrimitiveVertexLayout(
        const PrimitiveVertexLayout&,
        const shader::ShaderArtifactView&) noexcept;

    void RebindSemantics() noexcept;

    vector<render::VertexBufferLayout> _buffers;
    vector<string> _semantics;
    vector<render::VertexAttribute> _attributes;
};

std::optional<ResolvedPrimitiveVertexLayout> ResolvePrimitiveVertexLayout(
    const PrimitiveVertexLayout& layout,
    const shader::ShaderArtifactView& artifact) noexcept;

}  // namespace radray
