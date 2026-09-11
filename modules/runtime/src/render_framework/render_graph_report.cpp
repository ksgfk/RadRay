#include <radray/runtime/render_framework/render_graph.h>

namespace radray {
namespace {
string Quote(std::string_view value) {
    string result{"\""};
    for (const unsigned char c : value) {
        if (c == '"')
            result += "\\\"";
        else if (c == '\\')
            result += "\\\\";
        else if (c == '\n')
            result += "\\n";
        else if (c == '\r')
            result += "\\r";
        else if (c == '\t')
            result += "\\t";
        else if (c < 32)
            result += fmt::format("\\u{:04x}", c);
        else
            result += static_cast<char>(c);
    }
    result += '"';
    return result;
}
string Indices(std::span<const uint32_t> values) {
    string result{"["};
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) result += ',';
        result += fmt::format("{}", values[i]);
    }
    return result + ']';
}
}  // namespace

string RenderGraphExecutionReport::ToJson() const {
    string result = fmt::format("{{\"name\":{},\"declaredPasses\":{},\"livePasses\":{},\"culledPasses\":{},\"compilePlanReused\":{},\"textures\":{},\"buffers\":{},\"physicalAllocations\":{},\"transitionBarriers\":{},\"uavBarriers\":{},\"passes\":[",
                                Quote(Name), DeclaredPasses, LivePasses, CulledPasses, CompilePlanReused ? "true" : "false", Textures, Buffers, PhysicalAllocations, TransitionBarriers, UavBarriers);
    for (size_t i = 0; i < Passes.size(); ++i) {
        const auto& p = Passes[i];
        if (i) result += ',';
        result += fmt::format("{{\"name\":{},\"type\":{},\"live\":{},\"executed\":{},\"file\":{},\"line\":{},\"dataDependencies\":{},\"hazardDependencies\":{},\"livenessReason\":{},\"reads\":{},\"writes\":{},\"rasterGroup\":{},\"decisions\":[",
                              Quote(p.Name), Quote(EnumName(p.Type)), p.Live, p.Executed, Quote(p.File), p.Line, Indices(p.DataDependencies), Indices(p.HazardDependencies), Quote(p.LivenessReason), Indices(p.Reads), Indices(p.Writes), p.RasterGroup);
        for (size_t d = 0; d < p.Decisions.size(); ++d) {
            if (d) result += ',';
            result += Quote(p.Decisions[d]);
        }
        result += "],\"accesses\":[";
        for (size_t a = 0; a < p.Accesses.size(); ++a) {
            if (a) result += ',';
            const auto& access = p.Accesses[a];
            const auto& r = access.TextureRange;
            result += fmt::format("{{\"resource\":{},\"version\":{},\"state\":{},\"stages\":{},\"read\":{},\"write\":{},\"textureRange\":[{},{},{},{},{}],\"bufferRange\":[{},{}]}}", access.Resource, access.Version, access.State, access.Stages.value(), access.Read, access.Write, r.BaseArrayLayer, r.ArrayLayerCount, r.BaseMipLevel, r.MipLevelCount, r.Aspects.value(), access.BufferRange.Offset, access.BufferRange.Size);
        }
        result += "]}";
    }
    result += "],\"resources\":[";
    for (size_t i = 0; i < Resources.size(); ++i) {
        const auto& r = Resources[i];
        if (i) result += ',';
        result += fmt::format("{{\"name\":{},\"descriptor\":{},\"texture\":{},\"external\":{},\"physicalId\":{},\"firstUse\":{},\"lastUse\":{},\"viewId\":{},\"estimatedBytes\":{},\"physicalSlot\":{},\"port\":{},\"retainedOwner\":{}}}",
                              Quote(r.Name), Quote(r.Descriptor), r.Texture, r.External, r.PhysicalId, r.FirstUse, r.LastUse, r.ViewId, r.EstimatedBytes, r.PhysicalSlot, r.Port, r.RetainedOwner);
    }
    result += "],\"barriers\":[";
    for (size_t i = 0; i < Barriers.size(); ++i) {
        const auto& b = Barriers[i];
        if (i) result += ',';
        result += fmt::format("{{\"pass\":{},\"resource\":{},\"subresource\":{},\"before\":{},\"after\":{},\"uav\":{},\"scopeReason\":{}}}", b.Pass, b.Resource, b.Subresource, b.Before, b.After, b.Uav, Quote(b.ScopeReason));
    }
    result += fmt::format("],\"optimizations\":{{\"mergedRasterPasses\":{},\"discardedStores\":{},\"barrierBatches\":{}}},\"reusedResources\":{},\"executionOrder\":{},\"versions\":[", MergedRasterPasses, DiscardedStores, BarrierBatches, ReusedResources, Indices(ExecutionOrder));
    for (size_t i = 0; i < Versions.size(); ++i) {
        if (i) result += ',';
        const auto& v = Versions[i];
        result += fmt::format("{{\"resource\":{},\"cell\":{},\"version\":{},\"producer\":{},\"predecessor\":{},\"initialized\":{}}}", v.Resource, v.Cell, v.Version, v.Producer, v.Predecessor, v.Initialized);
    }
    result += "],\"diagnostics\":[";
    for (size_t i = 0; i < Diagnostics.size(); ++i) {
        const auto& d = Diagnostics[i];
        if (i) result += ',';
        result += fmt::format("{{\"code\":{},\"graph\":{},\"pass\":{},\"binding\":{},\"resource\":{},\"message\":{},\"file\":{},\"line\":{}}}",
                              Quote(d.Code), Quote(d.Graph), Quote(d.Pass), Quote(d.Binding), Quote(d.Resource), Quote(d.Message), Quote(d.File), d.Line);
    }
    result += fmt::format("],\"graphicsPipelines\":{{\"preparations\":{},\"creations\":{}}},\"pool\":{{\"hits\":{},\"misses\":{},\"created\":{},\"trimmed\":{},\"textures\":{},\"buffers\":{},\"views\":{},\"estimatedBytes\":{},\"peakEstimatedBytes\":{},\"memoryByView\":[",
                          GraphicsPipelinePreparations, GraphicsPipelineCreations,
                          Pool.Hits, Pool.Misses, Pool.Created, Pool.Trimmed, Pool.TextureCount, Pool.BufferCount, Pool.ViewCount, Pool.EstimatedBytes, Pool.PeakEstimatedBytes);
    for (size_t i = 0; i < Pool.MemoryByView.size(); ++i) {
        if (i) result += ',';
        const auto& m = Pool.MemoryByView[i];
        result += fmt::format("{{\"viewId\":{},\"colorTextureBytes\":{},\"depthTextureBytes\":{},\"storageTextureBytes\":{},\"bufferBytes\":{},\"inactiveBytes\":{}}}",
                              m.ViewId, m.ColorTextureBytes, m.DepthTextureBytes, m.StorageTextureBytes, m.BufferBytes, m.InactiveBytes);
    }
    result += "]}}";
    return result;
}

