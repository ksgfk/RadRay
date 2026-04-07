#include <radray/runtime/render_graph.h>

#include <algorithm>
#include <iterator>
#include <optional>

#include <fmt/format.h>

#include <radray/utility.h>

namespace radray {

namespace {

void _AppendGraphvizEscapedText(fmt::memory_buffer& buffer, std::string_view text) {
    auto out = std::back_inserter(buffer);
    size_t chunkBegin = 0;
    for (size_t index = 0; index < text.size(); ++index) {
        std::string_view replacement{};
        switch (text[index]) {
            case '\\': replacement = "\\\\"; break;
            case '"': replacement = "\\\""; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = ""; break;
            default: break;
        }
        if (replacement.data() == nullptr) {
            continue;
        }
        if (index > chunkBegin) {
            fmt::format_to(out, "{}", std::string_view{text.data() + chunkBegin, index - chunkBegin});
        }
        if (!replacement.empty()) {
            fmt::format_to(out, "{}", replacement);
        }
        chunkBegin = index + 1;
    }
    if (chunkBegin < text.size()) {
        fmt::format_to(out, "{}", std::string_view{text.data() + chunkBegin, text.size() - chunkBegin});
    }
}

template <typename T>
struct FormatGraphviz;

template <>
struct FormatGraphviz<RDGResourceNode> {
    static void AppendNodeAttributes(fmt::memory_buffer& buffer, const RDGResourceNode& node) {
        (void)buffer;
        (void)node;
    }

    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGResourceNode& node) {
        fmt::format_to(std::back_inserter(buffer), "\\nkind: Resource\\nownership: {}", node._ownership);
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGResourceNode& node) {
        (void)buffer;
        (void)node;
    }
};

template <typename TState>
void _AppendResourceImportExportStatements(fmt::memory_buffer& buffer, const RDGResourceNode& node, const std::optional<TState>& importState, const std::optional<TState>& exportState) {
    if (importState.has_value()) {
        auto out = std::back_inserter(buffer);
        fmt::format_to(out, "    n{}_import [shape=oval, style=dashed, label=\"kind: Import\\nownership: {}\"];\n", node._id, node._ownership);
        fmt::format_to(out, "    n{}_import -> n{} [label=\"stage: {}\\naccess: {}\"];\n", node._id, node._id, importState->Stage, importState->Access);
    }
    if (exportState.has_value()) {
        auto out = std::back_inserter(buffer);
        fmt::format_to(out, "    n{}_export [shape=oval, style=dashed, label=\"kind: Export\\nownership: {}\"];\n", node._id, node._ownership);
        fmt::format_to(out, "    n{} -> n{}_export [label=\"stage: {}\\naccess: {}\"];\n", node._id, node._id, exportState->Stage, exportState->Access);
    }
}

template <>
struct FormatGraphviz<RDGBufferNode> : FormatGraphviz<RDGResourceNode> {
    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGBufferNode& node) {
        FormatGraphviz<RDGResourceNode>::AppendNodeLabel(buffer, node);
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGBufferNode& node) {
        _AppendResourceImportExportStatements(buffer, node, node._importState, node._exportState);
    }
};

template <>
struct FormatGraphviz<RDGTextureNode> : FormatGraphviz<RDGResourceNode> {
    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGTextureNode& node) {
        FormatGraphviz<RDGResourceNode>::AppendNodeLabel(buffer, node);
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGTextureNode& node) {
        _AppendResourceImportExportStatements(buffer, node, node._importState, node._exportState);
    }
};

template <>
struct FormatGraphviz<RDGPassNode> {
    static void AppendNodeAttributes(fmt::memory_buffer& buffer, const RDGPassNode& node) {
        (void)node;
        fmt::format_to(std::back_inserter(buffer), "shape=ellipse, ");
    }

    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGPassNode& node) {
        (void)node;
        fmt::format_to(std::back_inserter(buffer), "\\nkind: Pass");
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGPassNode& node) {
        (void)buffer;
        (void)node;
    }
};

template <>
struct FormatGraphviz<RDGGraphicsPassNode> : FormatGraphviz<RDGPassNode> {
    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGGraphicsPassNode& node) {
        FormatGraphviz<RDGPassNode>::AppendNodeLabel(buffer, node);
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGGraphicsPassNode& node) {
        FormatGraphviz<RDGPassNode>::AppendExtraStatements(buffer, node);
    }
};

template <>
struct FormatGraphviz<RDGComputePassNode> : FormatGraphviz<RDGPassNode> {
    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGComputePassNode& node) {
        FormatGraphviz<RDGPassNode>::AppendNodeLabel(buffer, node);
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGComputePassNode& node) {
        FormatGraphviz<RDGPassNode>::AppendExtraStatements(buffer, node);
    }
};

template <>
struct FormatGraphviz<RDGCopyPassNode> : FormatGraphviz<RDGPassNode> {
    static void AppendNodeLabel(fmt::memory_buffer& buffer, const RDGCopyPassNode& node) {
        FormatGraphviz<RDGPassNode>::AppendNodeLabel(buffer, node);
    }

    static void AppendExtraStatements(fmt::memory_buffer& buffer, const RDGCopyPassNode& node) {
        FormatGraphviz<RDGPassNode>::AppendExtraStatements(buffer, node);
    }
};

template <typename Visitor>
decltype(auto) VisitGraphvizNode(const RDGNode& node, Visitor&& visitor) {
    switch (static_cast<RDGNodeTag>(node.GetTag())) {
        case RDGNodeTag::Resource:
            return visitor(static_cast<const RDGResourceNode&>(node));
        case RDGNodeTag::Buffer:
            return visitor(static_cast<const RDGBufferNode&>(node));
        case RDGNodeTag::Texture:
            return visitor(static_cast<const RDGTextureNode&>(node));
        case RDGNodeTag::Pass:
            return visitor(static_cast<const RDGPassNode&>(node));
        case RDGNodeTag::GraphicsPass:
            return visitor(static_cast<const RDGGraphicsPassNode&>(node));
        case RDGNodeTag::ComputePass:
            return visitor(static_cast<const RDGComputePassNode&>(node));
        case RDGNodeTag::CopyPass:
            return visitor(static_cast<const RDGCopyPassNode&>(node));
        case RDGNodeTag::UNKNOWN:
        default:
            break;
    }
    Unreachable();
}

template <>
struct FormatGraphviz<RDGResourceDependencyEdge> {
    static void AppendEdgeAttributes(fmt::memory_buffer& buffer, const RDGResourceDependencyEdge& edge) {
        (void)buffer;
        (void)edge;
    }

    static void AppendEdgeLabel(fmt::memory_buffer& buffer, const RDGResourceDependencyEdge& edge) {
        fmt::format_to(std::back_inserter(buffer), "label=\"stage: {}\\naccess: {}\"", edge._stage, edge._access);
    }
};

template <>
struct FormatGraphviz<RDGPassDependencyEdge> {
    static void AppendEdgeAttributes(fmt::memory_buffer& buffer, const RDGPassDependencyEdge& edge) {
        (void)edge;
        fmt::format_to(std::back_inserter(buffer), "style=dashed, ");
    }

    static void AppendEdgeLabel(fmt::memory_buffer& buffer, const RDGPassDependencyEdge& edge) {
        (void)edge;
        fmt::format_to(std::back_inserter(buffer), "label=\"kind: PassDependency\"");
    }
};

template <typename Visitor>
decltype(auto) VisitGraphvizEdge(const RDGEdge& edge, Visitor&& visitor) {
    switch (static_cast<RDGEdgeTag>(edge.GetTag())) {
        case RDGEdgeTag::ResourceDependency:
            return visitor(static_cast<const RDGResourceDependencyEdge&>(edge));
        case RDGEdgeTag::PassDependency:
            return visitor(static_cast<const RDGPassDependencyEdge&>(edge));
        case RDGEdgeTag::UNKNOWN:
        default:
            break;
    }
    Unreachable();
}

struct NormalizedBufferRange {
    uint64_t Offset{0};
    uint64_t Size{0};
    bool IsWholeResource{false};
};

struct NormalizedTextureRange {
    uint32_t BaseArrayLayer{0};
    uint32_t ArrayLayerCount{0};
    uint32_t BaseMipLevel{0};
    uint32_t MipLevelCount{0};
    bool IsWholeResource{false};
};

bool _IsResourceNode(const RDGNode* node) noexcept {
    return node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Resource);
}

bool _IsPassNode(const RDGNode* node) noexcept {
    return node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Pass);
}

bool _IsBufferNode(const RDGNode* node) noexcept {
    return node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Buffer);
}

bool _IsTextureNode(const RDGNode* node) noexcept {
    return node != nullptr && node->GetTag().HasFlag(RDGNodeTag::Texture);
}

bool _IsDefaultBufferRange(const render::BufferRange& range) noexcept {
    return range.Offset == 0 && range.Size == 0;
}

bool _IsDefaultTextureRange(const render::SubresourceRange& range) noexcept {
    return range.BaseArrayLayer == 0 &&
           range.ArrayLayerCount == 0 &&
           range.BaseMipLevel == 0 &&
           range.MipLevelCount == 0;
}

}  // namespace

bool IsReadAccessFlag(RDGMemoryAccess access) noexcept {
    switch (access) {
        case RDGMemoryAccess::VertexRead:
        case RDGMemoryAccess::IndexRead:
        case RDGMemoryAccess::ConstantRead:
        case RDGMemoryAccess::ShaderRead:
        case RDGMemoryAccess::ColorAttachmentRead:
        case RDGMemoryAccess::DepthStencilRead:
        case RDGMemoryAccess::TransferRead:
        case RDGMemoryAccess::HostRead:
        case RDGMemoryAccess::IndirectRead:
            return true;
        case RDGMemoryAccess::NONE:
        case RDGMemoryAccess::ShaderWrite:
        case RDGMemoryAccess::ColorAttachmentWrite:
        case RDGMemoryAccess::DepthStencilWrite:
        case RDGMemoryAccess::TransferWrite:
        case RDGMemoryAccess::HostWrite:
            return false;
    }
    Unreachable();
}

bool IsWriteAccessFlag(RDGMemoryAccess access) noexcept {
    switch (access) {
        case RDGMemoryAccess::ShaderWrite:
        case RDGMemoryAccess::ColorAttachmentWrite:
        case RDGMemoryAccess::DepthStencilWrite:
        case RDGMemoryAccess::TransferWrite:
        case RDGMemoryAccess::HostWrite:
            return true;
        case RDGMemoryAccess::NONE:
        case RDGMemoryAccess::VertexRead:
        case RDGMemoryAccess::IndexRead:
        case RDGMemoryAccess::ConstantRead:
        case RDGMemoryAccess::ShaderRead:
        case RDGMemoryAccess::ColorAttachmentRead:
        case RDGMemoryAccess::DepthStencilRead:
        case RDGMemoryAccess::TransferRead:
        case RDGMemoryAccess::HostRead:
        case RDGMemoryAccess::IndirectRead:
            return false;
    }
    Unreachable();
}

