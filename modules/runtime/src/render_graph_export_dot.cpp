#include <radray/runtime/render_graph.h>

#include <algorithm>

#include <fmt/format.h>

#include <radray/utility.h>

namespace radray {

namespace {

struct DotNodeStyle {
    std::string_view Shape;
    std::string_view FillColor;
};

struct DotQueueStyle {
    std::string_view ClusterColor;
    std::string_view PassFillColor;
    std::string_view SyncFillColor;
};

struct BufferPassUsage {
    RDGPassHandle Pass{};
    uint64_t FirstEdgeIndex{std::numeric_limits<uint64_t>::max()};
    uint64_t LastEdgeIndex{0};
    bool HasWrite{false};
    RDGBufferState FirstState{};
    RDGBufferState LastState{};
};

struct TexturePassUsage {
    RDGPassHandle Pass{};
    uint64_t FirstEdgeIndex{std::numeric_limits<uint64_t>::max()};
    uint64_t LastEdgeIndex{0};
    bool HasWrite{false};
    RDGTextureState FirstState{};
    RDGTextureState LastState{};
};

struct CompiledBufferResource {
    const RDGBufferNode* Node{nullptr};
    vector<BufferPassUsage> PassUsages;
};

struct CompiledTextureResource {
    const RDGTextureNode* Node{nullptr};
    vector<TexturePassUsage> PassUsages;
};

void AppendLine(fmt::memory_buffer& output, std::string_view line) {
    fmt::format_to(std::back_inserter(output), "{}\n", line);
}

void AppendDotEscaped(fmt::memory_buffer& output, std::string_view value) {
    for (char ch : value) {
        switch (ch) {
            case '\\': fmt::format_to(std::back_inserter(output), "\\\\"); break;
            case '"': fmt::format_to(std::back_inserter(output), "\\\""); break;
            case '\n': fmt::format_to(std::back_inserter(output), "\\n"); break;
            case '\r': break;
            default: fmt::format_to(std::back_inserter(output), "{}", ch); break;
        }
    }
}

void AppendNodeId(fmt::memory_buffer& output, uint64_t id) {
    fmt::format_to(std::back_inserter(output), "node_{}", id);
}

DotNodeStyle GetNodeStyle(const RDGNode& node) noexcept {
    if (node.GetTag().HasFlag(RDGNodeTag::Pass)) {
        return DotNodeStyle{"box", "#DBEAFE"};
    }
    if (node.GetTag().HasFlag(RDGNodeTag::Buffer)) {
        return DotNodeStyle{"ellipse", "#DCFCE7"};
    }
    if (node.GetTag().HasFlag(RDGNodeTag::Texture)) {
        return DotNodeStyle{"hexagon", "#FEF3C7"};
    }
    return DotNodeStyle{"diamond", "#E5E7EB"};
}

RDGNodeTag NormalizePassQueueTag(RDGNodeTags tag) noexcept {
    if (tag.HasFlag(RDGNodeTag::CopyPass)) {
        return RDGNodeTag::CopyPass;
    }
    if (tag.HasFlag(RDGNodeTag::ComputePass)) {
        return RDGNodeTag::ComputePass;
    }
    if (tag.HasFlag(RDGNodeTag::GraphicsPass)) {
        return RDGNodeTag::GraphicsPass;
    }
    return RDGNodeTag::UNKNOWN;
}

DotQueueStyle GetExecutionQueueStyle(RDGNodeTags tag) noexcept {
    switch (NormalizePassQueueTag(tag)) {
        case RDGNodeTag::GraphicsPass: return DotQueueStyle{"#93C5FD", "#DBEAFE", "#EFF6FF"};
        case RDGNodeTag::ComputePass: return DotQueueStyle{"#86EFAC", "#DCFCE7", "#F0FDF4"};
        case RDGNodeTag::CopyPass: return DotQueueStyle{"#FCD34D", "#FEF3C7", "#FFFBEB"};
        default: return DotQueueStyle{"#D1D5DB", "#E5E7EB", "#F3F4F6"};
    }
}

void AppendNodeLabel(fmt::memory_buffer& output, const RDGNode& node) {
    AppendDotEscaped(output, node._name);

    if (node.GetTag().HasFlag(RDGNodeTag::Pass)) {
        fmt::format_to(std::back_inserter(output), "\\nqueue={}", NormalizePassQueueTag(node.GetTag()));
    }
}

void AppendEdgeLabel(fmt::memory_buffer& output, const RDGEdge& edge) {
    fmt::format_to(std::back_inserter(output), "{}", RDGExecutionStages{edge._stage});
    if (edge._access != RDGMemoryAccess::NONE) {
        fmt::format_to(std::back_inserter(output), "\\n{}", RDGMemoryAccesses{edge._access});
    }
}

void AppendEdgeEndpointId(fmt::memory_buffer& output, const RDGNode* node, size_t edgeIndex, std::string_view endpoint) {
    if (node != nullptr) {
        AppendNodeId(output, node->_id);
        return;
    }
    fmt::format_to(std::back_inserter(output), "edge_{}_{}_null", edgeIndex, endpoint);
}

void AppendNullEndpointNode(fmt::memory_buffer& output, size_t edgeIndex, std::string_view endpoint) {
    fmt::format_to(
        std::back_inserter(output),
        "  edge_{}_{}_null [shape=diamond, style=filled, fillcolor=\"#FECACA\", fontname=\"Consolas\", label=\"",
        edgeIndex,
        endpoint);
    fmt::format_to(std::back_inserter(output), "null\\nedge={}\\n", edgeIndex);
    AppendDotEscaped(output, endpoint);
    fmt::format_to(std::back_inserter(output), "\"];\n");
}

unordered_map<const RDGEdge*, uint64_t> BuildEdgeIndexByPtr(const vector<unique_ptr<RDGEdge>>& edges) {
    unordered_map<const RDGEdge*, uint64_t> edgeIndexByPtr{};
    edgeIndexByPtr.reserve(edges.size());
    for (uint64_t i = 0; i < edges.size(); ++i) {
        edgeIndexByPtr.emplace(edges[i].get(), i);
    }
    return edgeIndexByPtr;
}

vector<BufferPassUsage> CollectBufferPassUsages(
    const RDGBufferNode& bufferNode,
    const unordered_map<const RDGEdge*, uint64_t>& edgeIndexByPtr) {
    unordered_map<uint64_t, uint32_t> usageIndexByPass{};
    usageIndexByPass.reserve(bufferNode._inEdges.size() + bufferNode._outEdges.size());
    vector<BufferPassUsage> passUsages{};
    passUsages.reserve(bufferNode._inEdges.size() + bufferNode._outEdges.size());

    for (auto* edge : bufferNode._inEdges) {
        RADRAY_ASSERT(edge != nullptr);
        RADRAY_ASSERT(edge->_from != nullptr && edge->_from->GetTag().HasFlag(RDGNodeTag::Pass));
        RADRAY_ASSERT(edge->_to != nullptr && edge->_to->GetTag().HasFlag(RDGNodeTag::Buffer));
        const uint64_t edgeIndex = edgeIndexByPtr.at(edge);
        const uint64_t passId = edge->_from->_id;
        auto it = usageIndexByPass.find(passId);
        if (it == usageIndexByPass.end()) {
            usageIndexByPass.emplace(passId, static_cast<uint32_t>(passUsages.size()));
            passUsages.emplace_back(BufferPassUsage{
                .Pass = RDGPassHandle{passId},
                .FirstEdgeIndex = edgeIndex,
                .LastEdgeIndex = edgeIndex,
                .HasWrite = true,
                .FirstState = RDGBufferState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Range = edge->_bufferRange,
                },
                .LastState = RDGBufferState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Range = edge->_bufferRange,
                },
            });
            continue;
        }