string RenderGraphExecutionReport::ToDot() const {
    string result = "digraph RenderGraph {\n  rankdir=LR;\n";
    for (size_t i = 0; i < Versions.size(); ++i) {
        const auto& v = Versions[i];
        const auto name = v.Resource < Resources.size() ? Resources[v.Resource].Name : string{"invalid"};
        result += fmt::format("  v{} [shape=ellipse,label={}];\n", i, Quote(fmt::format("{} v{} cell {}{}", name, v.Version, v.Cell, v.Initialized ? "" : " (undefined)")));
    }
    for (size_t p = 0; p < Passes.size(); ++p) {
        const auto& pass = Passes[p];
        result += fmt::format("  p{} [shape=box,label={},style={}];\n", p, Quote(pass.Name), pass.Live ? "solid" : "dashed");
        for (const auto value : pass.Reads) result += fmt::format("  v{} -> p{} [label=\"read\"];\n", value, p);
        for (const auto value : pass.Writes) result += fmt::format("  p{} -> v{} [label=\"write\"];\n", p, value);
        for (const auto dependency : pass.HazardDependencies) result += fmt::format("  p{} -> p{} [label=\"hazard\",style=dotted];\n", dependency, p);
    }
    return result + "}\n";
}

string RenderGraphExecutionReport::ToText() const {
    string result = fmt::format("Graph {}: {} live / {} declared, {} culled, compile plan {}; {} transitions, {} UAV barriers\n",
                                Name, LivePasses, DeclaredPasses, CulledPasses, CompilePlanReused ? "reused" : "compiled", TransitionBarriers, UavBarriers);
    if (!FirstErrorCode.empty()) result += fmt::format("  FirstErrorCode: {}\n", FirstErrorCode);
    for (size_t p = 0; p < Passes.size(); ++p) {
        const auto& pass = Passes[p];
        result += fmt::format("  [{}] {} {} {} ({}) at {}:{}\n", p, pass.Live ? "live" : "culled", EnumName(pass.Type), pass.Name, pass.LivenessReason, pass.File, pass.Line);
        for (const auto& decision : pass.Decisions) result += fmt::format("    {}\n", decision);
    }
    for (const auto& r : Resources) result += fmt::format("  {}: {} physical={} lifetime={}..{}\n", r.Name, r.Descriptor, r.PhysicalId, r.FirstUse, r.LastUse);
    for (const auto& b : Barriers) result += fmt::format("  barrier pass={} resource={} sub={} {} -> {} {}\n", b.Pass, b.Resource, b.Subresource, b.Before, b.After, b.Uav ? "UAV" : "transition");
    for (const auto& d : Diagnostics) result += fmt::format("  {}: {}/{}/{}/{}: {} ({}:{})\n", d.Code, d.Graph, d.Pass, d.Binding, d.Resource, d.Message, d.File, d.Line);
    return result;
}
}  // namespace radray