bool HasReadAccess(RDGMemoryAccesses access) noexcept {
    return access.HasFlag(RDGMemoryAccess::VertexRead) ||
           access.HasFlag(RDGMemoryAccess::IndexRead) ||
           access.HasFlag(RDGMemoryAccess::ConstantRead) ||
           access.HasFlag(RDGMemoryAccess::ShaderRead) ||
           access.HasFlag(RDGMemoryAccess::ColorAttachmentRead) ||
           access.HasFlag(RDGMemoryAccess::DepthStencilRead) ||
           access.HasFlag(RDGMemoryAccess::TransferRead) ||
           access.HasFlag(RDGMemoryAccess::HostRead) ||
           access.HasFlag(RDGMemoryAccess::IndirectRead);
}

bool HasWriteAccess(RDGMemoryAccesses access) noexcept {
    return access.HasFlag(RDGMemoryAccess::ShaderWrite) ||
           access.HasFlag(RDGMemoryAccess::ColorAttachmentWrite) ||
           access.HasFlag(RDGMemoryAccess::DepthStencilWrite) ||
           access.HasFlag(RDGMemoryAccess::TransferWrite) ||
           access.HasFlag(RDGMemoryAccess::HostWrite);
}

bool IsReadOnlyAccess(RDGMemoryAccesses access) noexcept {
    return HasReadAccess(access) && !HasWriteAccess(access);
}

RDGMemoryAccesses AllowedAccessesForStage(RDGExecutionStage stage) noexcept {
    switch (stage) {
        case RDGExecutionStage::NONE: return RDGMemoryAccess::NONE;
        case RDGExecutionStage::VertexInput: return RDGMemoryAccess::VertexRead | RDGMemoryAccess::IndexRead;
        case RDGExecutionStage::VertexShader: return RDGMemoryAccess::ConstantRead | RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite;
        case RDGExecutionStage::PixelShader: return RDGMemoryAccess::ConstantRead | RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite;
        case RDGExecutionStage::DepthStencil: return RDGMemoryAccess::DepthStencilRead | RDGMemoryAccess::DepthStencilWrite;
        case RDGExecutionStage::ColorOutput: return RDGMemoryAccess::ColorAttachmentRead | RDGMemoryAccess::ColorAttachmentWrite;
        case RDGExecutionStage::Indirect: return RDGMemoryAccess::IndirectRead;
        case RDGExecutionStage::ComputeShader: return RDGMemoryAccess::ConstantRead | RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite;
        case RDGExecutionStage::Copy: return RDGMemoryAccess::TransferRead | RDGMemoryAccess::TransferWrite;
        case RDGExecutionStage::Host: return RDGMemoryAccess::HostRead | RDGMemoryAccess::HostWrite;
        case RDGExecutionStage::Present: return RDGMemoryAccess::HostRead;
    }
    Unreachable();
}

RDGMemoryAccesses AllowedAccessesForStages(RDGExecutionStages stages) noexcept {
    RDGMemoryAccesses allowed{RDGMemoryAccess::NONE};
    if (stages.HasFlag(RDGExecutionStage::VertexInput)) allowed |= AllowedAccessesForStage(RDGExecutionStage::VertexInput);
    if (stages.HasFlag(RDGExecutionStage::VertexShader)) allowed |= AllowedAccessesForStage(RDGExecutionStage::VertexShader);
    if (stages.HasFlag(RDGExecutionStage::PixelShader)) allowed |= AllowedAccessesForStage(RDGExecutionStage::PixelShader);
    if (stages.HasFlag(RDGExecutionStage::DepthStencil)) allowed |= AllowedAccessesForStage(RDGExecutionStage::DepthStencil);
    if (stages.HasFlag(RDGExecutionStage::ColorOutput)) allowed |= AllowedAccessesForStage(RDGExecutionStage::ColorOutput);
    if (stages.HasFlag(RDGExecutionStage::Indirect)) allowed |= AllowedAccessesForStage(RDGExecutionStage::Indirect);
    if (stages.HasFlag(RDGExecutionStage::ComputeShader)) allowed |= AllowedAccessesForStage(RDGExecutionStage::ComputeShader);
    if (stages.HasFlag(RDGExecutionStage::Copy)) allowed |= AllowedAccessesForStage(RDGExecutionStage::Copy);
    if (stages.HasFlag(RDGExecutionStage::Host)) allowed |= AllowedAccessesForStage(RDGExecutionStage::Host);
    if (stages.HasFlag(RDGExecutionStage::Present)) allowed |= AllowedAccessesForStage(RDGExecutionStage::Present);
    return allowed;
}

bool AreLayoutsCompatible(RDGTextureLayout lhs, RDGTextureLayout rhs) noexcept {
    return lhs == rhs || lhs == RDGTextureLayout::General || rhs == RDGTextureLayout::General;
}

bool IsTextureLayoutCompatibleWithAccess(RDGTextureLayout layout, RDGMemoryAccesses access) noexcept {
    switch (layout) {
        case RDGTextureLayout::UNKNOWN:
        case RDGTextureLayout::Undefined:
            return false;
        case RDGTextureLayout::General:
            return true;
        case RDGTextureLayout::ShaderReadOnly:
            return !HasWriteAccess(access) &&
                   !access.HasFlag(RDGMemoryAccess::VertexRead) &&
                   !access.HasFlag(RDGMemoryAccess::IndexRead) &&
                   !access.HasFlag(RDGMemoryAccess::ColorAttachmentRead) &&
                   !access.HasFlag(RDGMemoryAccess::ColorAttachmentWrite) &&
                   !access.HasFlag(RDGMemoryAccess::DepthStencilRead) &&
                   !access.HasFlag(RDGMemoryAccess::DepthStencilWrite) &&
                   !access.HasFlag(RDGMemoryAccess::TransferRead) &&
                   !access.HasFlag(RDGMemoryAccess::TransferWrite) &&
                   !access.HasFlag(RDGMemoryAccess::IndirectRead) &&
                   !access.HasFlag(RDGMemoryAccess::HostWrite);
        case RDGTextureLayout::ColorAttachment:
            return !access.HasFlag(RDGMemoryAccess::VertexRead) &&
                   !access.HasFlag(RDGMemoryAccess::IndexRead) &&
                   !access.HasFlag(RDGMemoryAccess::ConstantRead) &&
                   !access.HasFlag(RDGMemoryAccess::ShaderRead) &&
                   !access.HasFlag(RDGMemoryAccess::ShaderWrite) &&
                   !access.HasFlag(RDGMemoryAccess::DepthStencilRead) &&
                   !access.HasFlag(RDGMemoryAccess::DepthStencilWrite) &&
                   !access.HasFlag(RDGMemoryAccess::TransferRead) &&
                   !access.HasFlag(RDGMemoryAccess::TransferWrite) &&
                   !access.HasFlag(RDGMemoryAccess::HostRead) &&
                   !access.HasFlag(RDGMemoryAccess::HostWrite) &&
                   !access.HasFlag(RDGMemoryAccess::IndirectRead) &&
                   (HasReadAccess(access) || HasWriteAccess(access)) &&
                   !(access & ~(RDGMemoryAccess::ColorAttachmentRead | RDGMemoryAccess::ColorAttachmentWrite));
        case RDGTextureLayout::DepthStencilReadOnly:
            return access == RDGMemoryAccess::DepthStencilRead;
        case RDGTextureLayout::DepthStencilAttachment:
            return !access.HasFlag(RDGMemoryAccess::VertexRead) &&
                   !access.HasFlag(RDGMemoryAccess::IndexRead) &&
                   !access.HasFlag(RDGMemoryAccess::ConstantRead) &&
                   !access.HasFlag(RDGMemoryAccess::ShaderRead) &&
                   !access.HasFlag(RDGMemoryAccess::ShaderWrite) &&
                   !access.HasFlag(RDGMemoryAccess::ColorAttachmentRead) &&
                   !access.HasFlag(RDGMemoryAccess::ColorAttachmentWrite) &&
                   !access.HasFlag(RDGMemoryAccess::TransferRead) &&
                   !access.HasFlag(RDGMemoryAccess::TransferWrite) &&
                   !access.HasFlag(RDGMemoryAccess::HostRead) &&
                   !access.HasFlag(RDGMemoryAccess::HostWrite) &&
                   !access.HasFlag(RDGMemoryAccess::IndirectRead) &&
                   (HasReadAccess(access) || HasWriteAccess(access)) &&
                   !(access & ~(RDGMemoryAccess::DepthStencilRead | RDGMemoryAccess::DepthStencilWrite));
        case RDGTextureLayout::TransferSource:
            return access == RDGMemoryAccess::TransferRead;
        case RDGTextureLayout::TransferDestination:
            return access == RDGMemoryAccess::TransferWrite;
        case RDGTextureLayout::Present:
            return !HasWriteAccess(access);
    }
    Unreachable();
}