        auto& usage = passUsages[it->second];
        if (edgeIndex < usage.FirstEdgeIndex) {
            usage.FirstEdgeIndex = edgeIndex;
            usage.FirstState = RDGBufferState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Range = edge->_bufferRange,
            };
        }
        if (edgeIndex > usage.LastEdgeIndex) {
            usage.LastEdgeIndex = edgeIndex;
            usage.LastState = RDGBufferState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Range = edge->_bufferRange,
            };
        }
        usage.HasWrite = true;
    }

    for (auto* edge : bufferNode._outEdges) {
        RADRAY_ASSERT(edge != nullptr);
        RADRAY_ASSERT(edge->_from != nullptr && edge->_from->GetTag().HasFlag(RDGNodeTag::Buffer));
        RADRAY_ASSERT(edge->_to != nullptr && edge->_to->GetTag().HasFlag(RDGNodeTag::Pass));
        const uint64_t edgeIndex = edgeIndexByPtr.at(edge);
        const uint64_t passId = edge->_to->_id;
        auto it = usageIndexByPass.find(passId);
        if (it == usageIndexByPass.end()) {
            usageIndexByPass.emplace(passId, static_cast<uint32_t>(passUsages.size()));
            passUsages.emplace_back(BufferPassUsage{
                .Pass = RDGPassHandle{passId},
                .FirstEdgeIndex = edgeIndex,
                .LastEdgeIndex = edgeIndex,
                .HasWrite = false,
                .FirstState = RDGBufferState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Range = edge->_bufferRange,
                },
                .LastState = RDGBufferState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Range = edge->_bufferRange,
                },
            });
            continue;
        }

        auto& usage = passUsages[it->second];
        if (edgeIndex < usage.FirstEdgeIndex) {
            usage.FirstEdgeIndex = edgeIndex;
            usage.FirstState = RDGBufferState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Range = edge->_bufferRange,
            };
        }
        if (edgeIndex > usage.LastEdgeIndex) {
            usage.LastEdgeIndex = edgeIndex;
            usage.LastState = RDGBufferState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Range = edge->_bufferRange,
            };
        }
    }

    std::sort(passUsages.begin(), passUsages.end(), [](const BufferPassUsage& lhs, const BufferPassUsage& rhs) {
        return lhs.Pass.Id < rhs.Pass.Id;
    });
    return passUsages;
}

