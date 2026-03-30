#include <radray/runtime/render_graph.h>

#include <fmt/format.h>

#include <radray/logger.h>
#include <radray/utility.h>

namespace radray {

namespace {

struct DotNodeStyle {
    std::string_view Shape;
    std::string_view FillColor;
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

void AppendNodeLabel(fmt::memory_buffer& output, const RDGNode& node) {
    AppendDotEscaped(output, node._name);

    if (node.GetTag().HasFlag(RDGNodeTag::Pass)) {
        const auto& passNode = static_cast<const RDGPassNode&>(node);
        fmt::format_to(std::back_inserter(output), "\\nqueue={}", render::format_as(passNode._type));
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

RDGPassHandle RenderGraph::AddPass(std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGPassNode>(id, name, render::QueueType::Direct);
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
    auto node = make_unique<RDGGraphicsPassNode>(id, fmt::format("RasterPass{}", id), render::QueueType::Direct);
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
    _graph->Link(pass, texture, RDGExecutionStage::ColorOutput, RDGMemoryAccess::ColorAttachmentWrite, RDGTextureLayout::ColorAttachment, range);
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
    _graph->Link(
        pass,
        texture,
        RDGExecutionStage::DepthStencil,
        isWrite ? RDGMemoryAccess::DepthStencilWrite : RDGMemoryAccess::DepthStencilRead,
        isWrite ? RDGTextureLayout::DepthStencilAttachment : RDGTextureLayout::DepthStencilReadOnly,
        range);
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
    auto node = make_unique<RDGComputePassNode>(id, fmt::format("ComputePass{}", id), render::QueueType::Compute);
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
    auto node = make_unique<RDGCopyPassNode>(id, fmt::format("CopyPass{}", id), render::QueueType::Copy);
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
    }
    Unreachable();
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
    }
    Unreachable();
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