namespace {

const RDGResourceNode* _GetResourceNode(const RDGResourceDependencyEdge& edge) noexcept {
    if (_IsResourceNode(edge._from)) return static_cast<const RDGResourceNode*>(edge._from);
    if (_IsResourceNode(edge._to)) return static_cast<const RDGResourceNode*>(edge._to);
    return nullptr;
}

const RDGPassNode* _GetPassNode(const RDGResourceDependencyEdge& edge) noexcept {
    if (_IsPassNode(edge._from)) return static_cast<const RDGPassNode*>(edge._from);
    if (_IsPassNode(edge._to)) return static_cast<const RDGPassNode*>(edge._to);
    return nullptr;
}

bool _NormalizeBufferRange(const render::BufferRange& range, std::optional<uint64_t> totalSize, NormalizedBufferRange* normalized) noexcept {
    if (normalized == nullptr) return false;
    if (range.Size == 0) return false;
    if (!totalSize.has_value()) {
        normalized->Offset = range.Offset;
        normalized->Size = range.Size == render::BufferRange::All() ? 0 : range.Size;
        normalized->IsWholeResource = range.Size == render::BufferRange::All();
        return true;
    }
    if (range.Offset > *totalSize) return false;
    const uint64_t size = range.Size == render::BufferRange::All() ? (*totalSize - range.Offset) : range.Size;
    if (size == 0) return false;
    if (range.Offset > *totalSize - size) return false;
    normalized->Offset = range.Offset;
    normalized->Size = size;
    normalized->IsWholeResource = range.Offset == 0 && size == *totalSize;
    return true;
}

bool _NormalizeTextureRange(const render::SubresourceRange& range, std::optional<uint32_t> arrayCount, std::optional<uint32_t> mipCount, NormalizedTextureRange* normalized) noexcept {
    if (normalized == nullptr) return false;
    if (range.ArrayLayerCount == 0 || range.MipLevelCount == 0) return false;
    const bool wholeLayers = range.ArrayLayerCount == render::SubresourceRange::All;
    const bool wholeMips = range.MipLevelCount == render::SubresourceRange::All;
    if (!arrayCount.has_value() || !mipCount.has_value()) {
        normalized->BaseArrayLayer = range.BaseArrayLayer;
        normalized->ArrayLayerCount = wholeLayers ? 0 : range.ArrayLayerCount;
        normalized->BaseMipLevel = range.BaseMipLevel;
        normalized->MipLevelCount = wholeMips ? 0 : range.MipLevelCount;
        normalized->IsWholeResource = wholeLayers || wholeMips;
        return true;
    }
    if (range.BaseArrayLayer >= *arrayCount || range.BaseMipLevel >= *mipCount) return false;
    const uint32_t normalizedArrayCount = wholeLayers ? (*arrayCount - range.BaseArrayLayer) : range.ArrayLayerCount;
    const uint32_t normalizedMipCount = wholeMips ? (*mipCount - range.BaseMipLevel) : range.MipLevelCount;
    if (normalizedArrayCount == 0 || normalizedMipCount == 0) return false;
    if (range.BaseArrayLayer > *arrayCount - normalizedArrayCount) return false;
    if (range.BaseMipLevel > *mipCount - normalizedMipCount) return false;
    normalized->BaseArrayLayer = range.BaseArrayLayer;
    normalized->ArrayLayerCount = normalizedArrayCount;
    normalized->BaseMipLevel = range.BaseMipLevel;
    normalized->MipLevelCount = normalizedMipCount;
    normalized->IsWholeResource = range.BaseArrayLayer == 0 &&
                                  normalizedArrayCount == *arrayCount &&
                                  range.BaseMipLevel == 0 &&
                                  normalizedMipCount == *mipCount;
    return true;
}

bool _BufferRangesOverlap(const NormalizedBufferRange& lhs, const NormalizedBufferRange& rhs) noexcept {
    if (lhs.IsWholeResource || rhs.IsWholeResource) return true;
    return lhs.Offset < rhs.Offset + rhs.Size && rhs.Offset < lhs.Offset + lhs.Size;
}

bool _TextureRangesOverlap(const NormalizedTextureRange& lhs, const NormalizedTextureRange& rhs) noexcept {
    if (lhs.IsWholeResource || rhs.IsWholeResource) return true;
    const bool layerOverlap = lhs.BaseArrayLayer < rhs.BaseArrayLayer + rhs.ArrayLayerCount &&
                              rhs.BaseArrayLayer < lhs.BaseArrayLayer + lhs.ArrayLayerCount;
    const bool mipOverlap = lhs.BaseMipLevel < rhs.BaseMipLevel + rhs.MipLevelCount &&
                            rhs.BaseMipLevel < lhs.BaseMipLevel + lhs.MipLevelCount;
    return layerOverlap && mipOverlap;
}

}  // namespace

bool RDGColorAttachmentRecord::HasWriteAccess() const noexcept {
    return Load == render::LoadAction::Clear || Store == render::StoreAction::Store;
}

bool RDGDepthStencilAttachmentRecord::HasWriteAccess() const noexcept {
    return DepthLoad == render::LoadAction::Clear ||
           StencilLoad == render::LoadAction::Clear ||
           DepthStore == render::StoreAction::Store ||
           StencilStore == render::StoreAction::Store;
}

bool RDGBufferState::HasWrite() const noexcept {
    return Access.HasFlag(RDGMemoryAccess::ShaderWrite) ||
           Access.HasFlag(RDGMemoryAccess::ColorAttachmentWrite) ||
           Access.HasFlag(RDGMemoryAccess::DepthStencilWrite) ||
           Access.HasFlag(RDGMemoryAccess::TransferWrite) ||
           Access.HasFlag(RDGMemoryAccess::HostWrite);
}

bool RDGTextureState::HasWrite() const noexcept {
    return Access.HasFlag(RDGMemoryAccess::ShaderWrite) ||
           Access.HasFlag(RDGMemoryAccess::ColorAttachmentWrite) ||
           Access.HasFlag(RDGMemoryAccess::DepthStencilWrite) ||
           Access.HasFlag(RDGMemoryAccess::TransferWrite) ||
           Access.HasFlag(RDGMemoryAccess::HostWrite);
}

RDGBufferHandle RenderGraph::AddBuffer(uint64_t size, render::MemoryType memory, render::BufferUses usage, std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGBufferNode>(id, name, RDGResourceOwnership::Internal);
    node->_size = size;
    node->_memory = memory;
    node->_usage = usage;
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGBufferHandle{raw->_id};
}

RDGTextureHandle RenderGraph::AddTexture(
    render::TextureDimension dim,
    uint32_t width, uint32_t height,
    uint32_t depthOrArraySize, uint32_t mipLevels,
    uint32_t sampleCount,
    render::TextureFormat format,
    render::MemoryType memory,
    render::TextureUses usage,
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
    node->_memory = memory;
    node->_usage = usage;
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGTextureHandle{raw->_id};
}

RDGBufferHandle RenderGraph::ImportBuffer(GpuBufferHandle buffer, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange, std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGBufferNode>(id, name, RDGResourceOwnership::External);
    node->_importBuffer = buffer;
    node->_importState = RDGBufferState{stage, access, bufferRange};
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGBufferHandle{raw->_id};
}

RDGTextureHandle RenderGraph::ImportTexture(GpuTextureHandle texture, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange, std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGTextureNode>(id, name, RDGResourceOwnership::External);
    node->_importTexture = texture;
    node->_importState = RDGTextureState{stage, access, layout, textureRange};
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGTextureHandle{raw->_id};
}

void RenderGraph::ExportBuffer(RDGBufferHandle node, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange) {
    auto base = _nodes[node.Id].get();
    RADRAY_ASSERT(base->GetTag().HasFlag(RDGNodeTag::Buffer));
    auto bufferNode = static_cast<RDGBufferNode*>(base);
    bufferNode->_exportState = RDGBufferState{stage, access, bufferRange};
}

void RenderGraph::ExportTexture(RDGTextureHandle node, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange) {
    auto base = _nodes[node.Id].get();
    RADRAY_ASSERT(base->GetTag().HasFlag(RDGNodeTag::Texture));
    auto textureNode = static_cast<RDGTextureNode*>(base);
    textureNode->_exportState = RDGTextureState{stage, access, layout, textureRange};
}

RDGPassHandle RenderGraph::AddRasterPass(std::string_view name, unique_ptr<IRDGRasterPass> pass) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGGraphicsPassNode>(id, name);
    node->_impl = std::move(pass);
    IRDGRasterPass::Builder builder{};
    node->_impl->Setup(builder);
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    builder.Build(this, raw);
    return RDGPassHandle{raw->_id};
}

RDGPassHandle RenderGraph::AddComputePass(std::string_view name, unique_ptr<IRDGComputePass> pass) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGComputePassNode>(id, name);
    node->_impl = std::move(pass);
    IRDGComputePass::Builder builder{};
    node->_impl->Setup(builder);
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    builder.Build(this, raw);
    return RDGPassHandle{raw->_id};
}

RDGPassHandle RenderGraph::AddCopyPass(std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGCopyPassNode>(id, name);
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGPassHandle{raw->_id};
}

void RenderGraph::AddPassDependency(RDGPassHandle before, RDGPassHandle after) {
    RADRAY_ASSERT(before.IsValid() && after.IsValid());
    auto* from = Resolve(before);
    auto* to = Resolve(after);
    RADRAY_ASSERT(from->GetTag().HasFlag(RDGNodeTag::Pass));
    RADRAY_ASSERT(to->GetTag().HasFlag(RDGNodeTag::Pass));
    auto edge = make_unique<RDGPassDependencyEdge>(from, to);
    auto* raw = edge.get();
    _edges.emplace_back(std::move(edge));
    from->_outEdges.emplace_back(raw);
    to->_inEdges.emplace_back(raw);
}

RDGEdge* RenderGraph::Link(RDGNodeHandle from_, RDGNodeHandle to_, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange) {
    auto* from = _nodes[from_.Id].get();
    auto* to = _nodes[to_.Id].get();
    RADRAY_ASSERT(
        (from->GetTag().HasFlag(RDGNodeTag::Pass) && to->GetTag().HasFlag(RDGNodeTag::Resource)) ||
        (from->GetTag().HasFlag(RDGNodeTag::Resource) && to->GetTag().HasFlag(RDGNodeTag::Pass)));
    auto edge = make_unique<RDGResourceDependencyEdge>(from, to, stage, access);
    edge->_bufferRange = bufferRange;
    auto* raw = edge.get();
    _edges.emplace_back(std::move(edge));
    from->_outEdges.emplace_back(raw);
    to->_inEdges.emplace_back(raw);
    return raw;
}

RDGEdge* RenderGraph::Link(RDGNodeHandle from_, RDGNodeHandle to_, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange) {
    auto* from = _nodes[from_.Id].get();
    auto* to = _nodes[to_.Id].get();
    RADRAY_ASSERT(
        (from->GetTag().HasFlag(RDGNodeTag::Pass) && to->GetTag().HasFlag(RDGNodeTag::Resource)) ||
        (from->GetTag().HasFlag(RDGNodeTag::Resource) && to->GetTag().HasFlag(RDGNodeTag::Pass)));
    auto edge = make_unique<RDGResourceDependencyEdge>(from, to, stage, access);
    edge->_textureLayout = layout;
    edge->_textureRange = textureRange;
    auto* raw = edge.get();
    _edges.emplace_back(std::move(edge));
    from->_outEdges.emplace_back(raw);
    to->_inEdges.emplace_back(raw);
    return raw;
}

