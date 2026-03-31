#include <radray/runtime/render_graph.h>

#include <algorithm>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_helper.h>
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_helper.h>
#include <radray/render/backend/vulkan_impl.h>
#endif

#include <fmt/format.h>

#include <radray/logger.h>
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

class PersistentResourceCleanup {
public:
    explicit PersistentResourceCleanup(GpuRuntime* runtime) noexcept
        : _runtime(runtime) {}

    ~PersistentResourceCleanup() noexcept {
        if (!_enabled || _runtime == nullptr) {
            return;
        }
        for (auto it = _handles.rbegin(); it != _handles.rend(); ++it) {
            if (!it->IsValid()) {
                continue;
            }
            try {
                _runtime->DestroyResourceImmediate(*it);
            } catch (...) {
            }
        }
    }

    void Track(GpuResourceHandle handle) {
        _handles.emplace_back(handle);
    }

    void Release() noexcept {
        _enabled = false;
    }

private:
    GpuRuntime* _runtime{nullptr};
    vector<GpuResourceHandle> _handles{};
    bool _enabled{true};
};

const RDGBufferNode& GetBufferNode(const RenderGraph& graph, RDGBufferHandle handle) {
    RADRAY_ASSERT(handle.IsValid());
    RADRAY_ASSERT(handle.Id < graph._nodes.size());
    auto* node = graph._nodes[handle.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Buffer));
    return static_cast<const RDGBufferNode&>(*node);
}

const RDGTextureNode& GetTextureNode(const RenderGraph& graph, RDGTextureHandle handle) {
    RADRAY_ASSERT(handle.IsValid());
    RADRAY_ASSERT(handle.Id < graph._nodes.size());
    auto* node = graph._nodes[handle.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Texture));
    return static_cast<const RDGTextureNode&>(*node);
}

const RDGPassNode& GetPassNode(const RenderGraph& graph, RDGPassHandle handle) {
    RADRAY_ASSERT(handle.IsValid());
    RADRAY_ASSERT(handle.Id < graph._nodes.size());
    auto* node = graph._nodes[handle.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Pass));
    return static_cast<const RDGPassNode&>(*node);
}

render::Buffer* ResolveBufferHandle(GpuBufferHandle handle) {
    RADRAY_ASSERT(handle.IsValid() && handle.NativeHandle != nullptr);
    auto* buffer = static_cast<render::Buffer*>(handle.NativeHandle);
    RADRAY_ASSERT(buffer != nullptr);
    return buffer;
}

render::Texture* ResolveTextureHandle(GpuTextureHandle handle) {
    RADRAY_ASSERT(handle.IsValid() && handle.NativeHandle != nullptr);
    auto* texture = static_cast<render::Texture*>(handle.NativeHandle);
    RADRAY_ASSERT(texture != nullptr);
    return texture;
}

GpuBufferHandle LookupBufferHandle(
    const unordered_map<uint64_t, GpuBufferHandle>& handles,
    RDGBufferHandle handle) {
    const auto it = handles.find(handle.Id);
    if (it == handles.end()) {
        RADRAY_ABORT("RenderGraph::Execute missing resolved buffer handle for node {}", handle.Id);
    }
    return it->second;
}

GpuTextureHandle LookupTextureHandle(
    const unordered_map<uint64_t, GpuTextureHandle>& handles,
    RDGTextureHandle handle) {
    const auto it = handles.find(handle.Id);
    if (it == handles.end()) {
        RADRAY_ABORT("RenderGraph::Execute missing resolved texture handle for node {}", handle.Id);
    }
    return it->second;
}

RDGMemoryAccesses CollectBufferAccessFlags(const RDGBufferNode& node) {
    RDGMemoryAccesses accesses{};
    if (node._importedState.has_value()) {
        accesses |= node._importedState->Access;
    }
    if (node._exportedState.has_value()) {
        accesses |= node._exportedState->Access;
    }
    for (const auto* edge : node._inEdges) {
        RADRAY_ASSERT(edge != nullptr);
        accesses |= edge->_access;
    }
    for (const auto* edge : node._outEdges) {
        RADRAY_ASSERT(edge != nullptr);
        accesses |= edge->_access;
    }
    return accesses;
}

void AccumulateBufferUsageFlags(render::BufferUses& usage, RDGMemoryAccess access) {
    const RDGMemoryAccesses accesses{access};
    if (accesses.HasFlag(RDGMemoryAccess::TransferRead)) {
        usage |= render::BufferUse::CopySource;
    }
    if (accesses.HasFlag(RDGMemoryAccess::TransferWrite)) {
        usage |= render::BufferUse::CopyDestination;
    }
    if (accesses.HasFlag(RDGMemoryAccess::VertexRead)) {
        usage |= render::BufferUse::Vertex;
    }
    if (accesses.HasFlag(RDGMemoryAccess::IndexRead)) {
        usage |= render::BufferUse::Index;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ConstantRead)) {
        usage |= render::BufferUse::CBuffer;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ShaderRead)) {
        usage |= render::BufferUse::Resource;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ShaderWrite)) {
        usage |= render::BufferUse::UnorderedAccess;
    }
    if (accesses.HasFlag(RDGMemoryAccess::IndirectRead)) {
        usage |= render::BufferUse::Indirect;
    }
    if (accesses.HasFlag(RDGMemoryAccess::HostRead)) {
        usage |= render::BufferUse::MapRead;
    }
    if (accesses.HasFlag(RDGMemoryAccess::HostWrite)) {
        usage |= render::BufferUse::MapWrite;
    }
}

void AccumulateTextureUsageFlags(render::TextureUses& usage, RDGMemoryAccess access) {
    const RDGMemoryAccesses accesses{access};
    if (accesses.HasFlag(RDGMemoryAccess::TransferRead)) {
        usage |= render::TextureUse::CopySource;
    }
    if (accesses.HasFlag(RDGMemoryAccess::TransferWrite)) {
        usage |= render::TextureUse::CopyDestination;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ShaderRead)) {
        usage |= render::TextureUse::Resource;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ColorAttachmentRead) ||
        accesses.HasFlag(RDGMemoryAccess::ColorAttachmentWrite)) {
        usage |= render::TextureUse::RenderTarget;
    }
    if (accesses.HasFlag(RDGMemoryAccess::DepthStencilRead)) {
        usage |= render::TextureUse::DepthStencilRead;
    }
    if (accesses.HasFlag(RDGMemoryAccess::DepthStencilWrite)) {
        usage |= render::TextureUse::DepthStencilWrite;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ShaderWrite)) {
        usage |= render::TextureUse::UnorderedAccess;
    }
}

render::BufferUses CollectBufferUsageFlags(const RDGBufferNode& node) {
    render::BufferUses usage{};
    if (node._importedState.has_value()) {
        AccumulateBufferUsageFlags(usage, node._importedState->Access);
    }
    if (node._exportedState.has_value()) {
        AccumulateBufferUsageFlags(usage, node._exportedState->Access);
    }
    for (const auto* edge : node._inEdges) {
        RADRAY_ASSERT(edge != nullptr);
        AccumulateBufferUsageFlags(usage, edge->_access);
    }
    for (const auto* edge : node._outEdges) {
        RADRAY_ASSERT(edge != nullptr);
        AccumulateBufferUsageFlags(usage, edge->_access);
    }
    return usage;
}

render::TextureUses CollectTextureUsageFlags(const RDGTextureNode& node) {
    render::TextureUses usage{};
    if (node._importedState.has_value()) {
        AccumulateTextureUsageFlags(usage, node._importedState->Access);
    }
    if (node._exportedState.has_value()) {
        AccumulateTextureUsageFlags(usage, node._exportedState->Access);
    }
    for (const auto* edge : node._inEdges) {
        RADRAY_ASSERT(edge != nullptr);
        AccumulateTextureUsageFlags(usage, edge->_access);
    }
    for (const auto* edge : node._outEdges) {
        RADRAY_ASSERT(edge != nullptr);
        AccumulateTextureUsageFlags(usage, edge->_access);
    }
    return usage;
}

render::MemoryType InferBufferMemoryType(const RDGBufferNode& node) {
    const auto usage = CollectBufferUsageFlags(node);
    const auto accesses = CollectBufferAccessFlags(node);
    if (node._exportedState.has_value() && node._exportedState->Access == RDGMemoryAccess::HostRead) {
        constexpr uint32_t readBackCompatibleMask =
            static_cast<uint32_t>(render::BufferUse::MapRead) |
            static_cast<uint32_t>(render::BufferUse::CopyDestination);
        if ((usage.value() & ~readBackCompatibleMask) == 0) {
            return render::MemoryType::ReadBack;
        }
    }
    const bool hasHostWrite = accesses.HasFlag(RDGMemoryAccess::HostWrite);
    const bool hasGpuWrite =
        accesses.HasFlag(RDGMemoryAccess::TransferWrite) ||
        accesses.HasFlag(RDGMemoryAccess::ShaderWrite);
    if (hasHostWrite && !hasGpuWrite) {
        return render::MemoryType::Upload;
    }
    return render::MemoryType::Device;
}

render::MemoryType InferTextureMemoryType(const RDGTextureNode&) {
    return render::MemoryType::Device;
}

render::BufferDescriptor BuildBufferDescriptor(const RDGBufferNode& node) {
    auto usage = CollectBufferUsageFlags(node);
    const auto memory = InferBufferMemoryType(node);
    if (usage == render::BufferUse::UNKNOWN) {
        if (memory == render::MemoryType::ReadBack) {
            usage |= render::BufferUse::MapRead;
        } else if (memory == render::MemoryType::Upload) {
            usage |= render::BufferUse::MapWrite;
        } else {
            usage = render::BufferUse::CopySource | render::BufferUse::CopyDestination;
        }
    }
    return render::BufferDescriptor{
        .Size = node._size,
        .Memory = memory,
        .Usage = usage,
        .Hints = render::ResourceHint::None,
    };
}

