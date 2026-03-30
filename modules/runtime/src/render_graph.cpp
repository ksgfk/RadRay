#include <radray/runtime/render_graph.h>

#include <algorithm>

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

RDGCompileResult RenderGraph::Compile() const {
    // 单个资源在单个 pass 内可能被多次使用，这里只记录首尾 usage，
    // 既能推导“进入 pass 前要切到什么状态”，也能得到“pass 结束后资源停在哪个状态”。
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
    unordered_map<const RDGEdge*, uint64_t> edgeIndexByPtr{};
    edgeIndexByPtr.reserve(_edges.size());
    for (uint64_t i = 0; i < _edges.size(); ++i) {
        edgeIndexByPtr.emplace(_edges[i].get(), i);
    }

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

    struct CompiledBufferResource {
        const RDGBufferNode* Node{nullptr};
        vector<BufferPassUsage> PassUsages;
    };

    struct CompiledTextureResource {
        const RDGTextureNode* Node{nullptr};
        vector<TexturePassUsage> PassUsages;
    };

    vector<CompiledBufferResource> compiledBuffers{};
    vector<CompiledTextureResource> compiledTextures{};
    compiledBuffers.reserve(_nodes.size());
    compiledTextures.reserve(_nodes.size());

    // 第一阶段：按资源汇总 usage。
    // 每个资源都会得到一个按 pass 分组的 usage 列表，后面依赖和状态推进都从这里出发。
    for (const auto& nodeHolder : _nodes) {
        auto* node = nodeHolder.get();
        if (node == nullptr) {
            continue;
        }

        if (node->GetTag().HasFlag(RDGNodeTag::Buffer)) {
            // Buffer 的 outEdges 是“资源被 pass 读取”，inEdges 是“pass 写回资源”。
            // 这里把同一个 pass 的多条边折叠成一个 usage，保留首尾状态和是否写入。
            auto* bufferNode = static_cast<RDGBufferNode*>(node);
            unordered_map<uint64_t, uint32_t> usageIndexByPass{};
            usageIndexByPass.reserve(bufferNode->_inEdges.size() + bufferNode->_outEdges.size());
            vector<BufferPassUsage> passUsages{};
            passUsages.reserve(bufferNode->_inEdges.size() + bufferNode->_outEdges.size());

            for (auto* edge : bufferNode->_inEdges) {
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

            for (auto* edge : bufferNode->_outEdges) {
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

            // 第二阶段的一部分：基于单资源 usage 推导 pass 依赖。
            // 规则是保守的：写后所有读/写都依赖这个写；写也依赖之前未截断的所有读。
            RDGPassHandle lastWriter{};
            bool hasLastWriter = false;
            vector<RDGPassHandle> activeReaders{};
            unordered_set<uint64_t> activeReaderIds{};
            activeReaderIds.reserve(passUsages.size());
            for (const auto& usage : passUsages) {
                if (hasLastWriter && lastWriter.Id != usage.Pass.Id) {
                    addDependency(lastWriter, usage.Pass, RDGResourceHandle{bufferNode->_id});
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
                    addDependency(reader, usage.Pass, RDGResourceHandle{bufferNode->_id});
                }

                activeReaders.clear();
                activeReaderIds.clear();
                lastWriter = usage.Pass;
                hasLastWriter = true;
            }

            compiledBuffers.emplace_back(CompiledBufferResource{
                .Node = bufferNode,
                .PassUsages = std::move(passUsages),
            });
            continue;
        }

        if (node->GetTag().HasFlag(RDGNodeTag::Texture)) {
            // Texture 和 Buffer 走同一套路，只是状态里多了 layout / subresource range。
            auto* textureNode = static_cast<RDGTextureNode*>(node);
            unordered_map<uint64_t, uint32_t> usageIndexByPass{};
            usageIndexByPass.reserve(textureNode->_inEdges.size() + textureNode->_outEdges.size());
            vector<TexturePassUsage> passUsages{};
            passUsages.reserve(textureNode->_inEdges.size() + textureNode->_outEdges.size());

            for (auto* edge : textureNode->_inEdges) {
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

            for (auto* edge : textureNode->_outEdges) {
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

            // 纹理依赖也按同样的保守 hazard 规则建立。
            RDGPassHandle lastWriter{};
            bool hasLastWriter = false;
            vector<RDGPassHandle> activeReaders{};
            unordered_set<uint64_t> activeReaderIds{};
            activeReaderIds.reserve(passUsages.size());
            for (const auto& usage : passUsages) {
                if (hasLastWriter && lastWriter.Id != usage.Pass.Id) {
                    addDependency(lastWriter, usage.Pass, RDGResourceHandle{textureNode->_id});
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
                    addDependency(reader, usage.Pass, RDGResourceHandle{textureNode->_id});
                }

                activeReaders.clear();
                activeReaderIds.clear();
                lastWriter = usage.Pass;
                hasLastWriter = true;
            }

            compiledTextures.emplace_back(CompiledTextureResource{
                .Node = textureNode,
                .PassUsages = std::move(passUsages),
            });
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