vector<TexturePassUsage> CollectTexturePassUsages(
    const RDGTextureNode& textureNode,
    const unordered_map<const RDGEdge*, uint64_t>& edgeIndexByPtr) {
    unordered_map<uint64_t, uint32_t> usageIndexByPass{};
    usageIndexByPass.reserve(textureNode._inEdges.size() + textureNode._outEdges.size());
    vector<TexturePassUsage> passUsages{};
    passUsages.reserve(textureNode._inEdges.size() + textureNode._outEdges.size());

    for (auto* edge : textureNode._inEdges) {
        RADRAY_ASSERT(edge != nullptr);
        RADRAY_ASSERT(edge->_from != nullptr && edge->_from->GetTag().HasFlag(RDGNodeTag::Pass));
        RADRAY_ASSERT(edge->_to != nullptr && edge->_to->GetTag().HasFlag(RDGNodeTag::Texture));
        const uint64_t edgeIndex = edgeIndexByPtr.at(edge);
        const uint64_t passId = edge->_from->_id;
        auto it = usageIndexByPass.find(passId);
        if (it == usageIndexByPass.end()) {
            usageIndexByPass.emplace(passId, static_cast<uint32_t>(passUsages.size()));
            passUsages.emplace_back(TexturePassUsage{
                .Pass = RDGPassHandle{passId},
                .FirstEdgeIndex = edgeIndex,
                .LastEdgeIndex = edgeIndex,
                .HasWrite = true,
                .FirstState = RDGTextureState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Layout = edge->_textureLayout,
                    .Range = edge->_textureRange,
                },
                .LastState = RDGTextureState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Layout = edge->_textureLayout,
                    .Range = edge->_textureRange,
                },
            });
            continue;
        }

        auto& usage = passUsages[it->second];
        if (edgeIndex < usage.FirstEdgeIndex) {
            usage.FirstEdgeIndex = edgeIndex;
            usage.FirstState = RDGTextureState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Layout = edge->_textureLayout,
                .Range = edge->_textureRange,
            };
        }
        if (edgeIndex > usage.LastEdgeIndex) {
            usage.LastEdgeIndex = edgeIndex;
            usage.LastState = RDGTextureState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Layout = edge->_textureLayout,
                .Range = edge->_textureRange,
            };
        }
        usage.HasWrite = true;
    }

    for (auto* edge : textureNode._outEdges) {
        RADRAY_ASSERT(edge != nullptr);
        RADRAY_ASSERT(edge->_from != nullptr && edge->_from->GetTag().HasFlag(RDGNodeTag::Texture));
        RADRAY_ASSERT(edge->_to != nullptr && edge->_to->GetTag().HasFlag(RDGNodeTag::Pass));
        const uint64_t edgeIndex = edgeIndexByPtr.at(edge);
        const uint64_t passId = edge->_to->_id;
        auto it = usageIndexByPass.find(passId);
        if (it == usageIndexByPass.end()) {
            usageIndexByPass.emplace(passId, static_cast<uint32_t>(passUsages.size()));
            passUsages.emplace_back(TexturePassUsage{
                .Pass = RDGPassHandle{passId},
                .FirstEdgeIndex = edgeIndex,
                .LastEdgeIndex = edgeIndex,
                .HasWrite = false,
                .FirstState = RDGTextureState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Layout = edge->_textureLayout,
                    .Range = edge->_textureRange,
                },
                .LastState = RDGTextureState{
                    .Stage = edge->_stage,
                    .Access = edge->_access,
                    .Layout = edge->_textureLayout,
                    .Range = edge->_textureRange,
                },
            });
            continue;
        }

        auto& usage = passUsages[it->second];
        if (edgeIndex < usage.FirstEdgeIndex) {
            usage.FirstEdgeIndex = edgeIndex;
            usage.FirstState = RDGTextureState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Layout = edge->_textureLayout,
                .Range = edge->_textureRange,
            };
        }
        if (edgeIndex > usage.LastEdgeIndex) {
            usage.LastEdgeIndex = edgeIndex;
            usage.LastState = RDGTextureState{
                .Stage = edge->_stage,
                .Access = edge->_access,
                .Layout = edge->_textureLayout,
                .Range = edge->_textureRange,
            };
        }
    }

    std::sort(passUsages.begin(), passUsages.end(), [](const TexturePassUsage& lhs, const TexturePassUsage& rhs) {
        return lhs.Pass.Id < rhs.Pass.Id;
    });
    return passUsages;
}

vector<CompiledBufferResource> CollectCompiledBufferResources(
    const vector<unique_ptr<RDGNode>>& nodes,
    const unordered_map<const RDGEdge*, uint64_t>& edgeIndexByPtr) {
    vector<CompiledBufferResource> compiledBuffers{};
    compiledBuffers.reserve(nodes.size());
    for (const auto& nodeHolder : nodes) {
        auto* node = nodeHolder.get();
        if (node == nullptr || !node->GetTag().HasFlag(RDGNodeTag::Buffer)) {
            continue;
        }

        auto* bufferNode = static_cast<RDGBufferNode*>(node);
        compiledBuffers.emplace_back(CompiledBufferResource{
            .Node = bufferNode,
            .PassUsages = CollectBufferPassUsages(*bufferNode, edgeIndexByPtr),
        });
    }
    return compiledBuffers;
}

vector<CompiledTextureResource> CollectCompiledTextureResources(
    const vector<unique_ptr<RDGNode>>& nodes,
    const unordered_map<const RDGEdge*, uint64_t>& edgeIndexByPtr) {
    vector<CompiledTextureResource> compiledTextures{};
    compiledTextures.reserve(nodes.size());
    for (const auto& nodeHolder : nodes) {
        auto* node = nodeHolder.get();
        if (node == nullptr || !node->GetTag().HasFlag(RDGNodeTag::Texture)) {
            continue;
        }

        auto* textureNode = static_cast<RDGTextureNode*>(node);
        compiledTextures.emplace_back(CompiledTextureResource{
            .Node = textureNode,
            .PassUsages = CollectTexturePassUsages(*textureNode, edgeIndexByPtr),
        });
    }
    return compiledTextures;
}

