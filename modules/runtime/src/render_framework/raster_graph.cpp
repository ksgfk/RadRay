#include <radray/runtime/render_framework/raster_graph.h>
#include <algorithm>

namespace radray {
RasterResourceId RasterGraph::AddResource(RasterGraphResource resource) {
    _resources.push_back(resource);
    return static_cast<RasterResourceId>(_resources.size() - 1);
}
RasterNodeId RasterGraph::AddNode(RasterGraphNode node) {
    _nodes.push_back(std::move(node));
    return static_cast<RasterNodeId>(_nodes.size() - 1);
}
std::optional<CompiledRasterGraph> RasterGraph::Compile() const {
    for (size_t i = 0; i < _resources.size(); ++i)
        for (size_t j = 0; j < i; ++j)
            if (_resources[i].Texture && _resources[i].Texture.Get() == _resources[j].Texture.Get()) return std::nullopt;
    vector<vector<RasterNodeId>> edges(_nodes.size());
    vector<uint32_t> incoming(_nodes.size());
    auto edge = [&](RasterNodeId from, RasterNodeId to) {
        if (std::find(edges[from].begin(), edges[from].end(), to) == edges[from].end()) {
            edges[from].push_back(to); ++incoming[to];
        }
    };
    struct Hazard { std::optional<RasterNodeId> Writer; vector<RasterNodeId> Readers; };
    vector<Hazard> hazards(_resources.size());
    for (RasterNodeId i = 0; i < _nodes.size(); ++i) {
        const auto& node = _nodes[i];
        for (auto predecessor : node.After) {
            if (predecessor >= _nodes.size() || predecessor == i) return std::nullopt;
            edge(predecessor, i);
        }
        vector<RasterResourceId> used;
        for (const auto& use : node.Uses) {
            if (use.Resource >= _resources.size() || use.State == render::TextureState::Undefined ||
                std::find(used.begin(), used.end(), use.Resource) != used.end()) return std::nullopt;
            used.push_back(use.Resource);
            auto& hazard = hazards[use.Resource];
            if (hazard.Writer) edge(*hazard.Writer, i);
            if (use.Access == RasterAccess::Read) hazard.Readers.push_back(i);
            else {
                for (auto reader : hazard.Readers) edge(reader, i);
                hazard.Readers.clear(); hazard.Writer = i;
            }
        }
    }
    CompiledRasterGraph result{_resources, _nodes, {}, {}};
    vector<render::TextureStates> states;
    vector<bool> initialized;
    for (const auto& resource : _resources) {
        states.push_back(resource.Enter); initialized.push_back(resource.Initialized);
    }
    vector<bool> scheduled(_nodes.size());
    for (size_t count = 0; count < _nodes.size(); ++count) {
        auto next = _nodes.size();
        for (size_t i = 0; i < _nodes.size(); ++i)
            if (!scheduled[i] && incoming[i] == 0) { next = i; break; }
        if (next == _nodes.size()) return std::nullopt;
        scheduled[next] = true;
        CompiledRasterGraph::Step step{static_cast<RasterNodeId>(next), {}};
        for (const auto& use : _nodes[next].Uses) {
            if (use.Access != RasterAccess::Write && !initialized[use.Resource]) return std::nullopt;
            if (use.Access != RasterAccess::Read) initialized[use.Resource] = true;
            if (states[use.Resource] != use.State) step.Barriers.push_back({
                .Target = _resources[use.Resource].Texture.Get(), .Before = states[use.Resource], .After = use.State});
            states[use.Resource] = use.State;
        }
        result.Steps.push_back(std::move(step));
        for (auto to : edges[next]) --incoming[to];
    }
    for (size_t i = 0; i < _resources.size(); ++i)
        if (states[i] != _resources[i].Exit) result.ExitBarriers.push_back({
            .Target = _resources[i].Texture.Get(), .Before = states[i], .After = _resources[i].Exit});
    return result;
}
bool CompiledRasterGraph::Record(render::CommandBuffer& command) const {
    for (const auto& resource : Resources) if (!resource.Texture) return false;
    for (const auto& node : Nodes) if (!node.Record) return false;
    auto barriers = [&](const auto& values) {
        for (const auto& value : values) {
            const render::ResourceBarrierDescriptor barrier = value;
            command.ResourceBarrier(std::span{&barrier, 1});
        }
    };
    for (const auto& step : Steps) {
        barriers(step.Barriers);
        if (!Nodes[step.Node].Record(command)) return false;
    }
    barriers(ExitBarriers);
    return true;
}
}  // namespace radray
