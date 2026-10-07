#include <radray/runtime/graphics_pipeline_cache.h>
#include <radray/hash.h>

namespace radray {
size_t GraphicsPipelineKeyHash::operator()(const GraphicsPipelineKey& key) const noexcept {
    HashCode hash;
    hash.Add(key.Program);
    hash.Add(key.Pass);
    for (const auto& s : key.VertexInput.Streams) {
        hash.Add(s.Binding);
        hash.Add(s.ArrayStride);
        hash.Add(s.StepMode);
    }
    for (const auto& a : key.VertexInput.Attributes) {
        hash.Add(a.Semantic);
        hash.Add(a.SemanticIndex);
        hash.Add(a.Format);
        hash.Add(a.Binding);
        hash.Add(a.Offset);
        hash.Add(a.Location);
    }
    const auto& p = key.Primitive;
    hash.Add(p.Topology);
    hash.Add(p.FaceClockwise);
    hash.Add(p.Cull);
    hash.Add(p.Poly);
    hash.Add(p.UnclippedDepth);
    hash.Add(p.Conservative);
    hash.Add(p.StripIndexFormat.has_value());
    if (p.StripIndexFormat) hash.Add(*p.StripIndexFormat);
    hash.Add(key.Depth.has_value());
    if (key.Depth) {
        const auto& d = *key.Depth;
        hash.Add(d.Format);
        hash.Add(d.DepthCompare);
        hash.Add(d.DepthTestEnable);
        hash.Add(d.DepthWriteEnable);
        hash.Add(d.DepthBias.Constant);
        hash.Add(d.DepthBias.SlopScale);
        hash.Add(d.DepthBias.Clamp);
        hash.Add(d.Stencil.has_value());
        if (d.Stencil) {
            for (const auto face : {d.Stencil->Front, d.Stencil->Back}) {
                hash.Add(face.Compare);
                hash.Add(face.FailOp);
                hash.Add(face.DepthFailOp);
                hash.Add(face.PassOp);
            }
            hash.Add(d.Stencil->ReadMask);
            hash.Add(d.Stencil->WriteMask);
        }
    }
    hash.Add(key.Samples.Count);
    hash.Add(key.Samples.Mask);
    hash.Add(key.Samples.AlphaToCoverageEnable);
    for (const auto& c : key.Colors) {
        hash.Add(c.Format);
        hash.Add(static_cast<render::ColorWrite>(c.WriteMask));
        hash.Add(c.Blend.has_value());
        if (c.Blend)
            for (auto component : {c.Blend->Color, c.Blend->Alpha}) {
                hash.Add(component.Src);
                hash.Add(component.Dst);
                hash.Add(component.Op);
            }
    }
    return hash.ToHashCode();
}
GraphicsPipelineCache::GraphicsPipelineCache(render::Device* device) noexcept : _device(device) {}
Nullable<render::GraphicsPipelineState*> GraphicsPipelineCache::GetOrCreate(const GraphicsPipelineKey& key) {
    const auto found = _pipelines.find(key);
    if (found != _pipelines.end()) return found->second.get();
    const auto attributes = key.VertexInput.GetAttributes();
    auto pso = _device->CreateGraphicsPipelineState({key.Program->GetPipelineLayout(), key.Program->GetStage(shader::ShaderStage::Vertex), key.Program->GetStage(shader::ShaderStage::Pixel), {key.VertexInput.Streams, attributes}, key.Primitive, key.Depth, key.Samples, key.Colors, key.Pass});
    if (!pso) return nullptr;
    return _pipelines.emplace(key, pso.Release()).first->second.get();
}
Nullable<const GeometryVertexLayout*> GraphicsPipelineCache::Match(const GeometryVertexLayout& geometry, ShaderProgram* program) {
    for (const auto& match : _matches)
        if (match->Program == program && match->Geometry == geometry) return match->Resolved ? &*match->Resolved : nullptr;
    auto match = make_unique<VertexMatch>(VertexMatch{geometry, program, MatchVertexLayout(geometry, program->GetArtifact().Generic())});
    auto result = match->Resolved ? Nullable<const GeometryVertexLayout*>{&*match->Resolved} : nullptr;
    _matches.push_back(std::move(match));
    return result;
}
}  // namespace radray