render::TextureDescriptor BuildTextureDescriptor(const RDGTextureNode& node) {
    auto usage = CollectTextureUsageFlags(node);
    if (usage == render::TextureUse::UNKNOWN) {
        usage = render::TextureUse::CopySource | render::TextureUse::CopyDestination;
    }
    return render::TextureDescriptor{
        .Dim = node._dim,
        .Width = node._width,
        .Height = node._height,
        .DepthOrArraySize = node._depthOrArraySize,
        .MipLevels = node._mipLevels,
        .SampleCount = node._sampleCount,
        .Format = node._format,
        .Memory = InferTextureMemoryType(node),
        .Usage = usage,
        .Hints = render::ResourceHint::None,
    };
}

render::BufferStates MapRDGBufferStateToRenderState(const RDGBufferState& state) {
    const RDGMemoryAccesses accesses{state.Access};
    render::BufferStates mapped{};
    if (accesses.HasFlag(RDGMemoryAccess::TransferRead)) {
        mapped |= render::BufferState::CopySource;
    }
    if (accesses.HasFlag(RDGMemoryAccess::TransferWrite)) {
        mapped |= render::BufferState::CopyDestination;
    }
    if (accesses.HasFlag(RDGMemoryAccess::VertexRead)) {
        mapped |= render::BufferState::Vertex;
    }
    if (accesses.HasFlag(RDGMemoryAccess::IndexRead)) {
        mapped |= render::BufferState::Index;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ConstantRead)) {
        mapped |= render::BufferState::CBuffer;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ShaderWrite)) {
        mapped |= render::BufferState::UnorderedAccess;
    } else if (accesses.HasFlag(RDGMemoryAccess::ShaderRead)) {
        mapped |= render::BufferState::ShaderRead;
    }
    if (accesses.HasFlag(RDGMemoryAccess::IndirectRead)) {
        mapped |= render::BufferState::Indirect;
    }
    if (accesses.HasFlag(RDGMemoryAccess::HostRead)) {
        mapped |= render::BufferState::HostRead;
    }
    if (accesses.HasFlag(RDGMemoryAccess::HostWrite)) {
        mapped |= render::BufferState::HostWrite;
    }
    if (mapped == render::BufferState::UNKNOWN) {
        return render::BufferState::Common;
    }
    return mapped;
}

render::TextureStates MapRDGTextureStateToRenderState(const RDGTextureState& state) {
    const RDGMemoryAccesses accesses{state.Access};
    render::TextureStates mapped{};
    switch (state.Layout) {
        case RDGTextureLayout::Undefined:
            return render::TextureState::Undefined;
        case RDGTextureLayout::Present:
            return render::TextureState::Present;
        case RDGTextureLayout::TransferSource:
            mapped |= render::TextureState::CopySource;
            break;
        case RDGTextureLayout::TransferDestination:
            mapped |= render::TextureState::CopyDestination;
            break;
        case RDGTextureLayout::ShaderReadOnly:
            mapped |= render::TextureState::ShaderRead;
            break;
        case RDGTextureLayout::ColorAttachment:
            mapped |= render::TextureState::RenderTarget;
            break;
        case RDGTextureLayout::DepthStencilReadOnly:
            mapped |= render::TextureState::DepthRead;
            break;
        case RDGTextureLayout::DepthStencilAttachment:
            mapped |= render::TextureState::DepthWrite;
            break;
        case RDGTextureLayout::General:
            if (accesses.HasFlag(RDGMemoryAccess::ShaderWrite)) {
                mapped |= render::TextureState::UnorderedAccess;
            } else if (accesses.HasFlag(RDGMemoryAccess::ShaderRead)) {
                mapped |= render::TextureState::ShaderRead;
            } else {
                mapped |= render::TextureState::Common;
            }
            break;
        case RDGTextureLayout::UNKNOWN:
            break;
    }
    if (mapped != render::TextureState::UNKNOWN) {
        return mapped;
    }
    if (accesses.HasFlag(RDGMemoryAccess::TransferRead)) {
        mapped |= render::TextureState::CopySource;
    }
    if (accesses.HasFlag(RDGMemoryAccess::TransferWrite)) {
        mapped |= render::TextureState::CopyDestination;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ColorAttachmentRead) ||
        accesses.HasFlag(RDGMemoryAccess::ColorAttachmentWrite)) {
        mapped |= render::TextureState::RenderTarget;
    }
    if (accesses.HasFlag(RDGMemoryAccess::DepthStencilWrite)) {
        mapped |= render::TextureState::DepthWrite;
    } else if (accesses.HasFlag(RDGMemoryAccess::DepthStencilRead)) {
        mapped |= render::TextureState::DepthRead;
    }
    if (accesses.HasFlag(RDGMemoryAccess::ShaderWrite)) {
        mapped |= render::TextureState::UnorderedAccess;
    } else if (accesses.HasFlag(RDGMemoryAccess::ShaderRead)) {
        mapped |= render::TextureState::ShaderRead;
    }
    if (mapped == render::TextureState::UNKNOWN) {
        return render::TextureState::Common;
    }
    return mapped;
}

uint32_t GetTextureArrayLayerCount(const render::TextureDescriptor& desc) {
    switch (desc.Dim) {
        case render::TextureDimension::Dim1DArray:
        case render::TextureDimension::Dim2DArray:
        case render::TextureDimension::Cube:
        case render::TextureDimension::CubeArray:
            return desc.DepthOrArraySize;
        default:
            return 1;
    }
}

render::SubresourceRange ResolveTextureRange(
    const render::TextureDescriptor& desc,
    render::SubresourceRange range) {
    const uint32_t layerCount = GetTextureArrayLayerCount(desc);
    if (range.ArrayLayerCount == render::SubresourceRange::All) {
        range.ArrayLayerCount = layerCount - range.BaseArrayLayer;
    }
    if (range.MipLevelCount == render::SubresourceRange::All) {
        range.MipLevelCount = desc.MipLevels - range.BaseMipLevel;
    }
    return range;
}

bool IsWholeTextureRange(const render::TextureDescriptor& desc, render::SubresourceRange range) {
    range = ResolveTextureRange(desc, range);
    return range.BaseArrayLayer == 0 &&
           range.BaseMipLevel == 0 &&
           range.ArrayLayerCount == GetTextureArrayLayerCount(desc) &&
           range.MipLevelCount == desc.MipLevels;
}

bool IsSameTextureRange(render::SubresourceRange lhs, render::SubresourceRange rhs) {
    return lhs.BaseArrayLayer == rhs.BaseArrayLayer &&
           lhs.ArrayLayerCount == rhs.ArrayLayerCount &&
           lhs.BaseMipLevel == rhs.BaseMipLevel &&
           lhs.MipLevelCount == rhs.MipLevelCount;
}

render::SubresourceRange ChooseTextureBarrierRange(
    const RDGTextureState& before,
    const RDGTextureState& after) {
    if (IsSameTextureRange(before.Range, after.Range)) {
        return after.Range;
    }
    return render::SubresourceRange::AllSub();
}

GpuTextureViewDescriptor BuildTextureViewDescForColorAttachment(
    GpuTextureHandle texture,
    const render::TextureDescriptor& textureDesc,
    const RDGColorAttachmentInfo& attachment) {
    return GpuTextureViewDescriptor{
        .Target = texture,
        .Dim = textureDesc.Dim,
        .Format = textureDesc.Format,
        .Range = attachment.Range,
        .Usage = render::TextureViewUsage::RenderTarget,
    };
}

GpuTextureViewDescriptor BuildTextureViewDescForDepthStencilAttachment(
    GpuTextureHandle texture,
    const render::TextureDescriptor& textureDesc,
    const RDGDepthStencilAttachmentInfo& attachment) {
    return GpuTextureViewDescriptor{
        .Target = texture,
        .Dim = textureDesc.Dim,
        .Format = textureDesc.Format,
        .Range = attachment.Range,
        .Usage = attachment.HasWriteAccess()
                     ? render::TextureViewUsage::DepthWrite
                     : render::TextureViewUsage::DepthRead,
    };
}

struct PassAttachmentViews {
    vector<render::ColorAttachment> ColorAttachments{};
    std::optional<render::DepthStencilAttachment> DepthStencilAttachment{};
};

PassAttachmentViews CreateAttachmentViewsForPass(
    GpuAsyncContext& context,
    const RDGGraphicsPassNode& passNode,
    const unordered_map<uint64_t, GpuTextureHandle>& textureHandles) {
    auto colorInfos = passNode._colorAttachments;
    std::sort(colorInfos.begin(), colorInfos.end(), [](const RDGColorAttachmentInfo& lhs, const RDGColorAttachmentInfo& rhs) {
        return lhs.Slot < rhs.Slot;
    });

    PassAttachmentViews attachments{};
    attachments.ColorAttachments.reserve(colorInfos.size());
    for (const auto& colorInfo : colorInfos) {
        const auto textureHandle = LookupTextureHandle(textureHandles, colorInfo.Texture);
        const auto textureDesc = ResolveTextureHandle(textureHandle)->GetDesc();
        const auto viewHandle = context.CreateTransientTextureView(
            BuildTextureViewDescForColorAttachment(textureHandle, textureDesc, colorInfo));
        attachments.ColorAttachments.emplace_back(render::ColorAttachment{
            .Target = static_cast<render::TextureView*>(viewHandle.NativeHandle),
            .Load = colorInfo.Load,
            .Store = colorInfo.Store,
            .ClearValue = colorInfo.ClearValue.value_or(render::ColorClearValue{}),
        });
    }

    if (passNode._depthStencilAttachment.has_value()) {
        const auto& depthInfo = passNode._depthStencilAttachment.value();
        const auto textureHandle = LookupTextureHandle(textureHandles, depthInfo.Texture);
        const auto textureDesc = ResolveTextureHandle(textureHandle)->GetDesc();
        const auto viewHandle = context.CreateTransientTextureView(
            BuildTextureViewDescForDepthStencilAttachment(textureHandle, textureDesc, depthInfo));
        attachments.DepthStencilAttachment = render::DepthStencilAttachment{
            .Target = static_cast<render::TextureView*>(viewHandle.NativeHandle),
            .DepthLoad = depthInfo.DepthLoad,
            .DepthStore = depthInfo.DepthStore,
            .StencilLoad = depthInfo.StencilLoad,
            .StencilStore = depthInfo.StencilStore,
            .ClearValue = depthInfo.ClearValue.value_or(render::DepthStencilClearValue{}),
        };
    }

    return attachments;
}