void AppendCompiledPassLabel(fmt::memory_buffer& output, const RDGPassNode& passNode, uint32_t passOrderIndex) {
    AppendDotEscaped(output, passNode._name);
    fmt::format_to(
        std::back_inserter(output),
        "\\norder={}\\nqueue={}",
        passOrderIndex,
        NormalizePassQueueTag(passNode.GetTag()));
}

void AppendCompiledResourceVersionNodeId(fmt::memory_buffer& output, uint64_t resourceId, uint32_t version) {
    fmt::format_to(std::back_inserter(output), "resource_{}_v{}", resourceId, version);
}

void AppendCompiledResourceVersionLabel(fmt::memory_buffer& output, const RDGNode& resourceNode, uint32_t version) {
    AppendDotEscaped(output, resourceNode._name);
    fmt::format_to(std::back_inserter(output), "\\nversion={}", version);
}

void AppendExecutionPassNodeId(fmt::memory_buffer& output, uint64_t passId) {
    fmt::format_to(std::back_inserter(output), "exec_pass_{}", passId);
}

void AppendExecutionSyncNodeId(fmt::memory_buffer& output, uint64_t passId, std::string_view phase) {
    fmt::format_to(std::back_inserter(output), "exec_sync_{}_{}", phase, passId);
}

void AppendCompiledStateLabel(fmt::memory_buffer& output, const RDGBufferState& state) {
    fmt::format_to(std::back_inserter(output), "{}", RDGExecutionStages{state.Stage});
    if (state.Access != RDGMemoryAccess::NONE) {
        fmt::format_to(std::back_inserter(output), "\\n{}", RDGMemoryAccesses{state.Access});
    }
}

void AppendCompiledStateLabel(fmt::memory_buffer& output, const RDGTextureState& state) {
    fmt::format_to(std::back_inserter(output), "{}", RDGExecutionStages{state.Stage});
    if (state.Access != RDGMemoryAccess::NONE) {
        fmt::format_to(std::back_inserter(output), "\\n{}", RDGMemoryAccesses{state.Access});
    }
}

void AppendExecutionStateSummary(fmt::memory_buffer& output, const RDGBufferState& state) {
    fmt::format_to(std::back_inserter(output), "{}", RDGExecutionStages{state.Stage});
    if (state.Access != RDGMemoryAccess::NONE) {
        fmt::format_to(std::back_inserter(output), " {}", RDGMemoryAccesses{state.Access});
    }
}

void AppendExecutionStateSummary(fmt::memory_buffer& output, const RDGTextureState& state) {
    fmt::format_to(std::back_inserter(output), "{}", RDGExecutionStages{state.Stage});
    if (state.Access != RDGMemoryAccess::NONE) {
        fmt::format_to(std::back_inserter(output), " {}", RDGMemoryAccesses{state.Access});
    }
    fmt::format_to(std::back_inserter(output), " {}", format_as(state.Layout));
}

void AppendExecutionBarrierLine(fmt::memory_buffer& output, const RenderGraph& graph, const RDGCompiledBarrier& barrier) {
    if (std::holds_alternative<RDGCompiledBufferBarrier>(barrier)) {
        const auto& bufferBarrier = std::get<RDGCompiledBufferBarrier>(barrier);
        RADRAY_ASSERT(bufferBarrier.Buffer.Id < graph._nodes.size());
        auto* resourceNode = graph._nodes[bufferBarrier.Buffer.Id].get();
        RADRAY_ASSERT(resourceNode != nullptr && resourceNode->GetTag().HasFlag(RDGNodeTag::Buffer));
        AppendDotEscaped(output, resourceNode->_name);
        fmt::format_to(std::back_inserter(output), ": ");
        AppendExecutionStateSummary(output, bufferBarrier.Before);
        fmt::format_to(std::back_inserter(output), " -> ");
        AppendExecutionStateSummary(output, bufferBarrier.After);
        return;
    }

    const auto& textureBarrier = std::get<RDGCompiledTextureBarrier>(barrier);
    RADRAY_ASSERT(textureBarrier.Texture.Id < graph._nodes.size());
    auto* resourceNode = graph._nodes[textureBarrier.Texture.Id].get();
    RADRAY_ASSERT(resourceNode != nullptr && resourceNode->GetTag().HasFlag(RDGNodeTag::Texture));
    AppendDotEscaped(output, resourceNode->_name);
    fmt::format_to(std::back_inserter(output), ": ");
    AppendExecutionStateSummary(output, textureBarrier.Before);
    fmt::format_to(std::back_inserter(output), " -> ");
    AppendExecutionStateSummary(output, textureBarrier.After);
}

void AppendExecutionBarrierLabel(
    fmt::memory_buffer& output,
    const RenderGraph& graph,
    std::string_view phase,
    const RDGPassNode& passNode,
    const vector<RDGCompiledBarrier>& barriers) {
    fmt::format_to(std::back_inserter(output), "sync ");
    AppendDotEscaped(output, phase);
    fmt::format_to(std::back_inserter(output), " ");
    AppendDotEscaped(output, passNode._name);
    for (const auto& barrier : barriers) {
        fmt::format_to(std::back_inserter(output), "\\n");
        AppendExecutionBarrierLine(output, graph, barrier);
    }
}

