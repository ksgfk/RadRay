#pragma once
#include <radray/render/rhi.h>
#include <radray/shader/shader_artifact.h>

namespace radray {
struct GeometryVertexAttribute {
    string Semantic;
    uint32_t SemanticIndex{0};
    render::VertexFormat Format{render::VertexFormat::UNKNOWN};
    uint32_t Binding{0}, Offset{0}, Location{0};
    friend bool operator==(const GeometryVertexAttribute&, const GeometryVertexAttribute&) = default;
};
struct GeometryVertexLayout {
    vector<render::VertexBufferLayout> Streams;
    vector<GeometryVertexAttribute> Attributes;
    friend bool operator==(const GeometryVertexLayout&, const GeometryVertexLayout&) = default;
    vector<render::VertexAttribute> GetAttributes() const;
};
bool ValidateGeometryVertexLayout(const GeometryVertexLayout& layout) noexcept;
std::optional<GeometryVertexLayout> MatchVertexLayout(const GeometryVertexLayout& layout, const shader::ShaderArtifactView& shader);
}  // namespace radray