#ifdef RADRAY_ENABLE_VULKAN
VkPipelineStageFlags MapRDGExecutionStageToVkPipelineStage(RDGExecutionStage stage, bool isSrc) {
    const RDGExecutionStages stages{stage};
    VkPipelineStageFlags mapped = 0;
    if (stages.HasFlag(RDGExecutionStage::VertexInput)) {
        mapped |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::VertexShader)) {
        mapped |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::PixelShader)) {
        mapped |= VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::DepthStencil)) {
        mapped |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::ColorOutput)) {
        mapped |= VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::Indirect)) {
        mapped |= VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::ComputeShader)) {
        mapped |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::Copy)) {
        mapped |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::Host)) {
        mapped |= VK_PIPELINE_STAGE_HOST_BIT;
    }
    if (stages.HasFlag(RDGExecutionStage::Present)) {
        mapped |= isSrc
                      ? (VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
                      : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    if (mapped == 0 && stage == RDGExecutionStage::NONE && isSrc) {
        mapped = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
    return mapped;
}

VkPipelineStageFlags MapRDGBufferStateToVkPipelineStage(const RDGBufferState& state, bool isSrc) {
    const auto mapped = MapRDGExecutionStageToVkPipelineStage(state.Stage, isSrc);
    if (mapped != 0) {
        return mapped;
    }
    return render::vulkan::BufferStateToPipelineStageFlags(MapRDGBufferStateToRenderState(state));
}

VkPipelineStageFlags MapRDGTextureStateToVkPipelineStage(const RDGTextureState& state, bool isSrc) {
    const auto mapped = MapRDGExecutionStageToVkPipelineStage(state.Stage, isSrc);
    if (mapped != 0) {
        return mapped;
    }
    return render::vulkan::TextureStateToPipelineStageFlags(MapRDGTextureStateToRenderState(state), isSrc);
}
#endif

void EmitBackendBarrier(
    render::RenderBackend backend,
    render::CommandBuffer* cmd,
    const RDGCompiledBufferBarrier& barrier,
    const unordered_map<uint64_t, GpuBufferHandle>& bufferHandles) {
    const auto bufferHandle = LookupBufferHandle(bufferHandles, barrier.Buffer);
    auto* buffer = ResolveBufferHandle(bufferHandle);
    const auto beforeState = MapRDGBufferStateToRenderState(barrier.Before);
    const auto afterState = MapRDGBufferStateToRenderState(barrier.After);
    switch (backend) {
#ifdef RADRAY_ENABLE_D3D12
        case render::RenderBackend::D3D12: {
            auto* cmdD3D12 = render::d3d12::CastD3D12Object(cmd);
            auto* bufferD3D12 = render::d3d12::CastD3D12Object(buffer);
            D3D12_RESOURCE_BARRIER raw{};
            if (beforeState == render::BufferState::UnorderedAccess &&
                afterState == render::BufferState::UnorderedAccess) {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = bufferD3D12->_buf.Get();
            } else {
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.Transition.pResource = bufferD3D12->_buf.Get();
                raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                raw.Transition.StateBefore = render::d3d12::MapType(beforeState);
                raw.Transition.StateAfter = render::d3d12::MapType(afterState);
                if (raw.Transition.StateBefore == raw.Transition.StateAfter) {
                    return;
                }
            }
            cmdD3D12->_cmdList->ResourceBarrier(1, &raw);
            return;
        }
#endif
#ifdef RADRAY_ENABLE_VULKAN
        case render::RenderBackend::Vulkan: {
            auto* cmdVk = render::vulkan::CastVkObject(cmd);
            auto* bufferVk = render::vulkan::CastVkObject(buffer);
            VkBufferMemoryBarrier raw{};
            raw.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            raw.pNext = nullptr;
            raw.srcAccessMask = render::vulkan::BufferStateToAccessFlags(beforeState);
            raw.dstAccessMask = render::vulkan::BufferStateToAccessFlags(afterState);
            raw.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            raw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            raw.buffer = bufferVk->_buffer;
            raw.offset = 0;
            raw.size = bufferVk->_reqSize;
            const auto srcStageMask = MapRDGBufferStateToVkPipelineStage(barrier.Before, true);
            const auto dstStageMask = MapRDGBufferStateToVkPipelineStage(barrier.After, false);
            cmdVk->_device->_ftb.vkCmdPipelineBarrier(
                cmdVk->_cmdBuffer,
                srcStageMask,
                dstStageMask,
                0,
                0,
                nullptr,
                1,
                &raw,
                0,
                nullptr);
            return;
        }
#endif
        default:
            RADRAY_ABORT("RenderGraph::Execute backend {} is not supported", backend);
    }
}

void EmitBackendBarrier(
    render::RenderBackend backend,
    render::CommandBuffer* cmd,
    const RDGCompiledTextureBarrier& barrier,
    const unordered_map<uint64_t, GpuTextureHandle>& textureHandles) {
    const auto textureHandle = LookupTextureHandle(textureHandles, barrier.Texture);
    auto* texture = ResolveTextureHandle(textureHandle);
    const auto textureDesc = texture->GetDesc();
    const auto beforeState = MapRDGTextureStateToRenderState(barrier.Before);
    const auto afterState = MapRDGTextureStateToRenderState(barrier.After);
    const auto barrierRange = ChooseTextureBarrierRange(barrier.Before, barrier.After);
    switch (backend) {
#ifdef RADRAY_ENABLE_D3D12
        case render::RenderBackend::D3D12: {
            auto* cmdD3D12 = render::d3d12::CastD3D12Object(cmd);
            auto* textureD3D12 = render::d3d12::CastD3D12Object(texture);
            if (beforeState == render::TextureState::UnorderedAccess &&
                afterState == render::TextureState::UnorderedAccess) {
                D3D12_RESOURCE_BARRIER raw{};
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.UAV.pResource = textureD3D12->_tex.Get();
                cmdD3D12->_cmdList->ResourceBarrier(1, &raw);
                return;
            }

            const auto stateBefore = render::d3d12::MapType(beforeState);
            const auto stateAfter = render::d3d12::MapType(afterState);
            if (stateBefore == D3D12_RESOURCE_STATE_COMMON &&
                stateAfter == D3D12_RESOURCE_STATE_COMMON &&
                (beforeState == render::TextureState::Present || afterState == render::TextureState::Present)) {
                return;
            }
            if (stateBefore == stateAfter) {
                return;
            }

            if (IsWholeTextureRange(textureDesc, barrierRange)) {
                D3D12_RESOURCE_BARRIER raw{};
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.Transition.pResource = textureD3D12->_tex.Get();
                raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                raw.Transition.StateBefore = stateBefore;
                raw.Transition.StateAfter = stateAfter;
                cmdD3D12->_cmdList->ResourceBarrier(1, &raw);
                return;
            }

            const auto resolvedRange = ResolveTextureRange(textureDesc, barrierRange);
            const uint32_t arrayLayerCount = GetTextureArrayLayerCount(textureDesc);
            vector<D3D12_RESOURCE_BARRIER> rawBarriers{};
            rawBarriers.reserve(static_cast<size_t>(resolvedRange.ArrayLayerCount) * resolvedRange.MipLevelCount);
            for (uint32_t arrayLayer = 0; arrayLayer < resolvedRange.ArrayLayerCount; ++arrayLayer) {
                for (uint32_t mip = 0; mip < resolvedRange.MipLevelCount; ++mip) {
                    auto& raw = rawBarriers.emplace_back(D3D12_RESOURCE_BARRIER{});
                    raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    raw.Transition.pResource = textureD3D12->_tex.Get();
                    raw.Transition.Subresource = D3D12CalcSubresource(
                        resolvedRange.BaseMipLevel + mip,
                        resolvedRange.BaseArrayLayer + arrayLayer,
                        0,
                        textureDesc.MipLevels,
                        arrayLayerCount);
                    raw.Transition.StateBefore = stateBefore;
                    raw.Transition.StateAfter = stateAfter;
                }
            }
            if (!rawBarriers.empty()) {
                cmdD3D12->_cmdList->ResourceBarrier(static_cast<UINT>(rawBarriers.size()), rawBarriers.data());
            }
            return;
        }
#endif
#ifdef RADRAY_ENABLE_VULKAN
        case render::RenderBackend::Vulkan: {
            auto* cmdVk = render::vulkan::CastVkObject(cmd);
            auto* textureVk = render::vulkan::CastVkObject(texture);
            VkImageMemoryBarrier raw{};
            raw.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            raw.pNext = nullptr;
            raw.srcAccessMask = render::vulkan::TextureStateToAccessFlags(beforeState);
            raw.dstAccessMask = render::vulkan::TextureStateToAccessFlags(afterState);
            raw.oldLayout = render::vulkan::TextureStateToLayout(beforeState);
            raw.newLayout = render::vulkan::TextureStateToLayout(afterState);
            raw.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            raw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            raw.image = textureVk->_image;
            raw.subresourceRange.aspectMask = render::vulkan::ImageFormatToAspectFlags(textureVk->_rawFormat);
            if (IsWholeTextureRange(textureDesc, barrierRange)) {
                raw.subresourceRange.baseMipLevel = 0;
                raw.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                raw.subresourceRange.baseArrayLayer = 0;
                raw.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            } else {
                const auto resolvedRange = ResolveTextureRange(textureDesc, barrierRange);
                raw.subresourceRange.baseMipLevel = resolvedRange.BaseMipLevel;
                raw.subresourceRange.levelCount = resolvedRange.MipLevelCount;
                raw.subresourceRange.baseArrayLayer = resolvedRange.BaseArrayLayer;
                raw.subresourceRange.layerCount = resolvedRange.ArrayLayerCount;
            }
            const auto srcStageMask = MapRDGTextureStateToVkPipelineStage(barrier.Before, true);
            const auto dstStageMask = MapRDGTextureStateToVkPipelineStage(barrier.After, false);
            cmdVk->_device->_ftb.vkCmdPipelineBarrier(
                cmdVk->_cmdBuffer,
                srcStageMask,
                dstStageMask,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &raw);
            return;
        }
#endif
        default:
            RADRAY_ABORT("RenderGraph::Execute backend {} is not supported", backend);
    }
}

void EmitBackendBarriers(
    render::RenderBackend backend,
    render::CommandBuffer* cmd,
    const vector<RDGCompiledBarrier>& barriers,
    const unordered_map<uint64_t, GpuBufferHandle>& bufferHandles,
    const unordered_map<uint64_t, GpuTextureHandle>& textureHandles) {
    for (const auto& barrier : barriers) {
        if (std::holds_alternative<RDGCompiledBufferBarrier>(barrier)) {
            EmitBackendBarrier(backend, cmd, std::get<RDGCompiledBufferBarrier>(barrier), bufferHandles);
        } else {
            EmitBackendBarrier(backend, cmd, std::get<RDGCompiledTextureBarrier>(barrier), textureHandles);
        }
    }
}

}  // namespace

RDGEdge* RenderGraph::_CreateEdge(
    RDGNode* from,
    RDGNode* to,
    RDGExecutionStage stage,
    RDGMemoryAccess access) {
    auto edge = make_unique<RDGEdge>(from, to, stage, access);
    auto* raw = edge.get();
    _edges.emplace_back(std::move(edge));
    from->_outEdges.emplace_back(raw);
    to->_inEdges.emplace_back(raw);
    return raw;
}

RDGBufferHandle RenderGraph::AddBuffer(uint64_t size, std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGBufferNode>(id, name, RDGResourceOwnership::Internal);
    node->_size = size;
    auto* raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGBufferHandle{raw->_id};
}

RDGTextureHandle RenderGraph::AddTexture(
    render::TextureDimension dim,
    uint32_t width, uint32_t height,
    uint32_t depthOrArraySize, uint32_t mipLevels,
    uint32_t sampleCount,
    render::TextureFormat format,
    std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGTextureNode>(id, name, RDGResourceOwnership::Internal);
    node->_dim = dim;
    node->_width = width;
    node->_height = height;
    node->_depthOrArraySize = depthOrArraySize;
    node->_mipLevels = mipLevels;
    node->_sampleCount = sampleCount;
    node->_format = format;
    auto* raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGTextureHandle{raw->_id};
}

RDGBufferHandle RenderGraph::ImportBuffer(
    GpuBufferHandle buffer,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    render::BufferRange bufferRange,
    std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGBufferNode>(id, name, RDGResourceOwnership::External);
    node->_backingHandle = buffer;
    node->_importedState = RDGBufferState{
        .Stage = stage,
        .Access = access,
        .Range = bufferRange,
    };
    auto* raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGBufferHandle{raw->_id};
}

RDGTextureHandle RenderGraph::ImportTexture(
    GpuTextureHandle texture,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    RDGTextureLayout layout,
    render::SubresourceRange textureRange,
    std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGTextureNode>(id, name, RDGResourceOwnership::External);
    node->_backingHandle = texture;
    node->_importedState = RDGTextureState{
        .Stage = stage,
        .Access = access,
        .Layout = layout,
        .Range = textureRange,
    };
    auto* raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGTextureHandle{raw->_id};
}

void RenderGraph::ExportBuffer(
    RDGBufferHandle nodeHandle,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    render::BufferRange bufferRange) {
    auto* baseNode = _nodes[nodeHandle.Id].get();
    RADRAY_ASSERT(baseNode->GetTag().HasFlag(RDGNodeTag::Buffer));
    auto* node = static_cast<RDGBufferNode*>(baseNode);
    node->_exportCount += 1;
    node->_exportedState = RDGBufferState{
        .Stage = stage,
        .Access = access,
        .Range = bufferRange,
    };
}

void RenderGraph::ExportTexture(
    RDGTextureHandle nodeHandle,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    RDGTextureLayout layout,
    render::SubresourceRange textureRange) {
    auto* baseNode = _nodes[nodeHandle.Id].get();
    RADRAY_ASSERT(baseNode->GetTag().HasFlag(RDGNodeTag::Texture));
    auto* node = static_cast<RDGTextureNode*>(baseNode);
    node->_exportCount += 1;
    node->_exportedState = RDGTextureState{
        .Stage = stage,
        .Access = access,
        .Layout = layout,
        .Range = textureRange,
    };
}

RDGPassHandle RenderGraph::AddPass(std::string_view name, RDGNodeTag tag) {
    const uint64_t id = _nodes.size();
    unique_ptr<RDGPassNode> node{};
    switch (NormalizePassQueueTag(tag)) {
        case RDGNodeTag::GraphicsPass:
            node = make_unique<RDGGraphicsPassNode>(id, name);
            break;
        case RDGNodeTag::ComputePass:
            node = make_unique<RDGComputePassNode>(id, name);
            break;
        case RDGNodeTag::CopyPass:
            node = make_unique<RDGCopyPassNode>(id, name);
            break;
        default:
            node = make_unique<RDGGraphicsPassNode>(id, name);
            break;
    }
    auto* raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGPassHandle{raw->_id};
}

RDGPassHandle RDGRasterPassBuilder::_EnsurePass() {
    RADRAY_ASSERT(_graph != nullptr);
    if (_pass.IsValid()) {
        return _pass;
    }

    const uint64_t id = _graph->_nodes.size();
    auto node = make_unique<RDGGraphicsPassNode>(id, fmt::format("RasterPass{}", id));
    auto* raw = node.get();
    _graph->_nodes.emplace_back(std::move(node));
    _pass = RDGPassHandle{raw->_id};
    return _pass;
}

void RDGRasterPassBuilder::_ValidateShaderStages(render::ShaderStages stages) const {
    RADRAY_ASSERT((stages & ~render::ShaderStage::Graphics) == render::ShaderStage::UNKNOWN);
}

void RDGRasterPassBuilder::_LinkBufferStages(
    RDGBufferHandle buffer,
    render::ShaderStages stages,
    RDGMemoryAccess access,
    render::BufferRange range) {
    _ValidateShaderStages(stages);
    const auto pass = _EnsurePass();
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _graph->Link(buffer, pass, RDGExecutionStage::VertexShader, access, range);
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _graph->Link(buffer, pass, RDGExecutionStage::PixelShader, access, range);
    }
}