void AppendExecutionDependencyLabel(
    fmt::memory_buffer& output,
    const RenderGraph& graph,
    const vector<uint64_t>& resourceIds) {
    for (size_t i = 0; i < resourceIds.size(); ++i) {
        if (i != 0) {
            fmt::format_to(std::back_inserter(output), "\\n");
        }
        RADRAY_ASSERT(resourceIds[i] < graph._nodes.size());
        auto* resourceNode = graph._nodes[resourceIds[i]].get();
        RADRAY_ASSERT(resourceNode != nullptr && resourceNode->GetTag().HasFlag(RDGNodeTag::Resource));
        AppendDotEscaped(output, resourceNode->_name);
    }
}

}  // namespace

string RenderGraph::ExportGraphviz() const {
    fmt::memory_buffer dot{};

    AppendLine(dot, "digraph RenderGraph {");
    AppendLine(dot, "  rankdir=LR;");
    AppendLine(dot, "  graph [fontname=\"Consolas\"];");
    AppendLine(dot, "  node [fontname=\"Consolas\"];");
    AppendLine(dot, "  edge [fontname=\"Consolas\"];");

    for (size_t i = 0; i < _nodes.size(); ++i) {
        const auto* node = _nodes[i].get();
        if (node == nullptr) {
            fmt::format_to(std::back_inserter(dot), "  ");
            AppendNodeId(dot, i);
            fmt::format_to(
                std::back_inserter(dot),
                " [shape=diamond, style=filled, fillcolor=\"#E5E7EB\", label=\"<null>");
            fmt::format_to(std::back_inserter(dot), "\"];\n");
            continue;
        }

        const auto style = GetNodeStyle(*node);
        fmt::format_to(std::back_inserter(dot), "  ");
        AppendNodeId(dot, node->_id);
        fmt::format_to(
            std::back_inserter(dot),
            " [shape={}, style=filled, fillcolor=\"{}\", label=\"",
            style.Shape,
            style.FillColor);
        AppendNodeLabel(dot, *node);
        fmt::format_to(std::back_inserter(dot), "\"];\n");
    }

    for (size_t i = 0; i < _edges.size(); ++i) {
        const auto* edge = _edges[i].get();
        if (edge == nullptr) {
            continue;
        }

        if (edge->_from == nullptr) {
            AppendNullEndpointNode(dot, i, "from");
        }
        if (edge->_to == nullptr) {
            AppendNullEndpointNode(dot, i, "to");
        }

        fmt::format_to(std::back_inserter(dot), "  ");
        AppendEdgeEndpointId(dot, edge->_from, i, "from");
        fmt::format_to(std::back_inserter(dot), " -> ");
        AppendEdgeEndpointId(dot, edge->_to, i, "to");
        fmt::format_to(std::back_inserter(dot), " [label=\"");
        AppendEdgeLabel(dot, *edge);
        fmt::format_to(std::back_inserter(dot), "\", color=\"#64748B\"];\n");
    }

    AppendLine(dot, "}");
    return fmt::to_string(dot);
}

