#include <radray/runtime/pipeline_cache.h>

#include <tuple>

#include <radray/logger.h>

namespace radray {
GraphicsPipelineCache::GraphicsPipelineCache(render::Device& device) : _device(device) {}

bool GraphicsPipelineCache::Entry::Matches(const Entry& other) const {
    return std::tie(layout, vs, ps, vsEntry, psEntry, buffers, attributes, primitive, depth, samples, colors, attachments) ==
           std::tie(other.layout, other.vs, other.ps, other.vsEntry, other.psEntry, other.buffers, other.attributes, other.primitive, other.depth, other.samples, other.colors, other.attachments);
}
Nullable<render::GraphicsPipelineState*> GraphicsPipelineCache::Get(const render::GraphicsPipelineStateDescriptor& d, const render::AttachmentSignature& attachments) {
    if (render::GetAttachmentSignature(d) != attachments) {
        RADRAY_ERR_LOG("Pipeline attachment signature mismatch");
        return nullptr;
    }
    Entry key;
    key.layout = d.PipelineLayout;
    if (d.VS) {
        key.vs = d.VS->Target;
        key.vsEntry = d.VS->EntryPoint;
    }
    if (d.PS) {
        key.ps = d.PS->Target;
        key.psEntry = d.PS->EntryPoint;
    }
    key.buffers.assign(d.VertexInput.Buffers.begin(), d.VertexInput.Buffers.end());
    for (const auto& a : d.VertexInput.Attributes) key.attributes.push_back({a.BufferBinding, a.Offset, string(a.Semantic), a.SemanticIndex, a.Format, a.Location});
    key.primitive = d.Primitive;
    key.depth = d.DepthStencil;
    key.samples = d.MultiSample;
    key.colors.assign(d.ColorTargets.begin(), d.ColorTargets.end());
    key.attachments = attachments;
    for (const auto& entry : _entries)
        if (entry.Matches(key)) return entry.pipeline.get();
    key.pipeline = _device.CreateGraphicsPipelineState(d).Release();
    if (!key.pipeline) {
        RADRAY_ERR_LOG("Cannot create graphics pipeline");
        return nullptr;
    }
    auto* result = key.pipeline.get();
    _entries.push_back(std::move(key));
    return result;
}
}  // namespace radray