RenderGraph::ValidateResult RenderGraph::Validate() const {
    auto fail = [](string message) -> ValidateResult {
        return ValidateResult{false, std::move(message)};
    };
    auto formatNode = [](const RDGNode* node) -> string {
        if (node == nullptr) return "<null node>";
        return fmt::format("node {} '{}'", node->_id, node->_name);
    };
    auto formatEdge = [&](const RDGEdge* edge) -> string {
        if (edge == nullptr) return "<null edge>";
        return fmt::format("{} -> {}", formatNode(edge->_from), formatNode(edge->_to));
    };
    auto tryResolveNode = [&](RDGNodeHandle handle) -> const RDGNode* {
        if (!handle.IsValid() || handle.Id >= _nodes.size()) return nullptr;
        return _nodes[handle.Id].get();
    };

    unordered_set<const RDGNode*> nodeSet{};
    nodeSet.reserve(_nodes.size());
    for (const auto& node : _nodes) {
        if (node) {
            nodeSet.emplace(node.get());
        }
    }

    unordered_set<const RDGEdge*> edgeSet{};
    edgeSet.reserve(_edges.size());
    for (const auto& edge : _edges) {
        if (edge) {
            edgeSet.emplace(edge.get());
        }
    }

    for (size_t index = 0; index < _nodes.size(); ++index) {
        const auto& node = _nodes[index];
        if (!node) {
            return fail(fmt::format("render graph validate failed: node slot {} is null", index));
        }
        if (!node->GetHandle().IsValid() || node->_id >= _nodes.size()) {
            return fail(fmt::format("render graph validate failed: {} has invalid handle/id", formatNode(node.get())));
        }
        if (node->_id != index) {
            return fail(fmt::format("render graph validate failed: {} id/index mismatch, expected {}", formatNode(node.get()), index));
        }
        if (node->GetTag() == RDGNodeTag::UNKNOWN) {
            return fail(fmt::format("render graph validate failed: {} has UNKNOWN tag", formatNode(node.get())));
        }
        if (_IsResourceNode(node.get()) && !_IsBufferNode(node.get()) && !_IsTextureNode(node.get())) {
            return fail(fmt::format("render graph validate failed: {} resource tag is not concrete", formatNode(node.get())));
        }
        if (_IsPassNode(node.get()) &&
            node->GetTag() != RDGNodeTag::GraphicsPass &&
            node->GetTag() != RDGNodeTag::ComputePass &&
            node->GetTag() != RDGNodeTag::CopyPass) {
            return fail(fmt::format("render graph validate failed: {} pass tag is not concrete", formatNode(node.get())));
        }
    }

    vector<uint32_t> indegree(_nodes.size(), 0);
    vector<vector<uint64_t>> adjacency(_nodes.size());
    vector<vector<uint64_t>> resourceOnlyAdjacency(_nodes.size());
    vector<uint32_t> resourceDependencyEdgeCount(_nodes.size(), 0);
    vector<uint32_t> passWriteEdgeCount(_nodes.size(), 0);
    vector<bool> resourceHasWriteSource(_nodes.size(), false);
    vector<const RDGResourceDependencyEdge*> resourceEdges{};
    vector<const RDGPassDependencyEdge*> passEdges{};
    unordered_set<string> resourceEdgeKeys{};
    unordered_set<string> passDependencyKeys{};

    for (const auto& edgeHolder : _edges) {
        if (!edgeHolder) {
            return fail("render graph validate failed: edge container has null entry");
        }
        const RDGEdge* edge = edgeHolder.get();
        if (edge->_from == nullptr || edge->_to == nullptr) {
            return fail(fmt::format("render graph validate failed: edge {} has null endpoint", formatEdge(edge)));
        }
        if (!nodeSet.contains(edge->_from) || !nodeSet.contains(edge->_to)) {
            return fail(fmt::format("render graph validate failed: edge {} references node outside graph", formatEdge(edge)));
        }
        if (edge->_from == edge->_to) {
            return fail(fmt::format("render graph validate failed: self loop on {}", formatNode(edge->_from)));
        }
        if (edge->GetTag() == RDGEdgeTag::UNKNOWN) {
            return fail(fmt::format("render graph validate failed: edge {} has UNKNOWN tag", formatEdge(edge)));
        }
        if (std::find(edge->_from->_outEdges.begin(), edge->_from->_outEdges.end(), edge) == edge->_from->_outEdges.end()) {
            return fail(fmt::format("render graph validate failed: edge {} missing from source out-edges", formatEdge(edge)));
        }
        if (std::find(edge->_to->_inEdges.begin(), edge->_to->_inEdges.end(), edge) == edge->_to->_inEdges.end()) {
            return fail(fmt::format("render graph validate failed: edge {} missing from target in-edges", formatEdge(edge)));
        }

        adjacency[edge->_from->_id].emplace_back(edge->_to->_id);
        ++indegree[edge->_to->_id];

        if (edge->GetTag() == RDGEdgeTag::ResourceDependency) {
            auto* resourceEdge = static_cast<const RDGResourceDependencyEdge*>(edge);
            resourceEdges.emplace_back(resourceEdge);
            resourceOnlyAdjacency[edge->_from->_id].emplace_back(edge->_to->_id);
            ++resourceDependencyEdgeCount[edge->_from->_id];
            ++resourceDependencyEdgeCount[edge->_to->_id];

            if (!(_IsResourceNode(edge->_from) ^ _IsResourceNode(edge->_to))) {
                return fail(fmt::format("render graph validate failed: resource dependency edge {} must connect one resource and one pass", formatEdge(edge)));
            }
            if (resourceEdge->_stage == RDGExecutionStage::NONE) {
                return fail(fmt::format("render graph validate failed: resource dependency edge {} has NONE stage", formatEdge(edge)));
            }
            if (resourceEdge->_access == RDGMemoryAccess::NONE) {
                return fail(fmt::format("render graph validate failed: resource dependency edge {} has NONE access", formatEdge(edge)));
            }

            const RDGResourceNode* resourceNode = _GetResourceNode(*resourceEdge);
            const RDGPassNode* passNode = _GetPassNode(*resourceEdge);
            if (resourceNode == nullptr || passNode == nullptr) {
                return fail(fmt::format("render graph validate failed: resource dependency edge {} cannot resolve typed endpoints", formatEdge(edge)));
            }

            if (IsReadOnlyAccess(resourceEdge->_access)) {
                if (!(edge->_from == resourceNode && edge->_to == passNode)) {
                    return fail(fmt::format("render graph validate failed: read-only edge {} must point Resource -> Pass", formatEdge(edge)));
                }
            } else if (HasWriteAccess(resourceEdge->_access)) {
                if (!(edge->_from == passNode && edge->_to == resourceNode)) {
                    return fail(fmt::format("render graph validate failed: write edge {} must point Pass -> Resource", formatEdge(edge)));
                }
                ++passWriteEdgeCount[passNode->_id];
                resourceHasWriteSource[resourceNode->_id] = true;
            }

            if (_IsBufferNode(resourceNode)) {
                if (_IsDefaultBufferRange(resourceEdge->_bufferRange)) {
                    return fail(fmt::format("render graph validate failed: buffer edge {} has default buffer range", formatEdge(edge)));
                }
                if (resourceEdge->_textureLayout != RDGTextureLayout::UNKNOWN || !_IsDefaultTextureRange(resourceEdge->_textureRange)) {
                    return fail(fmt::format("render graph validate failed: buffer edge {} carries texture state", formatEdge(edge)));
                }
            } else if (_IsTextureNode(resourceNode)) {
                if (resourceEdge->_textureLayout == RDGTextureLayout::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: texture edge {} has UNKNOWN layout", formatEdge(edge)));
                }
                if (_IsDefaultTextureRange(resourceEdge->_textureRange)) {
                    return fail(fmt::format("render graph validate failed: texture edge {} has default subresource range", formatEdge(edge)));
                }
                if (!_IsDefaultBufferRange(resourceEdge->_bufferRange)) {
                    return fail(fmt::format("render graph validate failed: texture edge {} carries buffer range", formatEdge(edge)));
                }
            }

            const string duplicateKey = fmt::format(
                "{}:{}:{}:{}:{}:{}:{}:{}:{}:{}:{}",
                edge->_from->_id,
                edge->_to->_id,
                resourceEdge->_stage.value(),
                resourceEdge->_access.value(),
                resourceEdge->_bufferRange.Offset,
                resourceEdge->_bufferRange.Size,
                static_cast<int32_t>(resourceEdge->_textureLayout),
                resourceEdge->_textureRange.BaseArrayLayer,
                resourceEdge->_textureRange.ArrayLayerCount,
                resourceEdge->_textureRange.BaseMipLevel,
                resourceEdge->_textureRange.MipLevelCount);
            if (!resourceEdgeKeys.emplace(duplicateKey).second) {
                return fail(fmt::format("render graph validate failed: duplicate resource dependency edge {}", formatEdge(edge)));
            }
        } else if (edge->GetTag() == RDGEdgeTag::PassDependency) {
            auto* passEdge = static_cast<const RDGPassDependencyEdge*>(edge);
            passEdges.emplace_back(passEdge);
            if (!_IsPassNode(edge->_from) || !_IsPassNode(edge->_to)) {
                return fail(fmt::format("render graph validate failed: pass dependency edge {} must connect two passes", formatEdge(edge)));
            }
            const string duplicateKey = fmt::format("{}:{}", edge->_from->_id, edge->_to->_id);
            if (!passDependencyKeys.emplace(duplicateKey).second) {
                return fail(fmt::format("render graph validate failed: duplicate pass dependency edge {}", formatEdge(edge)));
            }
        }
    }

    for (const auto& node : _nodes) {
        for (RDGEdge* edge : node->_inEdges) {
            if (edge == nullptr || !edgeSet.contains(edge)) {
                return fail(fmt::format("render graph validate failed: {} has in-edge not owned by graph", formatNode(node.get())));
            }
            if (edge->_to != node.get()) {
                return fail(fmt::format("render graph validate failed: {} has in-edge with mismatched destination", formatNode(node.get())));
            }
        }
        for (RDGEdge* edge : node->_outEdges) {
            if (edge == nullptr || !edgeSet.contains(edge)) {
                return fail(fmt::format("render graph validate failed: {} has out-edge not owned by graph", formatNode(node.get())));
            }
            if (edge->_from != node.get()) {
                return fail(fmt::format("render graph validate failed: {} has out-edge with mismatched source", formatNode(node.get())));
            }
        }
    }

    vector<uint64_t> topoOrder{};
    topoOrder.reserve(_nodes.size());
    vector<uint32_t> pendingIndegree = indegree;
    vector<uint64_t> ready{};
    ready.reserve(_nodes.size());
    for (uint64_t nodeId = 0; nodeId < _nodes.size(); ++nodeId) {
        if (pendingIndegree[nodeId] == 0) {
            ready.emplace_back(nodeId);
        }
    }
    for (size_t cursor = 0; cursor < ready.size(); ++cursor) {
        const uint64_t nodeId = ready[cursor];
        topoOrder.emplace_back(nodeId);
        for (uint64_t next : adjacency[nodeId]) {
            --pendingIndegree[next];
            if (pendingIndegree[next] == 0) {
                ready.emplace_back(next);
            }
        }
    }
    if (topoOrder.size() != _nodes.size()) {
        return fail("render graph validate failed: graph contains a cycle");
    }

    vector<bool> reachable(_nodes.size(), false);
    vector<uint64_t> visitQueue{};
    visitQueue.reserve(_nodes.size());
    for (const auto& node : _nodes) {
        if (!_IsResourceNode(node.get())) {
            continue;
        }
        const auto* resourceNode = static_cast<const RDGResourceNode*>(node.get());
        if (resourceNode->_ownership == RDGResourceOwnership::External || node->_inEdges.empty()) {
            if (!reachable[node->_id]) {
                reachable[node->_id] = true;
                visitQueue.emplace_back(node->_id);
            }
        }
    }
    for (size_t cursor = 0; cursor < visitQueue.size(); ++cursor) {
        for (uint64_t next : adjacency[visitQueue[cursor]]) {
            if (!reachable[next]) {
                reachable[next] = true;
                visitQueue.emplace_back(next);
            }
        }
    }
    for (const auto& node : _nodes) {
        if (_IsPassNode(node.get()) && !reachable[node->_id]) {
            return fail(fmt::format("render graph validate failed: {} is not reachable from any import/root resource", formatNode(node.get())));
        }
        if (_IsPassNode(node.get()) && resourceDependencyEdgeCount[node->_id] == 0) {
            return fail(fmt::format("render graph validate failed: {} has no resource dependency edge", formatNode(node.get())));
        }
        if (_IsResourceNode(node.get()) && resourceDependencyEdgeCount[node->_id] == 0) {
            if (_IsTextureNode(node.get())) {
                const auto* textureNode = static_cast<const RDGTextureNode*>(node.get());
                if (textureNode->_importState.has_value() &&
                    textureNode->_exportState.has_value() &&
                    textureNode->_importState->Layout != textureNode->_exportState->Layout) {
                    return fail(fmt::format("render graph validate failed: imported/exported texture {} changes layout without any pass usage", formatNode(node.get())));
                }
            }
            return fail(fmt::format("render graph validate failed: {} is not referenced by any pass", formatNode(node.get())));
        }
    }

    vector<const RDGPassNode*> passNodes{};
    passNodes.reserve(_nodes.size());
    unordered_map<uint64_t, uint32_t> passIndexByNodeId{};
    for (const auto& node : _nodes) {
        if (_IsPassNode(node.get())) {
            passIndexByNodeId.emplace(node->_id, static_cast<uint32_t>(passNodes.size()));
            passNodes.emplace_back(static_cast<const RDGPassNode*>(node.get()));
        }
    }

    auto buildPassReachability = [&](const vector<vector<uint64_t>>& adj) {
        vector<vector<uint8_t>> result(passNodes.size(), vector<uint8_t>(passNodes.size(), 0));
        vector<uint8_t> visited(_nodes.size(), 0);
        vector<uint64_t> queue{};
        queue.reserve(_nodes.size());
        for (uint32_t passIndex = 0; passIndex < passNodes.size(); ++passIndex) {
            std::fill(visited.begin(), visited.end(), uint8_t{0});
            queue.clear();
            visited[passNodes[passIndex]->_id] = 1;
            queue.emplace_back(passNodes[passIndex]->_id);
            for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
                const uint64_t nodeId = queue[cursor];
                for (uint64_t next : adj[nodeId]) {
                    if (visited[next]) {
                        continue;
                    }
                    visited[next] = 1;
                    queue.emplace_back(next);
                    if (_IsPassNode(_nodes[next].get())) {
                        result[passIndex][passIndexByNodeId[next]] = 1;
                    }
                }
            }
        }
        return result;
    };

    const auto fullPassReachability = buildPassReachability(adjacency);
    const auto resourceOnlyPassReachability = buildPassReachability(resourceOnlyAdjacency);
    auto buildPassReachabilitySkippingNode = [&](uint64_t skippedNodeId) {
        vector<vector<uint8_t>> result(passNodes.size(), vector<uint8_t>(passNodes.size(), 0));
        vector<uint8_t> visited(_nodes.size(), 0);
        vector<uint64_t> queue{};
        queue.reserve(_nodes.size());
        for (uint32_t passIndex = 0; passIndex < passNodes.size(); ++passIndex) {
            std::fill(visited.begin(), visited.end(), uint8_t{0});
            queue.clear();
            if (passNodes[passIndex]->_id == skippedNodeId) {
                continue;
            }
            visited[passNodes[passIndex]->_id] = 1;
            queue.emplace_back(passNodes[passIndex]->_id);
            for (size_t cursor = 0; cursor < queue.size(); ++cursor) {
                const uint64_t nodeId = queue[cursor];
                for (uint64_t next : adjacency[nodeId]) {
                    if (next == skippedNodeId || visited[next]) {
                        continue;
                    }
                    visited[next] = 1;
                    queue.emplace_back(next);
                    if (_IsPassNode(_nodes[next].get())) {
                        result[passIndex][passIndexByNodeId[next]] = 1;
                    }
                }
            }
        }
        return result;
    };

    struct UsageRecord {
        const RDGPassNode* Pass{nullptr};
        const RDGResourceDependencyEdge* Edge{nullptr};
        bool IsWrite{false};
        bool IsTexture{false};
        NormalizedBufferRange BufferRange{};
        NormalizedTextureRange TextureRange{};
    };

    unordered_map<uint64_t, vector<UsageRecord>> usagesByResource{};
    usagesByResource.reserve(_nodes.size());

    for (const auto& resourceEdge : resourceEdges) {
        const RDGResourceNode* resourceNode = _GetResourceNode(*resourceEdge);
        const RDGPassNode* passNode = _GetPassNode(*resourceEdge);
        const RDGMemoryAccesses allowedAccess = AllowedAccessesForStages(resourceEdge->_stage);
        if (!allowedAccess.HasFlag(resourceEdge->_access)) {
            return fail(fmt::format(
                "render graph validate failed: resource edge {} uses access {} incompatible with stage {}",
                formatEdge(resourceEdge),
                resourceEdge->_access,
                resourceEdge->_stage));
        }

        UsageRecord usage{};
        usage.Pass = passNode;
        usage.Edge = resourceEdge;
        usage.IsWrite = HasWriteAccess(resourceEdge->_access);
        usage.IsTexture = _IsTextureNode(resourceNode);

        if (_IsBufferNode(resourceNode)) {
            const auto* bufferNode = static_cast<const RDGBufferNode*>(resourceNode);
            std::optional<uint64_t> totalSize{};
            if (bufferNode->_ownership != RDGResourceOwnership::External && bufferNode->_size > 0) {
                totalSize = bufferNode->_size;
            }
            if (!_NormalizeBufferRange(resourceEdge->_bufferRange, totalSize, &usage.BufferRange)) {
                return fail(fmt::format("render graph validate failed: buffer edge {} range is invalid", formatEdge(resourceEdge)));
            }
        } else {
            const auto* textureNode = static_cast<const RDGTextureNode*>(resourceNode);
            std::optional<uint32_t> arrayCount{};
            std::optional<uint32_t> mipCount{};
            if (textureNode->_ownership != RDGResourceOwnership::External &&
                textureNode->_depthOrArraySize > 0 &&
                textureNode->_mipLevels > 0) {
                arrayCount = textureNode->_depthOrArraySize;
                mipCount = textureNode->_mipLevels;
            }
            if (!_NormalizeTextureRange(resourceEdge->_textureRange, arrayCount, mipCount, &usage.TextureRange)) {
                return fail(fmt::format("render graph validate failed: texture edge {} subresource range is invalid", formatEdge(resourceEdge)));
            }
            if (!IsTextureLayoutCompatibleWithAccess(resourceEdge->_textureLayout, resourceEdge->_access)) {
                return fail(fmt::format(
                    "render graph validate failed: texture edge {} layout {} is incompatible with access {}",
                    formatEdge(resourceEdge),
                    resourceEdge->_textureLayout,
                    resourceEdge->_access));
            }
            if (textureNode->_format != render::TextureFormat::UNKNOWN &&
                render::IsDepthStencilFormat(textureNode->_format) &&
                resourceEdge->_textureLayout != RDGTextureLayout::DepthStencilAttachment &&
                resourceEdge->_textureLayout != RDGTextureLayout::DepthStencilReadOnly &&
                resourceEdge->_textureLayout != RDGTextureLayout::General) {
                return fail(fmt::format(
                    "render graph validate failed: depth texture edge {} uses illegal layout {}",
                    formatEdge(resourceEdge),
                    resourceEdge->_textureLayout));
            }
        }

        usagesByResource[resourceNode->_id].emplace_back(usage);
    }

    for (const auto& node : _nodes) {
        if (!_IsResourceNode(node.get())) {
            continue;
        }
        const auto* resourceNode = static_cast<const RDGResourceNode*>(node.get());
        if (resourceNode->_ownership == RDGResourceOwnership::UNKNOWN) {
            return fail(fmt::format("render graph validate failed: {} has UNKNOWN ownership", formatNode(node.get())));
        }

        if (_IsBufferNode(node.get())) {
            const auto* bufferNode = static_cast<const RDGBufferNode*>(node.get());
            if (bufferNode->_ownership == RDGResourceOwnership::External) {
                if (!bufferNode->_importState.has_value()) {
                    return fail(fmt::format("render graph validate failed: external buffer {} has no import state", formatNode(node.get())));
                }
                if (!bufferNode->_importBuffer.IsValid()) {
                    return fail(fmt::format("render graph validate failed: external buffer {} has invalid import handle", formatNode(node.get())));
                }
            } else {
                if (bufferNode->_importState.has_value() || bufferNode->_importBuffer.IsValid()) {
                    return fail(fmt::format("render graph validate failed: internal/transient buffer {} must not carry import state", formatNode(node.get())));
                }
                if (bufferNode->_size == 0) {
                    return fail(fmt::format("render graph validate failed: buffer {} size must be > 0", formatNode(node.get())));
                }
                if (bufferNode->_usage == render::BufferUse::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: buffer {} usage is UNKNOWN", formatNode(node.get())));
                }
            }

            if (bufferNode->_importState.has_value()) {
                if (bufferNode->_importState->Stage == RDGExecutionStage::NONE || bufferNode->_importState->Access == RDGMemoryAccess::NONE) {
                    return fail(fmt::format("render graph validate failed: buffer import state on {} is incomplete", formatNode(node.get())));
                }
                if (!AllowedAccessesForStages(bufferNode->_importState->Stage).HasFlag(bufferNode->_importState->Access)) {
                    return fail(fmt::format("render graph validate failed: buffer import state on {} has incompatible stage/access", formatNode(node.get())));
                }
            }
            if (bufferNode->_exportState.has_value()) {
                if (bufferNode->_exportState->Stage == RDGExecutionStage::NONE || bufferNode->_exportState->Access == RDGMemoryAccess::NONE) {
                    return fail(fmt::format("render graph validate failed: buffer export state on {} is incomplete", formatNode(node.get())));
                }
                if (!AllowedAccessesForStages(bufferNode->_exportState->Stage).HasFlag(bufferNode->_exportState->Access)) {
                    return fail(fmt::format("render graph validate failed: buffer export state on {} has incompatible stage/access", formatNode(node.get())));
                }
                if (bufferNode->_ownership != RDGResourceOwnership::External && !resourceHasWriteSource[node->_id]) {
                    return fail(fmt::format("render graph validate failed: exported buffer {} has no pass write source", formatNode(node.get())));
                }
            }
        } else if (_IsTextureNode(node.get())) {
            const auto* textureNode = static_cast<const RDGTextureNode*>(node.get());
            if (textureNode->_ownership == RDGResourceOwnership::External) {
                if (!textureNode->_importState.has_value()) {
                    return fail(fmt::format("render graph validate failed: external texture {} has no import state", formatNode(node.get())));
                }
                if (!textureNode->_importTexture.IsValid()) {
                    return fail(fmt::format("render graph validate failed: external texture {} has invalid import handle", formatNode(node.get())));
                }
            } else {
                if (textureNode->_importState.has_value() || textureNode->_importTexture.IsValid()) {
                    return fail(fmt::format("render graph validate failed: internal/transient texture {} must not carry import state", formatNode(node.get())));
                }
                if (textureNode->_dim == render::TextureDimension::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: texture {} dimension is UNKNOWN", formatNode(node.get())));
                }
                if (textureNode->_width == 0 ||
                    textureNode->_height == 0 ||
                    textureNode->_depthOrArraySize == 0 ||
                    textureNode->_mipLevels == 0 ||
                    textureNode->_sampleCount == 0) {
                    return fail(fmt::format("render graph validate failed: texture {} has invalid extent/mip/sample values", formatNode(node.get())));
                }
                if (textureNode->_format == render::TextureFormat::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: texture {} format is UNKNOWN", formatNode(node.get())));
                }
                if (textureNode->_usage == render::TextureUse::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: texture {} usage is UNKNOWN", formatNode(node.get())));
                }
            }

            if (textureNode->_importState.has_value()) {
                if (textureNode->_importState->Stage == RDGExecutionStage::NONE || textureNode->_importState->Access == RDGMemoryAccess::NONE) {
                    return fail(fmt::format("render graph validate failed: texture import state on {} is incomplete", formatNode(node.get())));
                }
                if (textureNode->_importState->Layout == RDGTextureLayout::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: texture import state on {} has UNKNOWN layout", formatNode(node.get())));
                }
                if (!AllowedAccessesForStages(textureNode->_importState->Stage).HasFlag(textureNode->_importState->Access)) {
                    return fail(fmt::format("render graph validate failed: texture import state on {} has incompatible stage/access", formatNode(node.get())));
                }
                if (!IsTextureLayoutCompatibleWithAccess(textureNode->_importState->Layout, textureNode->_importState->Access)) {
                    return fail(fmt::format("render graph validate failed: texture import state on {} has incompatible layout/access", formatNode(node.get())));
                }
            }
            if (textureNode->_exportState.has_value()) {
                if (textureNode->_exportState->Stage == RDGExecutionStage::NONE || textureNode->_exportState->Access == RDGMemoryAccess::NONE) {
                    return fail(fmt::format("render graph validate failed: texture export state on {} is incomplete", formatNode(node.get())));
                }
                if (textureNode->_exportState->Layout == RDGTextureLayout::UNKNOWN) {
                    return fail(fmt::format("render graph validate failed: texture export state on {} has UNKNOWN layout", formatNode(node.get())));
                }
                if (!AllowedAccessesForStages(textureNode->_exportState->Stage).HasFlag(textureNode->_exportState->Access)) {
                    return fail(fmt::format("render graph validate failed: texture export state on {} has incompatible stage/access", formatNode(node.get())));
                }
                if (!IsTextureLayoutCompatibleWithAccess(textureNode->_exportState->Layout, textureNode->_exportState->Access)) {
                    return fail(fmt::format("render graph validate failed: texture export state on {} has incompatible layout/access", formatNode(node.get())));
                }
                if (textureNode->_ownership != RDGResourceOwnership::External && !resourceHasWriteSource[node->_id]) {
                    return fail(fmt::format("render graph validate failed: exported texture {} has no pass write source", formatNode(node.get())));
                }
            }
            if (textureNode->_importState.has_value() && textureNode->_exportState.has_value() && resourceDependencyEdgeCount[node->_id] == 0) {
                if (textureNode->_importState->Layout != textureNode->_exportState->Layout) {
                    return fail(fmt::format("render graph validate failed: imported/exported texture {} changes layout without any pass usage", formatNode(node.get())));
                }
            }
        }
    }

    const RDGExecutionStages graphicsStages =
        RDGExecutionStage::VertexInput |
        RDGExecutionStage::VertexShader |
        RDGExecutionStage::PixelShader |
        RDGExecutionStage::DepthStencil |
        RDGExecutionStage::ColorOutput |
        RDGExecutionStage::Indirect;

    for (const auto& node : _nodes) {
        if (!_IsPassNode(node.get())) {
            continue;
        }
        if (passWriteEdgeCount[node->_id] == 0) {
            return fail(fmt::format("render graph validate failed: {} has no resource write output", formatNode(node.get())));
        }

        vector<const RDGResourceDependencyEdge*> passResourceEdges{};
        vector<const RDGResourceDependencyEdge*> passTextureEdges{};
        passResourceEdges.reserve(node->_inEdges.size() + node->_outEdges.size());
        passTextureEdges.reserve(node->_inEdges.size() + node->_outEdges.size());

        auto collectPassEdge = [&](RDGEdge* edge) -> std::optional<ValidateResult> {
            if (edge == nullptr || edge->GetTag() != RDGEdgeTag::ResourceDependency) {
                return std::nullopt;
            }
            auto* resourceEdge = static_cast<const RDGResourceDependencyEdge*>(edge);
            if (_GetPassNode(*resourceEdge) != node.get()) {
                return fail(fmt::format("render graph validate failed: pass edge {} is registered on the wrong pass", formatEdge(resourceEdge)));
            }
            passResourceEdges.emplace_back(resourceEdge);
            if (_IsTextureNode(_GetResourceNode(*resourceEdge))) {
                passTextureEdges.emplace_back(resourceEdge);
            }
            return std::nullopt;
        };

        for (RDGEdge* edge : node->_inEdges) {
            if (auto result = collectPassEdge(edge); result.has_value()) {
                return std::move(result).value();
            }
        }
        for (RDGEdge* edge : node->_outEdges) {
            if (auto result = collectPassEdge(edge); result.has_value()) {
                return std::move(result).value();
            }
        }

        if (node->GetTag() == RDGNodeTag::GraphicsPass) {
            const auto* graphicsPass = static_cast<const RDGGraphicsPassNode*>(node.get());
            if (graphicsPass->_impl == nullptr) {
                return fail(fmt::format("render graph validate failed: graphics pass {} has null implementation", formatNode(node.get())));
            }

            vector<uint32_t> slots{};
            slots.reserve(graphicsPass->_colorAttachments.size());
            unordered_set<uint64_t> attachmentTextures{};
            attachmentTextures.reserve(graphicsPass->_colorAttachments.size());
            for (const auto& attachment : graphicsPass->_colorAttachments) {
                const RDGNode* attachmentNode = tryResolveNode(attachment.Texture);
                if (attachmentNode == nullptr || !_IsTextureNode(attachmentNode)) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} has invalid color attachment handle {}", formatNode(node.get()), attachment.Texture.Id));
                }
                const auto* textureNode = static_cast<const RDGTextureNode*>(attachmentNode);
                if (textureNode->_format != render::TextureFormat::UNKNOWN && render::IsDepthStencilFormat(textureNode->_format)) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} uses depth format texture as color attachment", formatNode(node.get())));
                }
                if (attachment.Load == render::LoadAction::Clear && !attachment.ClearValue.has_value()) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} color attachment slot {} clears without clear value", formatNode(node.get()), attachment.Slot));
                }
                slots.emplace_back(attachment.Slot);
                attachmentTextures.emplace(textureNode->_id);
            }
            std::sort(slots.begin(), slots.end());
            for (size_t slotIndex = 0; slotIndex < slots.size(); ++slotIndex) {
                if (slotIndex > 0 && slots[slotIndex] == slots[slotIndex - 1]) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} has duplicate color attachment slot {}", formatNode(node.get()), slots[slotIndex]));
                }
                if (slots[slotIndex] != slotIndex) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} color attachment slots are not contiguous from 0", formatNode(node.get())));
                }
            }

            if (graphicsPass->_depthStencilAttachment.has_value()) {
                const auto& attachment = graphicsPass->_depthStencilAttachment.value();
                const RDGNode* attachmentNode = tryResolveNode(attachment.Texture);
                if (attachmentNode == nullptr || !_IsTextureNode(attachmentNode)) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} has invalid depth-stencil attachment handle {}", formatNode(node.get()), attachment.Texture.Id));
                }
                const auto* textureNode = static_cast<const RDGTextureNode*>(attachmentNode);
                if (textureNode->_format != render::TextureFormat::UNKNOWN && !render::IsDepthStencilFormat(textureNode->_format)) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} depth-stencil attachment must use depth format", formatNode(node.get())));
                }
                if (attachmentTextures.contains(textureNode->_id)) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} uses same texture as color and depth-stencil attachment", formatNode(node.get())));
                }
                if ((attachment.DepthLoad == render::LoadAction::Clear || attachment.StencilLoad == render::LoadAction::Clear) &&
                    !attachment.ClearValue.has_value()) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} depth-stencil attachment clears without clear value", formatNode(node.get())));
                }
            }

            for (const auto* resourceEdge : passResourceEdges) {
                if (!graphicsStages.HasFlag(resourceEdge->_stage)) {
                    return fail(fmt::format("render graph validate failed: graphics pass {} uses non-graphics stage {}", formatNode(node.get()), resourceEdge->_stage));
                }
            }
        } else if (node->GetTag() == RDGNodeTag::ComputePass) {
            const auto* computePass = static_cast<const RDGComputePassNode*>(node.get());
            if (computePass->_impl == nullptr) {
                return fail(fmt::format("render graph validate failed: compute pass {} has null implementation", formatNode(node.get())));
            }
            for (const auto* resourceEdge : passResourceEdges) {
                if (resourceEdge->_stage != RDGExecutionStage::ComputeShader) {
                    return fail(fmt::format("render graph validate failed: compute pass {} uses stage {}", formatNode(node.get()), resourceEdge->_stage));
                }
                if (resourceEdge->_access.HasFlag(RDGMemoryAccess::VertexRead) ||
                    resourceEdge->_access.HasFlag(RDGMemoryAccess::IndexRead) ||
                    resourceEdge->_access.HasFlag(RDGMemoryAccess::ColorAttachmentRead) ||
                    resourceEdge->_access.HasFlag(RDGMemoryAccess::ColorAttachmentWrite) ||
                    resourceEdge->_access.HasFlag(RDGMemoryAccess::DepthStencilRead) ||
                    resourceEdge->_access.HasFlag(RDGMemoryAccess::DepthStencilWrite) ||
                    resourceEdge->_access.HasFlag(RDGMemoryAccess::IndirectRead)) {
                    return fail(fmt::format("render graph validate failed: compute pass {} uses graphics-only access {}", formatNode(node.get()), resourceEdge->_access));
                }
            }
        } else if (node->GetTag() == RDGNodeTag::CopyPass) {
            const auto* copyPass = static_cast<const RDGCopyPassNode*>(node.get());
            if (copyPass->_copys.empty()) {
                return fail(fmt::format("render graph validate failed: copy pass {} has no copy record", formatNode(node.get())));
            }
            for (const auto* resourceEdge : passResourceEdges) {
                if (resourceEdge->_stage != RDGExecutionStage::Copy) {
                    return fail(fmt::format("render graph validate failed: copy pass {} uses stage {}", formatNode(node.get()), resourceEdge->_stage));
                }
                if (!(resourceEdge->_access == RDGMemoryAccess::TransferRead || resourceEdge->_access == RDGMemoryAccess::TransferWrite)) {
                    return fail(fmt::format("render graph validate failed: copy pass {} uses non-transfer access {}", formatNode(node.get()), resourceEdge->_access));
                }
            }
            for (const auto& copyRecord : copyPass->_copys) {
                if (const auto* bufferToBuffer = std::get_if<RDGCopyBufferToBufferRecord>(&copyRecord)) {
                    const RDGNode* dst = tryResolveNode(bufferToBuffer->Dst);
                    const RDGNode* src = tryResolveNode(bufferToBuffer->Src);
                    if (dst == nullptr || !_IsBufferNode(dst) || src == nullptr || !_IsBufferNode(src)) {
                        return fail(fmt::format("render graph validate failed: copy pass {} has invalid buffer-to-buffer record", formatNode(node.get())));
                    }
                    if (bufferToBuffer->Size == 0) {
                        return fail(fmt::format("render graph validate failed: copy pass {} has zero-sized buffer copy", formatNode(node.get())));
                    }
                    if (bufferToBuffer->Dst == bufferToBuffer->Src &&
                        bufferToBuffer->DstOffset < bufferToBuffer->SrcOffset + bufferToBuffer->Size &&
                        bufferToBuffer->SrcOffset < bufferToBuffer->DstOffset + bufferToBuffer->Size) {
                        return fail(fmt::format("render graph validate failed: copy pass {} copies overlapping ranges within the same buffer", formatNode(node.get())));
                    }
                } else if (const auto* bufferToTexture = std::get_if<RDGCopyBufferToTextureRecord>(&copyRecord)) {
                    const RDGNode* dst = tryResolveNode(bufferToTexture->Dst);
                    const RDGNode* src = tryResolveNode(bufferToTexture->Src);
                    if (dst == nullptr || !_IsTextureNode(dst) || src == nullptr || !_IsBufferNode(src)) {
                        return fail(fmt::format("render graph validate failed: copy pass {} has invalid buffer-to-texture record", formatNode(node.get())));
                    }
                } else if (const auto* textureToBuffer = std::get_if<RDGCopyTextureToBufferRecord>(&copyRecord)) {
                    const RDGNode* dst = tryResolveNode(textureToBuffer->Dst);
                    const RDGNode* src = tryResolveNode(textureToBuffer->Src);
                    if (dst == nullptr || !_IsBufferNode(dst) || src == nullptr || !_IsTextureNode(src)) {
                        return fail(fmt::format("render graph validate failed: copy pass {} has invalid texture-to-buffer record", formatNode(node.get())));
                    }
                }
            }
        }

        for (size_t i = 0; i < passTextureEdges.size(); ++i) {
            const auto* lhsResource = static_cast<const RDGTextureNode*>(_GetResourceNode(*passTextureEdges[i]));
            std::optional<uint32_t> arrayCount{};
            std::optional<uint32_t> mipCount{};
            if (lhsResource->_ownership != RDGResourceOwnership::External &&
                lhsResource->_depthOrArraySize > 0 &&
                lhsResource->_mipLevels > 0) {
                arrayCount = lhsResource->_depthOrArraySize;
                mipCount = lhsResource->_mipLevels;
            }
            NormalizedTextureRange lhsRange{};
            if (!_NormalizeTextureRange(passTextureEdges[i]->_textureRange, arrayCount, mipCount, &lhsRange)) {
                return fail(fmt::format("render graph validate failed: {} has invalid texture range while checking layout consistency", formatNode(node.get())));
            }
            for (size_t j = i + 1; j < passTextureEdges.size(); ++j) {
                const auto* rhsResource = static_cast<const RDGTextureNode*>(_GetResourceNode(*passTextureEdges[j]));
                if (lhsResource->_id != rhsResource->_id) {
                    continue;
                }
                NormalizedTextureRange rhsRange{};
                if (!_NormalizeTextureRange(passTextureEdges[j]->_textureRange, arrayCount, mipCount, &rhsRange)) {
                    return fail(fmt::format("render graph validate failed: {} has invalid texture range while checking layout consistency", formatNode(node.get())));
                }
                if (_TextureRangesOverlap(lhsRange, rhsRange) &&
                    !AreLayoutsCompatible(passTextureEdges[i]->_textureLayout, passTextureEdges[j]->_textureLayout)) {
                    return fail(fmt::format("render graph validate failed: {} uses conflicting layouts on the same texture subresource", formatNode(node.get())));
                }
            }
        }
    }

    for (const auto& [resourceId, usages] : usagesByResource) {
        const auto* resourceNode = static_cast<const RDGResourceNode*>(_nodes[resourceId].get());
        const auto passReachabilitySkippingResource = buildPassReachabilitySkippingNode(resourceId);
        auto hasExternalPassOrder = [&](const RDGPassNode* before, const RDGPassNode* after) {
            return passReachabilitySkippingResource[passIndexByNodeId[before->_id]][passIndexByNodeId[after->_id]] != 0;
        };
        auto usagesOverlap = [](const UsageRecord& lhs, const UsageRecord& rhs) {
            return lhs.IsTexture
                       ? _TextureRangesOverlap(lhs.TextureRange, rhs.TextureRange)
                       : _BufferRangesOverlap(lhs.BufferRange, rhs.BufferRange);
        };
        auto overlappingWriteCountFor = [&](size_t usageIndex) {
            size_t count = 0;
            for (size_t writerIndex = 0; writerIndex < usages.size(); ++writerIndex) {
                if (!usages[writerIndex].IsWrite) {
                    continue;
                }
                if (!usagesOverlap(usages[usageIndex], usages[writerIndex])) {
                    continue;
                }
                ++count;
            }
            return count;
        };

        for (size_t i = 0; i < usages.size(); ++i) {
            const uint32_t lhsPassIndex = passIndexByNodeId[usages[i].Pass->_id];
            if (!usages[i].IsWrite && resourceNode->_ownership != RDGResourceOwnership::External) {
                bool hasWriter = false;
                for (size_t writerIndex = 0; writerIndex < usages.size(); ++writerIndex) {
                    if (!usages[writerIndex].IsWrite || usages[writerIndex].Pass->_id == usages[i].Pass->_id) {
                        continue;
                    }
                    const bool overlap = usagesOverlap(usages[i], usages[writerIndex]);
                    if (overlap && fullPassReachability[passIndexByNodeId[usages[writerIndex].Pass->_id]][lhsPassIndex]) {
                        hasWriter = true;
                        break;
                    }
                }
                if (!hasWriter) {
                    return fail(fmt::format("render graph validate failed: {} is read by {} without an earlier write source", formatNode(resourceNode), formatNode(usages[i].Pass)));
                }
            }
        }

        for (size_t i = 0; i < usages.size(); ++i) {
            if (usages[i].IsWrite) {
                continue;
            }
            for (size_t j = i + 1; j < usages.size(); ++j) {
                if (!usages[j].IsWrite || usages[i].Pass->_id == usages[j].Pass->_id) {
                    continue;
                }
                if (!usagesOverlap(usages[i], usages[j])) {
                    continue;
                }
                const bool useExternalOrder = overlappingWriteCountFor(i) > 1;
                const bool rhsBeforeLhs = useExternalOrder
                                              ? hasExternalPassOrder(usages[j].Pass, usages[i].Pass)
                                              : fullPassReachability[passIndexByNodeId[usages[j].Pass->_id]][passIndexByNodeId[usages[i].Pass->_id]] != 0;
                if (!rhsBeforeLhs) {
                    return fail(fmt::format("render graph validate failed: {} is overwritten by {} before read on {}", formatNode(resourceNode), formatNode(usages[j].Pass), formatNode(usages[i].Pass)));
                }
            }
        }

        for (size_t i = 0; i < usages.size(); ++i) {
            const uint32_t lhsPassIndex = passIndexByNodeId[usages[i].Pass->_id];
            for (size_t j = i + 1; j < usages.size(); ++j) {
                if (usages[i].Pass->_id == usages[j].Pass->_id) {
                    continue;
                }
                const bool overlap = usagesOverlap(usages[i], usages[j]);
                if (!overlap || (!usages[i].IsWrite && !usages[j].IsWrite)) {
                    continue;
                }

                const uint32_t rhsPassIndex = passIndexByNodeId[usages[j].Pass->_id];
                if (usages[i].IsWrite && usages[j].IsWrite) {
                    const bool lhsBeforeRhs = fullPassReachability[lhsPassIndex][rhsPassIndex] != 0;
                    const bool rhsBeforeLhs = fullPassReachability[rhsPassIndex][lhsPassIndex] != 0;
                    if (!lhsBeforeRhs && !rhsBeforeLhs) {
                        return fail(fmt::format("render graph validate failed: {} has unordered overlapping writes from {} and {}", formatNode(resourceNode), formatNode(usages[i].Pass), formatNode(usages[j].Pass)));
                    }
                } else if (usages[i].IsWrite) {
                    const bool useExternalOrder = overlappingWriteCountFor(j) > 1;
                    const bool lhsBeforeRhs = useExternalOrder
                                                  ? hasExternalPassOrder(usages[i].Pass, usages[j].Pass)
                                                  : fullPassReachability[lhsPassIndex][rhsPassIndex] != 0;
                    if (!lhsBeforeRhs) {
                        return fail(fmt::format("render graph validate failed: {} is read by {} before write from {}", formatNode(resourceNode), formatNode(usages[j].Pass), formatNode(usages[i].Pass)));
                    }
                }
            }
        }
    }

    string warningMessage{};
    for (const auto* passEdge : passEdges) {
        const uint32_t beforeIndex = passIndexByNodeId[passEdge->_from->_id];
        const uint32_t afterIndex = passIndexByNodeId[passEdge->_to->_id];
        if (resourceOnlyPassReachability[afterIndex][beforeIndex]) {
            return fail(fmt::format("render graph validate failed: pass dependency {} contradicts resource-derived order", formatEdge(passEdge)));
        }
        if (resourceOnlyPassReachability[beforeIndex][afterIndex]) {
            if (!warningMessage.empty()) {
                warningMessage += '\n';
            }
            warningMessage += fmt::format("render graph validate warning: pass dependency {} is redundant", formatEdge(passEdge));
        }
    }

    return {true, std::move(warningMessage)};
}