void RDGRasterPassBuilder::_LinkTextureStages(
    RDGTextureHandle texture,
    render::ShaderStages stages,
    RDGMemoryAccess access,
    RDGTextureLayout layout,
    render::SubresourceRange range) {
    _ValidateShaderStages(stages);
    const auto pass = _EnsurePass();
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _graph->Link(texture, pass, RDGExecutionStage::VertexShader, access, layout, range);
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _graph->Link(texture, pass, RDGExecutionStage::PixelShader, access, layout, range);
    }
}

RDGPassHandle RDGRasterPassBuilder::Build() {
    return _EnsurePass();
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseColorAttachment(
    uint32_t slot,
    RDGTextureHandle texture,
    render::SubresourceRange range,
    render::LoadAction load,
    render::StoreAction store,
    std::optional<render::ColorClearValue> clearValue) {
    const auto pass = this->Build();
    RADRAY_ASSERT(pass.IsValid() && pass.Id < _graph->_nodes.size());
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::GraphicsPass));
    auto* passNode = static_cast<RDGGraphicsPassNode*>(node);
    passNode->_colorAttachments.emplace_back(RDGColorAttachmentInfo{
        .Slot = slot,
        .Texture = texture,
        .Range = range,
        .Load = load,
        .Store = store,
        .ClearValue = std::move(clearValue),
    });
    RDGMemoryAccess access = RDGMemoryAccess::ColorAttachmentWrite;
    if (load == render::LoadAction::Load) {
        access = RDGMemoryAccess::ColorAttachmentRead | RDGMemoryAccess::ColorAttachmentWrite;
    }
    _graph->Link(pass, texture, RDGExecutionStage::ColorOutput, access, RDGTextureLayout::ColorAttachment, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseDepthStencilAttachment(
    RDGTextureHandle texture,
    render::SubresourceRange range,
    render::LoadAction depthLoad,
    render::StoreAction depthStore,
    render::LoadAction stencilLoad,
    render::StoreAction stencilStore,
    std::optional<render::DepthStencilClearValue> clearValue) {
    const auto pass = this->Build();
    RADRAY_ASSERT(pass.IsValid() && pass.Id < _graph->_nodes.size());
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::GraphicsPass));
    auto* passNode = static_cast<RDGGraphicsPassNode*>(node);
    RADRAY_ASSERT(!passNode->_depthStencilAttachment.has_value());
    passNode->_depthStencilAttachment = RDGDepthStencilAttachmentInfo{
        .Texture = texture,
        .Range = range,
        .DepthLoad = depthLoad,
        .DepthStore = depthStore,
        .StencilLoad = stencilLoad,
        .StencilStore = stencilStore,
        .ClearValue = std::move(clearValue),
    };

    const bool isWrite = passNode->_depthStencilAttachment->HasWriteAccess();
    if (isWrite) {
        _graph->Link(
            pass,
            texture,
            RDGExecutionStage::DepthStencil,
            RDGMemoryAccess::DepthStencilWrite,
            RDGTextureLayout::DepthStencilAttachment,
            range);
    } else {
        _graph->Link(
            texture,
            pass,
            RDGExecutionStage::DepthStencil,
            RDGMemoryAccess::DepthStencilRead,
            RDGTextureLayout::DepthStencilReadOnly,
            range);
    }
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseVertexBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _graph->Link(buffer, this->Build(), RDGExecutionStage::VertexInput, RDGMemoryAccess::VertexRead, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseIndexBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _graph->Link(buffer, this->Build(), RDGExecutionStage::VertexInput, RDGMemoryAccess::IndexRead, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseIndirectBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _graph->Link(buffer, this->Build(), RDGExecutionStage::Indirect, RDGMemoryAccess::IndirectRead, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseCBuffer(
    RDGBufferHandle buffer,
    render::ShaderStages stages,
    render::BufferRange range) {
    _LinkBufferStages(buffer, stages, RDGMemoryAccess::ConstantRead, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseBuffer(
    RDGBufferHandle buffer,
    render::ShaderStages stages,
    render::BufferRange range) {
    _LinkBufferStages(buffer, stages, RDGMemoryAccess::ShaderRead, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseRWBuffer(
    RDGBufferHandle buffer,
    render::ShaderStages stages,
    render::BufferRange range) {
    const auto pass = this->Build();
    _ValidateShaderStages(stages);
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _graph->Link(pass, buffer, RDGExecutionStage::VertexShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, range);
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _graph->Link(pass, buffer, RDGExecutionStage::PixelShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, range);
    }
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseTexture(
    RDGTextureHandle texture,
    render::ShaderStages stages,
    render::SubresourceRange range) {
    _LinkTextureStages(texture, stages, RDGMemoryAccess::ShaderRead, RDGTextureLayout::ShaderReadOnly, range);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseRWTexture(
    RDGTextureHandle texture,
    render::ShaderStages stages,
    render::SubresourceRange range) {
    const auto pass = this->Build();
    _ValidateShaderStages(stages);
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _graph->Link(pass, texture, RDGExecutionStage::VertexShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, RDGTextureLayout::General, range);
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _graph->Link(pass, texture, RDGExecutionStage::PixelShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, RDGTextureLayout::General, range);
    }
    return *this;
}

RDGPassHandle RDGComputePassBuilder::_EnsurePass() {
    RADRAY_ASSERT(_graph != nullptr);
    if (_pass.IsValid()) {
        return _pass;
    }

    const uint64_t id = _graph->_nodes.size();
    auto node = make_unique<RDGComputePassNode>(id, fmt::format("ComputePass{}", id));
    auto* raw = node.get();
    _graph->_nodes.emplace_back(std::move(node));
    _pass = RDGPassHandle{raw->_id};
    return _pass;
}

RDGPassHandle RDGComputePassBuilder::Build() {
    return _EnsurePass();
}

RDGComputePassBuilder& RDGComputePassBuilder::UseCBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _graph->Link(buffer, this->Build(), RDGExecutionStage::ComputeShader, RDGMemoryAccess::ConstantRead, range);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _graph->Link(buffer, this->Build(), RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, range);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseRWBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _graph->Link(this->Build(), buffer, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, range);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseTexture(RDGTextureHandle texture, render::SubresourceRange range) {
    _graph->Link(texture, this->Build(), RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, RDGTextureLayout::ShaderReadOnly, range);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseRWTexture(RDGTextureHandle texture, render::SubresourceRange range) {
    _graph->Link(this->Build(), texture, RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, RDGTextureLayout::General, range);
    return *this;
}

RDGPassHandle RDGCopyPassBuilder::_EnsurePass() {
    RADRAY_ASSERT(_graph != nullptr);
    if (_pass.IsValid()) {
        return _pass;
    }

    const uint64_t id = _graph->_nodes.size();
    auto node = make_unique<RDGCopyPassNode>(id, fmt::format("CopyPass{}", id));
    auto* raw = node.get();
    _graph->_nodes.emplace_back(std::move(node));
    _pass = RDGPassHandle{raw->_id};
    return _pass;
}

RDGPassHandle RDGCopyPassBuilder::Build() {
    return _EnsurePass();
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyBufferToBuffer(
    RDGBufferHandle dst,
    uint64_t dstOffset,
    RDGBufferHandle src,
    uint64_t srcOffset,
    uint64_t size) {
    const auto pass = this->Build();
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::CopyPass));
    auto* passNode = static_cast<RDGCopyPassNode*>(node);
    passNode->_ops.emplace_back(RDGCopyBufferToBufferInfo{
        .Dst = dst,
        .DstOffset = dstOffset,
        .Src = src,
        .SrcOffset = srcOffset,
        .Size = size,
    });
    _graph->Link(src, pass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, render::BufferRange{srcOffset, size});
    _graph->Link(pass, dst, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, render::BufferRange{dstOffset, size});
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyBufferToTexture(
    RDGTextureHandle dst,
    render::SubresourceRange dstRange,
    RDGBufferHandle src,
    uint64_t srcOffset) {
    const auto pass = this->Build();
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::CopyPass));
    auto* passNode = static_cast<RDGCopyPassNode*>(node);
    passNode->_ops.emplace_back(RDGCopyBufferToTextureInfo{
        .Dst = dst,
        .DstRange = dstRange,
        .Src = src,
        .SrcOffset = srcOffset,
    });
    _graph->Link(src, pass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, render::BufferRange{srcOffset, render::BufferRange::All()});
    _graph->Link(pass, dst, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, RDGTextureLayout::TransferDestination, dstRange);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyTextureToBuffer(
    RDGBufferHandle dst,
    uint64_t dstOffset,
    RDGTextureHandle src,
    render::SubresourceRange srcRange) {
    const auto pass = this->Build();
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::CopyPass));
    auto* passNode = static_cast<RDGCopyPassNode*>(node);
    passNode->_ops.emplace_back(RDGCopyTextureToBufferInfo{
        .Dst = dst,
        .DstOffset = dstOffset,
        .Src = src,
        .SrcRange = srcRange,
    });
    _graph->Link(src, pass, RDGExecutionStage::Copy, RDGMemoryAccess::TransferRead, RDGTextureLayout::TransferSource, srcRange);
    _graph->Link(pass, dst, RDGExecutionStage::Copy, RDGMemoryAccess::TransferWrite, render::BufferRange{dstOffset, render::BufferRange::All()});
    return *this;
}

void RenderGraph::Link(
    RDGNodeHandle fromHandle,
    RDGNodeHandle toHandle,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    render::BufferRange bufferRange) {
    auto* from = _nodes[fromHandle.Id].get();
    auto* to = _nodes[toHandle.Id].get();
    auto* edge = this->_CreateEdge(from, to, stage, access);
    edge->_bufferRange = bufferRange;
}

void RenderGraph::Link(
    RDGNodeHandle fromHandle,
    RDGNodeHandle toHandle,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    RDGTextureLayout layout,
    render::SubresourceRange textureRange) {
    auto* from = _nodes[fromHandle.Id].get();
    auto* to = _nodes[toHandle.Id].get();
    auto* edge = this->_CreateEdge(from, to, stage, access);
    edge->_textureLayout = layout;
    edge->_textureRange = textureRange;
}

RDGCompileResult RenderGraph::Compile() const {
    RDGCompileResult result{};

    // 编译阶段只在 RDG 抽象层比较状态，不下沉到 render backend barrier。
    const auto bufferStateEqual = [](const RDGBufferState& lhs, const RDGBufferState& rhs) {
        return lhs.Stage == rhs.Stage &&
               lhs.Access == rhs.Access &&
               lhs.Range.Offset == rhs.Range.Offset &&
               lhs.Range.Size == rhs.Range.Size;
    };
    const auto textureStateEqual = [](const RDGTextureState& lhs, const RDGTextureState& rhs) {
        return lhs.Stage == rhs.Stage &&
               lhs.Access == rhs.Access &&
               lhs.Layout == rhs.Layout &&
               lhs.Range.BaseArrayLayer == rhs.Range.BaseArrayLayer &&
               lhs.Range.ArrayLayerCount == rhs.Range.ArrayLayerCount &&
               lhs.Range.BaseMipLevel == rhs.Range.BaseMipLevel &&
               lhs.Range.MipLevelCount == rhs.Range.MipLevelCount;
    };
    const uint64_t dependencyKeyStride = _nodes.size() + 1;

    // 先给每条 edge 一个稳定序号，后面用它还原“同一个 pass 内谁先谁后”的声明顺序。
    const auto edgeIndexByPtr = BuildEdgeIndexByPtr(_edges);

    // 收集所有 pass，并建立 pass id -> 邻接表索引的映射，后面推 DAG 和拓扑排序都靠它。
    vector<RDGPassHandle> passHandles{};
    passHandles.reserve(_nodes.size());
    unordered_map<uint64_t, uint32_t> passIndexById{};
    passIndexById.reserve(_nodes.size());
    for (const auto& nodeHolder : _nodes) {
        auto* node = nodeHolder.get();
        if (node == nullptr || !node->GetTag().HasFlag(RDGNodeTag::Pass)) {
            continue;
        }

        passIndexById.emplace(node->_id, static_cast<uint32_t>(passHandles.size()));
        passHandles.emplace_back(RDGPassHandle{node->_id});
    }

    vector<vector<uint32_t>> adjacency(passHandles.size());
    vector<uint32_t> indegree(passHandles.size(), 0);
    unordered_set<uint64_t> uniquePassDeps{};
    uniquePassDeps.reserve(_edges.size());
    result.Dependencies.reserve(_edges.size());

    // 资源级 hazard 可能会多次推导出同一条 pass 依赖：
    // Dependencies 保留完整来源，adjacency 只保留去重后的 DAG 边。
    const auto addDependency = [&](RDGPassHandle before, RDGPassHandle after, RDGResourceHandle resource) {
        if (!before.IsValid() || !after.IsValid() || before.Id == after.Id) {
            return;
        }

        result.Dependencies.emplace_back(RDGPassDependency{
            .Before = before,
            .After = after,
            .Resource = resource,
        });

        const uint64_t key = before.Id * dependencyKeyStride + after.Id;
        if (!uniquePassDeps.emplace(key).second) {
            return;
        }

        const auto beforeIt = passIndexById.find(before.Id);
        const auto afterIt = passIndexById.find(after.Id);
        RADRAY_ASSERT(beforeIt != passIndexById.end());
        RADRAY_ASSERT(afterIt != passIndexById.end());
        adjacency[beforeIt->second].emplace_back(afterIt->second);
        indegree[afterIt->second] += 1;
    };

    const auto compiledBuffers = CollectCompiledBufferResources(_nodes, edgeIndexByPtr);
    const auto compiledTextures = CollectCompiledTextureResources(_nodes, edgeIndexByPtr);

    // 第一阶段：按资源汇总 usage，并基于单资源 hazard 推导 pass 依赖。
    // 规则是保守的：写后所有读/写都依赖这个写；写也依赖之前未截断的所有读。
    for (const auto& compiledBuffer : compiledBuffers) {
        RDGPassHandle lastWriter{};
        bool hasLastWriter = false;
        vector<RDGPassHandle> activeReaders{};
        unordered_set<uint64_t> activeReaderIds{};
        activeReaderIds.reserve(compiledBuffer.PassUsages.size());
        for (const auto& usage : compiledBuffer.PassUsages) {
            if (hasLastWriter && lastWriter.Id != usage.Pass.Id) {
                addDependency(lastWriter, usage.Pass, RDGResourceHandle{compiledBuffer.Node->_id});
            }
            if (!usage.HasWrite) {
                if (activeReaderIds.emplace(usage.Pass.Id).second) {
                    activeReaders.emplace_back(usage.Pass);
                }
                continue;
            }

            for (const auto& reader : activeReaders) {
                if (reader.Id == usage.Pass.Id) {
                    continue;
                }
                addDependency(reader, usage.Pass, RDGResourceHandle{compiledBuffer.Node->_id});
            }

            activeReaders.clear();
            activeReaderIds.clear();
            lastWriter = usage.Pass;
            hasLastWriter = true;
        }
    }

    for (const auto& compiledTexture : compiledTextures) {
        RDGPassHandle lastWriter{};
        bool hasLastWriter = false;
        vector<RDGPassHandle> activeReaders{};
        unordered_set<uint64_t> activeReaderIds{};
        activeReaderIds.reserve(compiledTexture.PassUsages.size());
        for (const auto& usage : compiledTexture.PassUsages) {
            if (hasLastWriter && lastWriter.Id != usage.Pass.Id) {
                addDependency(lastWriter, usage.Pass, RDGResourceHandle{compiledTexture.Node->_id});
            }
            if (!usage.HasWrite) {
                if (activeReaderIds.emplace(usage.Pass.Id).second) {
                    activeReaders.emplace_back(usage.Pass);
                }
                continue;
            }

            for (const auto& reader : activeReaders) {
                if (reader.Id == usage.Pass.Id) {
                    continue;
                }
                addDependency(reader, usage.Pass, RDGResourceHandle{compiledTexture.Node->_id});
            }

            activeReaders.clear();
            activeReaderIds.clear();
            lastWriter = usage.Pass;
            hasLastWriter = true;
        }
    }

    // 第二阶段：对去重后的 pass 依赖图做稳定拓扑排序。
    // 同层按 pass id 升序选择，保证相同输入图得到稳定输出顺序。
    vector<uint32_t> ready{};
    ready.reserve(passHandles.size());
    for (uint32_t i = 0; i < indegree.size(); ++i) {
        if (indegree[i] == 0) {
            ready.emplace_back(i);
        }
    }

    result.PassOrder.reserve(passHandles.size());
    while (!ready.empty()) {
        const auto nextIt = std::min_element(ready.begin(), ready.end(), [&](uint32_t lhs, uint32_t rhs) {
            return passHandles[lhs].Id < passHandles[rhs].Id;
        });
        const uint32_t current = *nextIt;
        ready.erase(nextIt);
        result.PassOrder.emplace_back(passHandles[current]);

        for (const auto next : adjacency[current]) {
            RADRAY_ASSERT(indegree[next] > 0);
            indegree[next] -= 1;
            if (indegree[next] == 0) {
                ready.emplace_back(next);
            }
        }
    }
    RADRAY_ASSERT(result.PassOrder.size() == passHandles.size());
    if (result.PassOrder.size() != passHandles.size()) {
        RADRAY_ABORT("RenderGraph::Compile detected a cycle in pass dependencies");
    }
    // 当前资源 hazard 推导默认把 pass 声明顺序当作时间轴。
    // 如果拓扑排序被迫打破这个顺序，说明用户声明顺序与实际数据流冲突，直接报错暴露这个隐式约束。
    for (uint32_t i = 1; i < result.PassOrder.size(); ++i) {
        const auto previousPass = result.PassOrder[i - 1];
        const auto currentPass = result.PassOrder[i];
        if (previousPass.Id < currentPass.Id) {
            continue;
        }

        RADRAY_ASSERT(previousPass.Id < _nodes.size() && currentPass.Id < _nodes.size());
        auto* previousNode = _nodes[previousPass.Id].get();
        auto* currentNode = _nodes[currentPass.Id].get();
        RADRAY_ASSERT(previousNode != nullptr && previousNode->GetTag().HasFlag(RDGNodeTag::Pass));
        RADRAY_ASSERT(currentNode != nullptr && currentNode->GetTag().HasFlag(RDGNodeTag::Pass));
        RADRAY_ABORT(
            "RenderGraph::Compile detected declaration-order conflict: pass '{}' ({}) must execute before pass '{}' ({}) due to data flow, but it was declared later",
            previousNode->_name,
            previousPass.Id,
            currentNode->_name,
            currentPass.Id);
    }

    // 按拓扑结果初始化每个 compiled pass，并把 DAG 前驱列表回填进去。
    unordered_map<uint64_t, uint32_t> passOrderIndexById{};
    passOrderIndexById.reserve(result.PassOrder.size());
    result.Passes.reserve(result.PassOrder.size());
    for (uint32_t i = 0; i < result.PassOrder.size(); ++i) {
        passOrderIndexById.emplace(result.PassOrder[i].Id, i);
        result.Passes.emplace_back(RDGCompiledPass{
            .Pass = result.PassOrder[i],
            .Predecessors = {},
            .BarriersBefore = {},
            .BarriersAfter = {},
        });
    }

    for (uint32_t beforeIndex = 0; beforeIndex < adjacency.size(); ++beforeIndex) {
        const auto before = passHandles[beforeIndex];
        for (const auto afterIndex : adjacency[beforeIndex]) {
            const auto after = passHandles[afterIndex];
            auto& predecessors = result.Passes[passOrderIndexById.at(after.Id)].Predecessors;
            predecessors.emplace_back(before);
        }
    }

    for (auto& compiledPass : result.Passes) {
        std::sort(compiledPass.Predecessors.begin(), compiledPass.Predecessors.end(), [](const RDGPassHandle& lhs, const RDGPassHandle& rhs) {
            return lhs.Id < rhs.Id;
        });
    }

    // 第三阶段：沿着拓扑顺序推进每个资源的当前状态，
    // 需要切状态时记到 BarriersBefore，导出到图外的最终状态记到 BarriersAfter。
    result.Lifetimes.reserve(compiledBuffers.size() + compiledTextures.size());

    for (const auto& compiledBuffer : compiledBuffers) {
        auto passUsages = compiledBuffer.PassUsages;
        std::sort(passUsages.begin(), passUsages.end(), [&](const BufferPassUsage& lhs, const BufferPassUsage& rhs) {
            return passOrderIndexById.at(lhs.Pass.Id) < passOrderIndexById.at(rhs.Pass.Id);
        });

        RDGCompiledResourceLifetime lifetime{
            .Resource = RDGResourceHandle{compiledBuffer.Node->_id},
            .FirstPassIndex = std::nullopt,
            .LastPassIndex = std::nullopt,
        };
        if (!passUsages.empty()) {
            lifetime.FirstPassIndex = passOrderIndexById.at(passUsages.front().Pass.Id);
            lifetime.LastPassIndex = passOrderIndexById.at(passUsages.back().Pass.Id);
        }
        result.Lifetimes.emplace_back(lifetime);

        // Buffer 的初始状态来自 import；内部资源默认从 NONE/NONE 开始。
        RDGBufferState currentState = compiledBuffer.Node->_importedState.value_or(RDGBufferState{
            .Stage = RDGExecutionStage::NONE,
            .Access = RDGMemoryAccess::NONE,
            .Range = render::BufferRange::AllRange(),
        });
        bool hasPreviousUsage = false;
        bool previousUsageHadWrite = false;

        for (const auto& usage : passUsages) {
            const uint32_t passIndex = passOrderIndexById.at(usage.Pass.Id);
            const bool sameStateWriteHazard =
                hasPreviousUsage && previousUsageHadWrite && bufferStateEqual(currentState, usage.FirstState);
            if (!bufferStateEqual(currentState, usage.FirstState) || sameStateWriteHazard) {
                // 即使状态完全一致，只要上一个 pass 对资源发生过写入，
                // 这里仍要保留一条 same-state barrier，给执行层表达纯内存依赖。
                result.Passes[passIndex].BarriersBefore.emplace_back(RDGCompiledBufferBarrier{
                    .Buffer = RDGBufferHandle{compiledBuffer.Node->_id},
                    .Before = currentState,
                    .After = usage.FirstState,
                });
                currentState = usage.FirstState;
            }

            currentState = usage.LastState;
            hasPreviousUsage = true;
            previousUsageHadWrite = usage.HasWrite;
        }

        if (compiledBuffer.Node->_exportedState.has_value() && lifetime.LastPassIndex.has_value() &&
            (!bufferStateEqual(currentState, compiledBuffer.Node->_exportedState.value()) ||
             (hasPreviousUsage && previousUsageHadWrite))) {
            result.Passes[*lifetime.LastPassIndex].BarriersAfter.emplace_back(RDGCompiledBufferBarrier{
                .Buffer = RDGBufferHandle{compiledBuffer.Node->_id},
                .Before = currentState,
                .After = compiledBuffer.Node->_exportedState.value(),
            });
        }
    }

    for (const auto& compiledTexture : compiledTextures) {
        auto passUsages = compiledTexture.PassUsages;
        std::sort(passUsages.begin(), passUsages.end(), [&](const TexturePassUsage& lhs, const TexturePassUsage& rhs) {
            return passOrderIndexById.at(lhs.Pass.Id) < passOrderIndexById.at(rhs.Pass.Id);
        });

        RDGCompiledResourceLifetime lifetime{
            .Resource = RDGResourceHandle{compiledTexture.Node->_id},
            .FirstPassIndex = std::nullopt,
            .LastPassIndex = std::nullopt,
        };
        if (!passUsages.empty()) {
            lifetime.FirstPassIndex = passOrderIndexById.at(passUsages.front().Pass.Id);
            lifetime.LastPassIndex = passOrderIndexById.at(passUsages.back().Pass.Id);
        }
        result.Lifetimes.emplace_back(lifetime);

        // Texture 的默认起始 layout 视为 Undefined，方便后续明确记录第一次使用前的切换。
        RDGTextureState currentState = compiledTexture.Node->_importedState.value_or(RDGTextureState{
            .Stage = RDGExecutionStage::NONE,
            .Access = RDGMemoryAccess::NONE,
            .Layout = RDGTextureLayout::Undefined,
            .Range = render::SubresourceRange::AllSub(),
        });
        bool hasPreviousUsage = false;
        bool previousUsageHadWrite = false;

        for (const auto& usage : passUsages) {
            const uint32_t passIndex = passOrderIndexById.at(usage.Pass.Id);
            const bool sameStateWriteHazard =
                hasPreviousUsage && previousUsageHadWrite && textureStateEqual(currentState, usage.FirstState);
            if (!textureStateEqual(currentState, usage.FirstState) || sameStateWriteHazard) {
                result.Passes[passIndex].BarriersBefore.emplace_back(RDGCompiledTextureBarrier{
                    .Texture = RDGTextureHandle{compiledTexture.Node->_id},
                    .Before = currentState,
                    .After = usage.FirstState,
                });
                currentState = usage.FirstState;
            }

            currentState = usage.LastState;
            hasPreviousUsage = true;
            previousUsageHadWrite = usage.HasWrite;
        }

        if (compiledTexture.Node->_exportedState.has_value() && lifetime.LastPassIndex.has_value() &&
            (!textureStateEqual(currentState, compiledTexture.Node->_exportedState.value()) ||
             (hasPreviousUsage && previousUsageHadWrite))) {
            result.Passes[*lifetime.LastPassIndex].BarriersAfter.emplace_back(RDGCompiledTextureBarrier{
                .Texture = RDGTextureHandle{compiledTexture.Node->_id},
                .Before = currentState,
                .After = compiledTexture.Node->_exportedState.value(),
            });
        }
    }

    return result;
}

RDGExecuteResult RenderGraph::Execute(GpuRuntime& runtime, const RDGCompileResult& compiled) const {
    // const auto [isValid, errorMessage] = this->Validate();
    // if (!isValid) {
    //     RADRAY_ABORT("RenderGraph::Execute validation failed: {}", errorMessage);
    // }

    // const auto compiled = this->Compile();
    // if (compiled.PassOrder.empty()) {
    //     return {};
    // }
    // if (runtime._device == nullptr) {
    //     throw GpuSystemException("RenderGraph::Execute requires a valid GpuRuntime");
    // }

    const auto backend = runtime._device->GetBackend();
    PersistentResourceCleanup cleanup{&runtime};
    auto context = runtime.BeginAsync(render::QueueType::Direct);

    unordered_map<uint64_t, GpuBufferHandle> bufferHandles{};
    unordered_map<uint64_t, GpuTextureHandle> textureHandles{};
    bufferHandles.reserve(_nodes.size());
    textureHandles.reserve(_nodes.size());

    vector<GpuResourceHandle> transientLikePersistentResources{};
    transientLikePersistentResources.reserve(_nodes.size());

    for (const auto& nodeHolder : _nodes) {
        auto* node = nodeHolder.get();
        RADRAY_ASSERT(node != nullptr);

        if (node->GetTag().HasFlag(RDGNodeTag::Buffer)) {
            RDGBufferHandle bufferHandle{};
            bufferHandle.Id = node->_id;
            const auto& bufferNode = GetBufferNode(*this, bufferHandle);
            if (bufferNode._ownership == RDGResourceOwnership::External) {
                bufferHandles.emplace(bufferNode._id, bufferNode._backingHandle);
                continue;
            }

            const auto handle = runtime.CreateBuffer(BuildBufferDescriptor(bufferNode));
            bufferHandles.emplace(bufferNode._id, handle);
            cleanup.Track(handle);
            if (!bufferNode._exportedState.has_value()) {
                transientLikePersistentResources.emplace_back(handle);
            }
            continue;
        }

        if (node->GetTag().HasFlag(RDGNodeTag::Texture)) {
            RDGTextureHandle textureHandle{};
            textureHandle.Id = node->_id;
            const auto& textureNode = GetTextureNode(*this, textureHandle);
            if (textureNode._ownership == RDGResourceOwnership::External) {
                textureHandles.emplace(textureNode._id, textureNode._backingHandle);
                continue;
            }

            const auto handle = runtime.CreateTexture(BuildTextureDescriptor(textureNode));
            textureHandles.emplace(textureNode._id, handle);
            cleanup.Track(handle);
            if (!textureNode._exportedState.has_value()) {
                transientLikePersistentResources.emplace_back(handle);
            }
        }
    }

    auto* cmd = context->CreateCommandBuffer();
    cmd->Begin();

    for (const auto& compiledPass : compiled.Passes) {
        EmitBackendBarriers(backend, cmd, compiledPass.BarriersBefore, bufferHandles, textureHandles);

        const auto& passNode = GetPassNode(*this, compiledPass.Pass);
        switch (NormalizePassQueueTag(passNode.GetTag())) {
            case RDGNodeTag::GraphicsPass: {
                const auto& graphicsPassNode = static_cast<const RDGGraphicsPassNode&>(passNode);
                auto attachments = CreateAttachmentViewsForPass(*context, graphicsPassNode, textureHandles);
                render::RenderPassDescriptor passDesc{};
                passDesc.ColorAttachments = std::span<const render::ColorAttachment>(attachments.ColorAttachments);
                passDesc.DepthStencilAttachment = attachments.DepthStencilAttachment;
                passDesc.Name = passNode._name;
                auto encoderOpt = cmd->BeginRenderPass(passDesc);
                if (!encoderOpt.HasValue()) {
                    RADRAY_ABORT("RenderGraph::Execute BeginRenderPass failed for pass '{}'", passNode._name);
                }
                cmd->EndRenderPass(encoderOpt.Release());
                break;
            }
            case RDGNodeTag::ComputePass: {
                auto encoderOpt = cmd->BeginComputePass();
                if (!encoderOpt.HasValue()) {
                    RADRAY_ABORT("RenderGraph::Execute BeginComputePass failed for pass '{}'", passNode._name);
                }
                cmd->EndComputePass(encoderOpt.Release());
                break;
            }
            case RDGNodeTag::CopyPass: {
                const auto& copyPassNode = static_cast<const RDGCopyPassNode&>(passNode);
                for (const auto& op : copyPassNode._ops) {
                    if (std::holds_alternative<RDGCopyBufferToBufferInfo>(op)) {
                        const auto& info = std::get<RDGCopyBufferToBufferInfo>(op);
                        cmd->CopyBufferToBuffer(
                            ResolveBufferHandle(LookupBufferHandle(bufferHandles, info.Dst)),
                            info.DstOffset,
                            ResolveBufferHandle(LookupBufferHandle(bufferHandles, info.Src)),
                            info.SrcOffset,
                            info.Size);
                    } else if (std::holds_alternative<RDGCopyBufferToTextureInfo>(op)) {
                        const auto& info = std::get<RDGCopyBufferToTextureInfo>(op);
                        cmd->CopyBufferToTexture(
                            ResolveTextureHandle(LookupTextureHandle(textureHandles, info.Dst)),
                            info.DstRange,
                            ResolveBufferHandle(LookupBufferHandle(bufferHandles, info.Src)),
                            info.SrcOffset);
                    } else {
                        const auto& info = std::get<RDGCopyTextureToBufferInfo>(op);
                        cmd->CopyTextureToBuffer(
                            ResolveBufferHandle(LookupBufferHandle(bufferHandles, info.Dst)),
                            info.DstOffset,
                            ResolveTextureHandle(LookupTextureHandle(textureHandles, info.Src)),
                            info.SrcRange);
                    }
                }
                break;
            }
            default:
                RADRAY_ABORT(
                    "RenderGraph::Execute encountered unsupported pass tag {} on pass '{}'",
                    passNode.GetTag(),
                    passNode._name);
        }

        EmitBackendBarriers(backend, cmd, compiledPass.BarriersAfter, bufferHandles, textureHandles);
    }

    cmd->End();

    auto task = runtime.SubmitAsync(std::move(context));
    cleanup.Release();
    for (const auto& handle : transientLikePersistentResources) {
        runtime.DestroyResourceAfter(handle, task);
    }

    RDGExecuteResult result{};
    for (const auto& nodeHolder : _nodes) {
        auto* node = nodeHolder.get();
        RADRAY_ASSERT(node != nullptr);

        if (node->GetTag().HasFlag(RDGNodeTag::Buffer)) {
            RDGBufferHandle bufferHandle{};
            bufferHandle.Id = node->_id;
            const auto& bufferNode = GetBufferNode(*this, bufferHandle);
            if (bufferNode._exportedState.has_value()) {
                result.ExportedBuffers.emplace(bufferNode._id, LookupBufferHandle(bufferHandles, bufferHandle));
            }
            continue;
        }

        if (node->GetTag().HasFlag(RDGNodeTag::Texture)) {
            RDGTextureHandle textureHandle{};
            textureHandle.Id = node->_id;
            const auto& textureNode = GetTextureNode(*this, textureHandle);
            if (textureNode._exportedState.has_value()) {
                result.ExportedTextures.emplace(textureNode._id, LookupTextureHandle(textureHandles, textureHandle));
            }
        }
    }

    result.Task = std::move(task);
    return result;
}

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
            fmt::format_to(
                std::back_inserter(dot),
                "  ");
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

std::pair<bool, string> RenderGraph::Validate() const {
    for (size_t i = 0; i < _nodes.size(); ++i) {
        auto* node = _nodes[i].get();
        if (node == nullptr) {
            return {false, fmt::format("{}: null node", i)};
        }
        if (node->_id != i) {
            return {false, fmt::format("{}: node id mismatch", i)};
        }

        if (node->GetTag().HasFlag(RDGNodeTag::GraphicsPass)) {
            const auto* passNode = static_cast<RDGGraphicsPassNode*>(node);
            unordered_set<uint32_t> colorSlots{};
            for (const auto& attachment : passNode->_colorAttachments) {
                if (!colorSlots.emplace(attachment.Slot).second) {
                    return {false, fmt::format("pass node '{}' has duplicate color attachment slot {}", node->_name, attachment.Slot)};
                }
                if (attachment.Texture.Id >= _nodes.size()) {
                    return {false, fmt::format("pass node '{}' references invalid color attachment handle {}", node->_name, attachment.Texture.Id)};
                }
                auto* textureNode = _nodes[attachment.Texture.Id].get();
                if (textureNode == nullptr || !textureNode->GetTag().HasFlag(RDGNodeTag::Texture)) {
                    return {false, fmt::format("pass node '{}' color attachment slot {} is not a texture node", node->_name, attachment.Slot)};
                }
            }
            if (passNode->_depthStencilAttachment.has_value()) {
                const auto& attachment = passNode->_depthStencilAttachment.value();
                if (attachment.Texture.Id >= _nodes.size()) {
                    return {false, fmt::format("pass node '{}' references invalid depth-stencil attachment handle {}", node->_name, attachment.Texture.Id)};
                }
                auto* textureNode = _nodes[attachment.Texture.Id].get();
                if (textureNode == nullptr || !textureNode->GetTag().HasFlag(RDGNodeTag::Texture)) {
                    return {false, fmt::format("pass node '{}' depth-stencil attachment is not a texture node", node->_name)};
                }
            }
        }

        if (node->GetTag().HasFlag(RDGNodeTag::Buffer)) {
            auto* bufferNode = static_cast<RDGBufferNode*>(node);
            if (bufferNode->_ownership == RDGResourceOwnership::External && !bufferNode->_backingHandle.IsValid()) {
                return {false, fmt::format("external buffer node '{}' has invalid backing handle", node->_name)};
            }
            if (bufferNode->_exportCount > 1) {
                return {false, fmt::format("buffer node '{}' exported more than once", node->_name)};
            }
            if (bufferNode->_ownership == RDGResourceOwnership::Transient && bufferNode->_exportedState.has_value()) {
                return {false, fmt::format("transient buffer node '{}' was exported", node->_name)};
            }
        }

        if (node->GetTag().HasFlag(RDGNodeTag::Texture)) {
            auto* textureNode = static_cast<RDGTextureNode*>(node);
            if (textureNode->_ownership == RDGResourceOwnership::External && !textureNode->_backingHandle.IsValid()) {
                return {false, fmt::format("external texture node '{}' has invalid backing handle", node->_name)};
            }
            if (textureNode->_exportCount > 1) {
                return {false, fmt::format("texture node '{}' exported more than once", node->_name)};
            }
            if (textureNode->_ownership == RDGResourceOwnership::Transient && textureNode->_exportedState.has_value()) {
                return {false, fmt::format("transient texture node '{}' was exported", node->_name)};
            }
        }
    }

    for (size_t i = 0; i < _edges.size(); ++i) {
        auto* edge = _edges[i].get();
        if (edge == nullptr) {
            return {false, fmt::format("{}: null edge", i)};
        }
        if (edge->_from == nullptr || edge->_to == nullptr) {
            return {false, fmt::format("{}: edge has null endpoint", i)};
        }

        const bool fromPass = edge->_from->GetTag().HasFlag(RDGNodeTag::Pass);
        const bool toPass = edge->_to->GetTag().HasFlag(RDGNodeTag::Pass);
        if (fromPass == toPass) {
            return {false, fmt::format(
                               "invalid edge '{}' -> '{}': only pass-resource links are allowed",
                               edge->_from->_name,
                               edge->_to->_name)};
        }

        auto* resource = fromPass ? edge->_to : edge->_from;
        if (!resource->GetTag().HasFlag(RDGNodeTag::Buffer) &&
            !resource->GetTag().HasFlag(RDGNodeTag::Texture)) {
            return {false, fmt::format(
                               "edge '{}' -> '{}' is bound to a non-resource node",
                               edge->_from->_name,
                               edge->_to->_name)};
        }
    }

    return {true, {}};
}

std::string_view format_as(RDGNodeTag v) noexcept {
    switch (v) {
        case RDGNodeTag::UNKNOWN: return "UNKNOWN";
        case RDGNodeTag::Resource: return "Resource";
        case RDGNodeTag::Buffer: return "Buffer";
        case RDGNodeTag::Texture: return "Texture";
        case RDGNodeTag::Pass: return "Pass";
        case RDGNodeTag::GraphicsPass: return "Direct";
        case RDGNodeTag::ComputePass: return "Compute";
        case RDGNodeTag::CopyPass: return "Copy";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(RDGExecutionStage v) noexcept {
    switch (v) {
        case RDGExecutionStage::NONE: return "NONE";
        case RDGExecutionStage::VertexInput: return "VertexInput";
        case RDGExecutionStage::VertexShader: return "VertexShader";
        case RDGExecutionStage::PixelShader: return "PixelShader";
        case RDGExecutionStage::DepthStencil: return "DepthStencil";
        case RDGExecutionStage::ColorOutput: return "ColorOutput";
        case RDGExecutionStage::Indirect: return "Indirect";
        case RDGExecutionStage::ComputeShader: return "ComputeShader";
        case RDGExecutionStage::Copy: return "Copy";
        case RDGExecutionStage::Host: return "Host";
        case RDGExecutionStage::Present: return "Present";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(RDGMemoryAccess v) noexcept {
    switch (v) {
        case RDGMemoryAccess::NONE: return "NONE";
        case RDGMemoryAccess::VertexRead: return "VertexRead";
        case RDGMemoryAccess::IndexRead: return "IndexRead";
        case RDGMemoryAccess::ConstantRead: return "ConstantRead";
        case RDGMemoryAccess::ShaderRead: return "ShaderRead";
        case RDGMemoryAccess::ShaderWrite: return "ShaderWrite";
        case RDGMemoryAccess::ColorAttachmentRead: return "ColorAttachmentRead";
        case RDGMemoryAccess::ColorAttachmentWrite: return "ColorAttachmentWrite";
        case RDGMemoryAccess::DepthStencilRead: return "DepthStencilRead";
        case RDGMemoryAccess::DepthStencilWrite: return "DepthStencilWrite";
        case RDGMemoryAccess::TransferRead: return "TransferRead";
        case RDGMemoryAccess::TransferWrite: return "TransferWrite";
        case RDGMemoryAccess::HostRead: return "HostRead";
        case RDGMemoryAccess::HostWrite: return "HostWrite";
        case RDGMemoryAccess::IndirectRead: return "IndirectRead";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(RDGTextureLayout v) noexcept {
    switch (v) {
        case RDGTextureLayout::UNKNOWN: return "UNKNOWN";
        case RDGTextureLayout::Undefined: return "Undefined";
        case RDGTextureLayout::General: return "General";
        case RDGTextureLayout::ShaderReadOnly: return "ShaderReadOnly";
        case RDGTextureLayout::ColorAttachment: return "ColorAttachment";
        case RDGTextureLayout::DepthStencilReadOnly: return "DepthStencilReadOnly";
        case RDGTextureLayout::DepthStencilAttachment: return "DepthStencilAttachment";
        case RDGTextureLayout::TransferSource: return "TransferSource";
        case RDGTextureLayout::TransferDestination: return "TransferDestination";
        case RDGTextureLayout::Present: return "Present";
    }
    Unreachable();
}

std::string_view format_as(RDGResourceOwnership v) noexcept {
    switch (v) {
        case RDGResourceOwnership::UNKNOWN: return "UNKNOWN";
        case RDGResourceOwnership::External: return "External";
        case RDGResourceOwnership::Internal: return "Internal";
        case RDGResourceOwnership::Transient: return "Transient";
    }
    Unreachable();
}

}  // namespace radray