string RenderGraph::ExportCompiledGraphviz(const RDGCompileResult& compiled) const {
    fmt::memory_buffer dot{};

    unordered_map<uint64_t, uint32_t> passOrderIndexById{};
    passOrderIndexById.reserve(compiled.PassOrder.size());
    for (uint32_t i = 0; i < compiled.PassOrder.size(); ++i) {
        passOrderIndexById.emplace(compiled.PassOrder[i].Id, i);
    }

    const auto edgeIndexByPtr = BuildEdgeIndexByPtr(_edges);
    auto compiledBuffers = CollectCompiledBufferResources(_nodes, edgeIndexByPtr);
    auto compiledTextures = CollectCompiledTextureResources(_nodes, edgeIndexByPtr);
    for (auto& compiledBuffer : compiledBuffers) {
        std::sort(compiledBuffer.PassUsages.begin(), compiledBuffer.PassUsages.end(), [&](const BufferPassUsage& lhs, const BufferPassUsage& rhs) {
            return passOrderIndexById.at(lhs.Pass.Id) < passOrderIndexById.at(rhs.Pass.Id);
        });
    }
    for (auto& compiledTexture : compiledTextures) {
        std::sort(compiledTexture.PassUsages.begin(), compiledTexture.PassUsages.end(), [&](const TexturePassUsage& lhs, const TexturePassUsage& rhs) {
            return passOrderIndexById.at(lhs.Pass.Id) < passOrderIndexById.at(rhs.Pass.Id);
        });
    }

    AppendLine(dot, "digraph CompiledRenderGraph {");
    AppendLine(dot, "  rankdir=LR;");
    AppendLine(dot, "  graph [fontname=\"Consolas\"];");
    AppendLine(dot, "  node [fontname=\"Consolas\"];");
    AppendLine(dot, "  edge [fontname=\"Consolas\"];");

    for (uint32_t i = 0; i < compiled.PassOrder.size(); ++i) {
        const auto passHandle = compiled.PassOrder[i];
        RADRAY_ASSERT(passHandle.Id < _nodes.size());
        const auto* node = _nodes[passHandle.Id].get();
        RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Pass));
        const auto& passNode = static_cast<const RDGPassNode&>(*node);
        const auto style = GetNodeStyle(passNode);
        fmt::format_to(std::back_inserter(dot), "  ");
        AppendNodeId(dot, passHandle.Id);
        fmt::format_to(
            std::back_inserter(dot),
            " [shape={}, style=filled, fillcolor=\"{}\", label=\"",
            style.Shape,
            style.FillColor);
        AppendCompiledPassLabel(dot, passNode, i);
        fmt::format_to(std::back_inserter(dot), "\"];\n");
    }

    const auto appendResourceVersions = [&](const RDGNode& resourceNode, const auto& passUsages) {
        uint32_t versionCount = 0;
        for (const auto& usage : passUsages) {
            if (usage.HasWrite) {
                versionCount += 1;
            }
        }

        const auto style = GetNodeStyle(resourceNode);
        for (uint32_t version = 0; version <= versionCount; ++version) {
            fmt::format_to(std::back_inserter(dot), "  ");
            AppendCompiledResourceVersionNodeId(dot, resourceNode._id, version);
            fmt::format_to(
                std::back_inserter(dot),
                " [shape={}, style=filled, fillcolor=\"{}\", label=\"",
                style.Shape,
                style.FillColor);
            AppendCompiledResourceVersionLabel(dot, resourceNode, version);
            fmt::format_to(std::back_inserter(dot), "\"];\n");
        }
    };

    for (const auto& compiledBuffer : compiledBuffers) {
        appendResourceVersions(*compiledBuffer.Node, compiledBuffer.PassUsages);
    }
    for (const auto& compiledTexture : compiledTextures) {
        appendResourceVersions(*compiledTexture.Node, compiledTexture.PassUsages);
    }

    for (const auto& compiledBuffer : compiledBuffers) {
        uint32_t currentVersion = 0;
        for (const auto& usage : compiledBuffer.PassUsages) {
            fmt::format_to(std::back_inserter(dot), "  ");
            AppendCompiledResourceVersionNodeId(dot, compiledBuffer.Node->_id, currentVersion);
            fmt::format_to(std::back_inserter(dot), " -> ");
            AppendNodeId(dot, usage.Pass.Id);
            fmt::format_to(std::back_inserter(dot), " [label=\"");
            AppendCompiledStateLabel(dot, usage.FirstState);
            fmt::format_to(std::back_inserter(dot), "\", color=\"#64748B\"];\n");

            if (!usage.HasWrite) {
                continue;
            }

            fmt::format_to(std::back_inserter(dot), "  ");
            AppendNodeId(dot, usage.Pass.Id);
            fmt::format_to(std::back_inserter(dot), " -> ");
            AppendCompiledResourceVersionNodeId(dot, compiledBuffer.Node->_id, currentVersion + 1);
            fmt::format_to(std::back_inserter(dot), " [label=\"");
            AppendCompiledStateLabel(dot, usage.LastState);
            fmt::format_to(std::back_inserter(dot), "\", color=\"#64748B\"];\n");
            currentVersion += 1;
        }
    }

    for (const auto& compiledTexture : compiledTextures) {
        uint32_t currentVersion = 0;
        for (const auto& usage : compiledTexture.PassUsages) {
            fmt::format_to(std::back_inserter(dot), "  ");
            AppendCompiledResourceVersionNodeId(dot, compiledTexture.Node->_id, currentVersion);
            fmt::format_to(std::back_inserter(dot), " -> ");
            AppendNodeId(dot, usage.Pass.Id);
            fmt::format_to(std::back_inserter(dot), " [label=\"");
            AppendCompiledStateLabel(dot, usage.FirstState);
            fmt::format_to(std::back_inserter(dot), "\", color=\"#64748B\"];\n");

            if (!usage.HasWrite) {
                continue;
            }

            fmt::format_to(std::back_inserter(dot), "  ");
            AppendNodeId(dot, usage.Pass.Id);
            fmt::format_to(std::back_inserter(dot), " -> ");
            AppendCompiledResourceVersionNodeId(dot, compiledTexture.Node->_id, currentVersion + 1);
            fmt::format_to(std::back_inserter(dot), " [label=\"");
            AppendCompiledStateLabel(dot, usage.LastState);
            fmt::format_to(std::back_inserter(dot), "\", color=\"#64748B\"];\n");
            currentVersion += 1;
        }
    }

    AppendLine(dot, "}");
    return fmt::to_string(dot);
}