RenderGraph::CompileResult RenderGraph::Compile() const {
    // TODO:
    return {};
}

string RenderGraph::ExportGraphviz() const {
    fmt::memory_buffer buffer;
    auto out = std::back_inserter(buffer);
    fmt::format_to(out, "digraph RenderGraph {{\n");
    fmt::format_to(out, "    rankdir=LR;\n");
    fmt::format_to(out, "    node [shape=box];\n");
    for (const auto& node : _nodes) {
        fmt::format_to(out, "    n{} [", node->_id);
        VisitGraphvizNode(*node, [&buffer](const auto& typedNode) {
            using NodeType = std::remove_cvref_t<decltype(typedNode)>;
            FormatGraphviz<NodeType>::AppendNodeAttributes(buffer, typedNode);
        });
        fmt::format_to(out, "label=\"id: {}\\nname: ", node->_id);
        _AppendGraphvizEscapedText(buffer, node->_name);
        VisitGraphvizNode(*node, [&buffer](const auto& typedNode) {
            using NodeType = std::remove_cvref_t<decltype(typedNode)>;
            FormatGraphviz<NodeType>::AppendNodeLabel(buffer, typedNode);
        });
        fmt::format_to(out, "\\ntag: {}\"];\n", node->GetTag());
    }
    for (const auto& edge : _edges) {
        fmt::format_to(out, "    n{} -> n{} [", edge->_from->_id, edge->_to->_id);
        VisitGraphvizEdge(*edge, [&buffer](const auto& typedEdge) {
            using EdgeType = std::remove_cvref_t<decltype(typedEdge)>;
            FormatGraphviz<EdgeType>::AppendEdgeAttributes(buffer, typedEdge);
        });
        VisitGraphvizEdge(*edge, [&buffer](const auto& typedEdge) {
            using EdgeType = std::remove_cvref_t<decltype(typedEdge)>;
            FormatGraphviz<EdgeType>::AppendEdgeLabel(buffer, typedEdge);
        });
        fmt::format_to(out, "];\n");
    }
    for (const auto& node : _nodes) {
        VisitGraphvizNode(*node, [&buffer](const auto& typedNode) {
            using NodeType = std::remove_cvref_t<decltype(typedNode)>;
            FormatGraphviz<NodeType>::AppendExtraStatements(buffer, typedNode);
        });
    }
    fmt::format_to(out, "}}\n");
    return fmt::to_string(buffer);
}

