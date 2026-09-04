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
    string result = fmt::format("{{\"name\":{},\"declaredPasses\":{},\"livePasses\":{},\"culledPasses\":{},\"textures\":{},\"buffers\":{},\"physicalAllocations\":{},\"transitionBarriers\":{},\"uavBarriers\":{},\"passes\":[",
                                Quote(Name), DeclaredPasses, LivePasses, CulledPasses, Textures, Buffers, PhysicalAllocations, TransitionBarriers, UavBarriers);
    for (size_t i = 0; i < Passes.size(); ++i) {
        const auto& p = Passes[i];
        if (i) result += ',';
        result += fmt::format("{{\"name\":{},\"type\":{},\"live\":{},\"executed\":{},\"file\":{},\"line\":{},\"dataDependencies\":{},\"hazardDependencies\":{},\"livenessReason\":{}}}",
                              Quote(p.Name), Quote(EnumName(p.Type)), p.Live, p.Executed, Quote(p.File), p.Line, Indices(p.DataDependencies), Indices(p.HazardDependencies), Quote(p.LivenessReason));
    }
    result += "],\"resources\":[";
    for (size_t i = 0; i < Resources.size(); ++i) {
        const auto& r = Resources[i];
        if (i) result += ',';
        result += fmt::format("{{\"name\":{},\"descriptor\":{},\"texture\":{},\"external\":{},\"physicalId\":{},\"firstUse\":{},\"lastUse\":{}}}",
                              Quote(r.Name), Quote(r.Descriptor), r.Texture, r.External, r.PhysicalId, r.FirstUse, r.LastUse);
    }
    result += "],\"barriers\":[";
    for (size_t i = 0; i < Barriers.size(); ++i) {
        const auto& b = Barriers[i];
        if (i) result += ',';
        result += fmt::format("{{\"pass\":{},\"resource\":{},\"subresource\":{},\"before\":{},\"after\":{},\"uav\":{}}}", b.Pass, b.Resource, b.Subresource, b.Before, b.After, b.Uav);
    }
    result += "],\"diagnostics\":[";
    for (size_t i = 0; i < Diagnostics.size(); ++i) {
        const auto& d = Diagnostics[i];
        if (i) result += ',';
        result += fmt::format("{{\"code\":{},\"graph\":{},\"pass\":{},\"resource\":{},\"message\":{},\"file\":{},\"line\":{}}}",
                              Quote(d.Code), Quote(d.Graph), Quote(d.Pass), Quote(d.Resource), Quote(d.Message), Quote(d.File), d.Line);
    }
    result += fmt::format("],\"pool\":{{\"hits\":{},\"misses\":{},\"created\":{},\"trimmed\":{},\"textures\":{},\"buffers\":{},\"views\":{},\"estimatedBytes\":{}}}}}",
                          Pool.Hits, Pool.Misses, Pool.Created, Pool.Trimmed, Pool.TextureCount, Pool.BufferCount, Pool.ViewCount, Pool.EstimatedBytes);
    return result;
}

string RenderGraphExecutionReport::ToDot() const {
    string result = "digraph RenderGraph {\n";
    for (size_t p = 0; p < Passes.size(); ++p) {
        const auto& pass = Passes[p];
        result += fmt::format("  p{} [label={},style={}];\n", p, Quote(pass.Name), pass.Live ? "solid" : "dashed");
        for (const auto dependency : pass.DataDependencies) result += fmt::format("  p{} -> p{} [label=\"content\"];\n", dependency, p);
        for (const auto dependency : pass.HazardDependencies) result += fmt::format("  p{} -> p{} [label=\"hazard\",style=dotted];\n", dependency, p);
    }
    return result + "}\n";
}

string RenderGraphExecutionReport::ToText() const {
    string result = fmt::format("Graph {}: {} live / {} declared, {} culled; {} transitions, {} UAV barriers\n", Name, LivePasses, DeclaredPasses, CulledPasses, TransitionBarriers, UavBarriers);
    for (size_t p = 0; p < Passes.size(); ++p) {
        const auto& pass = Passes[p];
        result += fmt::format("  [{}] {} {} {} ({}) at {}:{}\n", p, pass.Live ? "live" : "culled", EnumName(pass.Type), pass.Name, pass.LivenessReason, pass.File, pass.Line);
    }
    for (const auto& r : Resources) result += fmt::format("  {}: {} physical={} lifetime={}..{}\n", r.Name, r.Descriptor, r.PhysicalId, r.FirstUse, r.LastUse);
    for (const auto& b : Barriers) result += fmt::format("  barrier pass={} resource={} sub={} {} -> {} {}\n", b.Pass, b.Resource, b.Subresource, b.Before, b.After, b.Uav ? "UAV" : "transition");
    for (const auto& d : Diagnostics) result += fmt::format("  {}: {}/{}/{}: {} ({}:{})\n", d.Code, d.Graph, d.Pass, d.Resource, d.Message, d.File, d.Line);
    return result;
}
}  // namespace radray