string RenderGraph::ExportExecutionGraphviz(const RDGCompileResult& compiled) const {
    fmt::memory_buffer dot{};

    unordered_map<uint64_t, uint32_t> compiledPassIndexById{};
    compiledPassIndexById.reserve(compiled.Passes.size());
    for (uint32_t i = 0; i < compiled.Passes.size(); ++i) {
        compiledPassIndexById.emplace(compiled.Passes[i].Pass.Id, i);
    }

    unordered_map<uint64_t, uint32_t> passLevelById{};
    passLevelById.reserve(compiled.Passes.size());
    uint32_t maxLevel = 0;
    for (const auto& compiledPass : compiled.Passes) {
        uint32_t level = 0;
        for (const auto& predecessor : compiledPass.Predecessors) {
            level = std::max(level, passLevelById.at(predecessor.Id) + 1);
        }
        passLevelById.emplace(compiledPass.Pass.Id, level);
        maxLevel = std::max(maxLevel, level);
    }

    vector<vector<uint64_t>> passIdsByLevel(maxLevel + 1);
    vector<uint64_t> directPassIds{};
    vector<uint64_t> computePassIds{};
    vector<uint64_t> copyPassIds{};
    directPassIds.reserve(compiled.PassOrder.size());
    computePassIds.reserve(compiled.PassOrder.size());
    copyPassIds.reserve(compiled.PassOrder.size());
    for (const auto& passHandle : compiled.PassOrder) {
        passIdsByLevel[passLevelById.at(passHandle.Id)].emplace_back(passHandle.Id);

        RADRAY_ASSERT(passHandle.Id < _nodes.size());
        auto* node = _nodes[passHandle.Id].get();
        RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Pass));
        const auto* passNode = static_cast<const RDGPassNode*>(node);
        switch (NormalizePassQueueTag(passNode->GetTag())) {
            case RDGNodeTag::GraphicsPass: directPassIds.emplace_back(passHandle.Id); break;
            case RDGNodeTag::ComputePass: computePassIds.emplace_back(passHandle.Id); break;
            case RDGNodeTag::CopyPass: copyPassIds.emplace_back(passHandle.Id); break;
            default: directPassIds.emplace_back(passHandle.Id); break;
        }
    }

    const uint64_t dependencyKeyStride = _nodes.size() + 1;
    unordered_map<uint64_t, vector<uint64_t>> dependencyResourceIdsByKey{};
    dependencyResourceIdsByKey.reserve(compiled.Dependencies.size());
    for (const auto& dependency : compiled.Dependencies) {
        const uint64_t key = dependency.Before.Id * dependencyKeyStride + dependency.After.Id;
        auto& resourceIds = dependencyResourceIdsByKey[key];
        bool found = false;
        for (const auto resourceId : resourceIds) {
            if (resourceId == dependency.Resource.Id) {
                found = true;
                break;
            }
        }
        if (!found) {
            resourceIds.emplace_back(dependency.Resource.Id);
        }
    }

    const auto appendExecutionHeadNodeId = [&](fmt::memory_buffer& output, uint64_t passId) {
        const auto& compiledPass = compiled.Passes[compiledPassIndexById.at(passId)];
        if (!compiledPass.BarriersBefore.empty()) {
            AppendExecutionSyncNodeId(output, passId, "before");
            return;
        }
        AppendExecutionPassNodeId(output, passId);
    };
    const auto appendExecutionTailNodeId = [&](fmt::memory_buffer& output, uint64_t passId) {
        const auto& compiledPass = compiled.Passes[compiledPassIndexById.at(passId)];
        if (!compiledPass.BarriersAfter.empty()) {
            AppendExecutionSyncNodeId(output, passId, "after");
            return;
        }
        AppendExecutionPassNodeId(output, passId);
    };

    AppendLine(dot, "digraph CompiledExecutionGraph {");
    AppendLine(dot, "  rankdir=LR;");
    AppendLine(dot, "  newrank=true;");
    AppendLine(dot, "  compound=true;");
    AppendLine(dot, "  graph [fontname=\"Consolas\"];");
    AppendLine(dot, "  node [fontname=\"Consolas\"];");
    AppendLine(dot, "  edge [fontname=\"Consolas\"];");

    const auto appendQueueCluster = [&](RDGNodeTag queueTag, const vector<uint64_t>& passIds) {
        if (passIds.empty()) {
            return;
        }

        const auto queueName = queueTag;
        const auto style = GetExecutionQueueStyle(queueTag);
        fmt::format_to(std::back_inserter(dot), "  subgraph cluster_exec_{} {{\n", queueName);
        fmt::format_to(std::back_inserter(dot), "    label=\"{} Queue\";\n", queueName);
        fmt::format_to(std::back_inserter(dot), "    color=\"{}\";\n", style.ClusterColor);
        AppendLine(dot, "    style=rounded;");

        for (const auto passId : passIds) {
            const auto& compiledPass = compiled.Passes[compiledPassIndexById.at(passId)];
            RADRAY_ASSERT(passId < _nodes.size());
            auto* node = _nodes[passId].get();
            RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Pass));
            const auto& passNode = static_cast<const RDGPassNode&>(*node);
            const auto passOrderIndex = compiledPassIndexById.at(passId);
            const auto passLevel = passLevelById.at(passId);

            if (!compiledPass.BarriersBefore.empty()) {
                fmt::format_to(std::back_inserter(dot), "    ");
                AppendExecutionSyncNodeId(dot, passId, "before");
                fmt::format_to(
                    std::back_inserter(dot),
                    " [shape=note, style=filled, fillcolor=\"{}\", label=\"",
                    style.SyncFillColor);
                AppendExecutionBarrierLabel(dot, *this, "before", passNode, compiledPass.BarriersBefore);
                fmt::format_to(std::back_inserter(dot), "\"];\n");
            }

            fmt::format_to(std::back_inserter(dot), "    ");
            AppendExecutionPassNodeId(dot, passId);
            fmt::format_to(
                std::back_inserter(dot),
                " [shape=box, style=\"rounded,filled\", fillcolor=\"{}\", label=\"",
                style.PassFillColor);
            AppendDotEscaped(dot, passNode._name);
            fmt::format_to(
                std::back_inserter(dot),
                "\\norder={}\\nlevel={}\\nqueue={}",
                passOrderIndex,
                passLevel,
                queueName);
            fmt::format_to(std::back_inserter(dot), "\"];\n");

            if (!compiledPass.BarriersAfter.empty()) {
                fmt::format_to(std::back_inserter(dot), "    ");
                AppendExecutionSyncNodeId(dot, passId, "after");
                fmt::format_to(
                    std::back_inserter(dot),
                    " [shape=note, style=filled, fillcolor=\"{}\", label=\"",
                    style.SyncFillColor);
                AppendExecutionBarrierLabel(dot, *this, "after", passNode, compiledPass.BarriersAfter);
                fmt::format_to(std::back_inserter(dot), "\"];\n");
            }
        }

        AppendLine(dot, "  }");
    };

    appendQueueCluster(RDGNodeTag::GraphicsPass, directPassIds);
    appendQueueCluster(RDGNodeTag::ComputePass, computePassIds);
    appendQueueCluster(RDGNodeTag::CopyPass, copyPassIds);

    for (const auto& compiledPass : compiled.Passes) {
        const auto passId = compiledPass.Pass.Id;
        if (!compiledPass.BarriersBefore.empty()) {
            fmt::format_to(std::back_inserter(dot), "  ");
            AppendExecutionSyncNodeId(dot, passId, "before");
            fmt::format_to(std::back_inserter(dot), " -> ");
            AppendExecutionPassNodeId(dot, passId);
            fmt::format_to(std::back_inserter(dot), " [color=\"#475569\", penwidth=1.5];\n");
        }
        if (!compiledPass.BarriersAfter.empty()) {
            fmt::format_to(std::back_inserter(dot), "  ");
            AppendExecutionPassNodeId(dot, passId);
            fmt::format_to(std::back_inserter(dot), " -> ");
            AppendExecutionSyncNodeId(dot, passId, "after");
            fmt::format_to(std::back_inserter(dot), " [color=\"#475569\", penwidth=1.5];\n");
        }
    }

    const auto appendQueueEdges = [&](RDGNodeTag queueTag, const vector<uint64_t>& passIds) {
        if (passIds.size() < 2) {
            return;
        }

        const auto style = GetExecutionQueueStyle(queueTag);
        for (size_t i = 1; i < passIds.size(); ++i) {
            fmt::format_to(std::back_inserter(dot), "  ");
            appendExecutionTailNodeId(dot, passIds[i - 1]);
            fmt::format_to(std::back_inserter(dot), " -> ");
            appendExecutionHeadNodeId(dot, passIds[i]);
            fmt::format_to(
                std::back_inserter(dot),
                " [color=\"{}\", penwidth=2.0];\n",
                style.ClusterColor);
        }
    };

    appendQueueEdges(RDGNodeTag::GraphicsPass, directPassIds);
    appendQueueEdges(RDGNodeTag::ComputePass, computePassIds);
    appendQueueEdges(RDGNodeTag::CopyPass, copyPassIds);

    for (const auto& compiledPass : compiled.Passes) {
        RADRAY_ASSERT(compiledPass.Pass.Id < _nodes.size());
        auto* node = _nodes[compiledPass.Pass.Id].get();
        RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Pass));
        const auto* passNode = static_cast<const RDGPassNode*>(node);

        for (const auto& predecessor : compiledPass.Predecessors) {
            RADRAY_ASSERT(predecessor.Id < _nodes.size());
            auto* predecessorNodeBase = _nodes[predecessor.Id].get();
            RADRAY_ASSERT(predecessorNodeBase != nullptr && predecessorNodeBase->GetTag().HasFlag(RDGNodeTag::Pass));
            const auto* predecessorNode = static_cast<const RDGPassNode*>(predecessorNodeBase);
            if (NormalizePassQueueTag(predecessorNode->GetTag()) == NormalizePassQueueTag(passNode->GetTag())) {
                continue;
            }

            const uint64_t key = predecessor.Id * dependencyKeyStride + compiledPass.Pass.Id;
            fmt::format_to(std::back_inserter(dot), "  ");
            appendExecutionTailNodeId(dot, predecessor.Id);
            fmt::format_to(std::back_inserter(dot), " -> ");
            appendExecutionHeadNodeId(dot, compiledPass.Pass.Id);
            fmt::format_to(std::back_inserter(dot), " [style=dashed, color=\"#64748B\", label=\"");
            const auto it = dependencyResourceIdsByKey.find(key);
            if (it != dependencyResourceIdsByKey.end()) {
                AppendExecutionDependencyLabel(dot, *this, it->second);
            }
            fmt::format_to(std::back_inserter(dot), "\"];\n");
        }
    }

    for (const auto& passIds : passIdsByLevel) {
        if (passIds.empty()) {
            continue;
        }

        fmt::format_to(std::back_inserter(dot), "  {{ rank=same; ");
        for (const auto passId : passIds) {
            AppendExecutionPassNodeId(dot, passId);
            fmt::format_to(std::back_inserter(dot), "; ");
        }
        fmt::format_to(std::back_inserter(dot), "}}\n");
    }

    AppendLine(dot, "}");
    return fmt::to_string(dot);
}

}  // namespace radray