RDGNode* RenderGraph::Resolve(RDGNodeHandle handle) const {
    RADRAY_ASSERT(handle.Id >= 0 && handle.Id < _nodes.size());
    return _nodes[handle.Id].get();
}

RDGExecutionStages RenderGraph::ShaderStagesToExecStages(render::ShaderStages stages) noexcept {
    RDGExecutionStages executionStages{RDGExecutionStage::NONE};
    if (stages.HasFlag(render::ShaderStage::Vertex)) executionStages |= RDGExecutionStage::VertexShader;
    if (stages.HasFlag(render::ShaderStage::Pixel)) executionStages |= RDGExecutionStage::PixelShader;
    return executionStages;
}

RDGGraphicsPassNode::~RDGGraphicsPassNode() noexcept = default;

RDGComputePassNode::~RDGComputePassNode() noexcept = default;

void IRDGRasterPass::Builder::Build(RenderGraph* graph, RDGGraphicsPassNode* node) {
    for (auto& attachment : _colorAttachments) {
        node->_colorAttachments.emplace_back(std::move(attachment));
    }
    _colorAttachments.clear();
    if (_depthStencilAttachment.has_value()) {
        RADRAY_ASSERT(!node->_depthStencilAttachment.has_value());
        node->_depthStencilAttachment = std::move(_depthStencilAttachment);
        _depthStencilAttachment.reset();
    }
    for (const auto& usage : _buffers) {
        if (usage.State.HasWrite()) {
            graph->Link(node->GetHandle(), usage.Buffer, usage.State.Stage, usage.State.Access, usage.State.Range);
        } else {
            graph->Link(usage.Buffer, node->GetHandle(), usage.State.Stage, usage.State.Access, usage.State.Range);
        }
    }
    _buffers.clear();
    for (const auto& usage : _textures) {
        if (usage.State.HasWrite()) {
            graph->Link(node->GetHandle(), usage.Texture, usage.State.Stage, usage.State.Access, usage.State.Layout, usage.State.Range);
        } else {
            graph->Link(usage.Texture, node->GetHandle(), usage.State.Stage, usage.State.Access, usage.State.Layout, usage.State.Range);
        }
    }
    _textures.clear();
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseColorAttachment(
    uint32_t slot,
    RDGTextureHandle texture,
    render::SubresourceRange range,
    render::LoadAction load,
    render::StoreAction store,
    std::optional<render::ColorClearValue> clearValue) {
    _colorAttachments.emplace_back(texture, slot, range, load, store, clearValue);
    RDGMemoryAccesses access{RDGMemoryAccess::ColorAttachmentWrite};
    if (load == render::LoadAction::Load) {
        access |= RDGMemoryAccess::ColorAttachmentRead;
    }
    _textures.emplace_back(texture, RDGTextureState{RDGExecutionStage::ColorOutput, access, RDGTextureLayout::ColorAttachment, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseDepthStencilAttachment(
    RDGTextureHandle texture,
    render::SubresourceRange range,
    render::LoadAction depthLoad, render::StoreAction depthStore,
    render::LoadAction stencilLoad, render::StoreAction stencilStore,
    std::optional<render::DepthStencilClearValue> clearValue) {
    _depthStencilAttachment = RDGDepthStencilAttachmentRecord{
        texture,
        range,
        depthLoad,
        depthStore,
        stencilLoad,
        stencilStore,
        clearValue};
    if (_depthStencilAttachment->HasWriteAccess()) {
        _textures.emplace_back(texture, RDGTextureState{RDGExecutionStage::DepthStencil, RDGMemoryAccess::DepthStencilWrite, RDGTextureLayout::DepthStencilAttachment, range});
    } else {
        _textures.emplace_back(texture, RDGTextureState{RDGExecutionStage::DepthStencil, RDGMemoryAccess::DepthStencilRead, RDGTextureLayout::DepthStencilReadOnly, range});
    }
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseVertexBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _buffers.emplace_back(buffer, RDGBufferState{RDGExecutionStage::VertexInput, RDGMemoryAccess::VertexRead, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseIndexBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _buffers.emplace_back(buffer, RDGBufferState{RDGExecutionStage::VertexInput, RDGMemoryAccess::IndexRead, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseIndirectBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _buffers.emplace_back(buffer, RDGBufferState{RDGExecutionStage::Indirect, RDGMemoryAccess::IndirectRead, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseCBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range) {
    RDGExecutionStages executionStages = RenderGraph::ShaderStagesToExecStages(stages);
    _buffers.emplace_back(buffer, RDGBufferState{executionStages, RDGMemoryAccess::ConstantRead, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range) {
    RDGExecutionStages executionStages = RenderGraph::ShaderStagesToExecStages(stages);
    _buffers.emplace_back(buffer, RDGBufferState{executionStages, RDGMemoryAccess::ShaderRead, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseRWBuffer(RDGBufferHandle buffer, render::ShaderStages stages, render::BufferRange range) {
    RDGExecutionStages executionStages = RenderGraph::ShaderStagesToExecStages(stages);
    _buffers.emplace_back(buffer, RDGBufferState{executionStages, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseTexture(RDGTextureHandle texture, render::ShaderStages stages, render::SubresourceRange range) {
    RDGExecutionStages executionStages = RenderGraph::ShaderStagesToExecStages(stages);
    _textures.emplace_back(texture, RDGTextureState{executionStages, RDGMemoryAccess::ShaderRead, RDGTextureLayout::ShaderReadOnly, range});
    return *this;
}

IRDGRasterPass::Builder& IRDGRasterPass::Builder::UseRWTexture(RDGTextureHandle texture, render::ShaderStages stages, render::SubresourceRange range) {
    RDGExecutionStages executionStages = RenderGraph::ShaderStagesToExecStages(stages);
    _textures.emplace_back(texture, RDGTextureState{executionStages, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, RDGTextureLayout::General, range});
    return *this;
}

void IRDGComputePass::Builder::Build(RenderGraph* graph, RDGComputePassNode* node) {
    for (const auto& usage : _buffers) {
        if (usage.State.HasWrite()) {
            graph->Link(node->GetHandle(), usage.Buffer, usage.State.Stage, usage.State.Access, usage.State.Range);
        } else {
            graph->Link(usage.Buffer, node->GetHandle(), usage.State.Stage, usage.State.Access, usage.State.Range);
        }
    }
    _buffers.clear();
    for (const auto& usage : _textures) {
        if (usage.State.HasWrite()) {
            graph->Link(node->GetHandle(), usage.Texture, usage.State.Stage, usage.State.Access, usage.State.Layout, usage.State.Range);
        } else {
            graph->Link(usage.Texture, node->GetHandle(), usage.State.Stage, usage.State.Access, usage.State.Layout, usage.State.Range);
        }
    }
    _textures.clear();
}

IRDGComputePass::Builder& IRDGComputePass::Builder::UseCBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _buffers.emplace_back(buffer, RDGBufferState{RDGExecutionStage::ComputeShader, RDGMemoryAccess::ConstantRead, range});
    return *this;
}

IRDGComputePass::Builder& IRDGComputePass::Builder::UseBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _buffers.emplace_back(buffer, RDGBufferState{RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, range});
    return *this;
}

IRDGComputePass::Builder& IRDGComputePass::Builder::UseRWBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _buffers.emplace_back(buffer, RDGBufferState{RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, range});
    return *this;
}

IRDGComputePass::Builder& IRDGComputePass::Builder::UseTexture(RDGTextureHandle texture, render::SubresourceRange range) {
    _textures.emplace_back(texture, RDGTextureState{RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead, RDGTextureLayout::ShaderReadOnly, range});
    return *this;
}

IRDGComputePass::Builder& IRDGComputePass::Builder::UseRWTexture(RDGTextureHandle texture, render::SubresourceRange range) {
    _textures.emplace_back(texture, RDGTextureState{RDGExecutionStage::ComputeShader, RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite, RDGTextureLayout::General, range});
    return *this;
}

RDGPassHandle RDGCopyPassBuilder::Build(RenderGraph* graph) {
    auto passHandle = graph->AddCopyPass(_name);
    auto pass = static_cast<RDGCopyPassNode*>(graph->Resolve(passHandle));
    pass->_copys = std::move(_copys);
    for (const auto& usage : _buffers) {
        if (usage.State.HasWrite()) {
            graph->Link(passHandle, usage.Buffer, usage.State.Stage, usage.State.Access, usage.State.Range);
        } else {
            graph->Link(usage.Buffer, passHandle, usage.State.Stage, usage.State.Access, usage.State.Range);
        }
    }
    _buffers.clear();
    for (const auto& usage : _textures) {
        if (usage.State.HasWrite()) {
            graph->Link(passHandle, usage.Texture, usage.State.Stage, usage.State.Access, usage.State.Layout, usage.State.Range);
        } else {
            graph->Link(usage.Texture, passHandle, usage.State.Stage, usage.State.Access, usage.State.Layout, usage.State.Range);
        }
    }
    _textures.clear();
    return passHandle;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::SetName(std::string_view name) {
    _name = name;
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyBufferToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGBufferHandle src, uint64_t srcOffset, uint64_t size) {
    _copys.emplace_back(RDGCopyBufferToBufferRecord{dst, dstOffset, src, srcOffset, size});
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyBufferToTexture(RDGTextureHandle dst, render::SubresourceRange dstRange, RDGBufferHandle src, uint64_t srcOffset) {
    _copys.emplace_back(RDGCopyBufferToTextureRecord{dst, dstRange, src, srcOffset});
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyTextureToBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGTextureHandle src, render::SubresourceRange srcRange) {
    _copys.emplace_back(RDGCopyTextureToBufferRecord{dst, dstOffset, src, srcRange});
    return *this;
}

}  // namespace radray

namespace radray {

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

std::string_view format_as(RDGEdgeTag v) noexcept {
    switch (v) {
        case RDGEdgeTag::UNKNOWN: return "UNKNOWN";
        case RDGEdgeTag::ResourceDependency: return "ResourceDependency";
        case RDGEdgeTag::PassDependency: return "PassDependency";
    }
    Unreachable();
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
