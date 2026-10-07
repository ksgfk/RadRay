#pragma once
#include <radray/runtime/vertex_layout.h>
#include <radray/runtime/shader_program.h>

namespace radray {
struct GraphicsPipelineKey {
    ShaderProgram* Program;
    GeometryVertexLayout VertexInput;
    render::PrimitiveState Primitive;
    std::optional<render::DepthStencilState> Depth;
    render::MultiSampleState Samples;
    vector<render::ColorTargetState> Colors;
    render::RenderPass* Pass;
    friend bool operator==(const GraphicsPipelineKey&, const GraphicsPipelineKey&) = default;
};
struct GraphicsPipelineKeyHash {
    size_t operator()(const GraphicsPipelineKey& key) const noexcept;
};
/// RT owned. Program/pass identities must survive until idle cache destruction.
class GraphicsPipelineCache {
public:
    explicit GraphicsPipelineCache(render::Device* device) noexcept;
    Nullable<render::GraphicsPipelineState*> GetOrCreate(const GraphicsPipelineKey& key);
    Nullable<const GeometryVertexLayout*> Match(const GeometryVertexLayout& geometry, ShaderProgram* program);
    size_t GetSize() const noexcept { return _pipelines.size(); }

private:
    struct VertexMatch {
        GeometryVertexLayout Geometry;
        ShaderProgram* Program;
        std::optional<GeometryVertexLayout> Resolved;
    };
    render::Device* _device;
    unordered_map<GraphicsPipelineKey, unique_ptr<render::GraphicsPipelineState>, GraphicsPipelineKeyHash> _pipelines;
    vector<unique_ptr<VertexMatch>> _matches;
};
}  // namespace radray
