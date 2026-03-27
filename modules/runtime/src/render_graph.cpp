#include <radray/runtime/render_graph.h>

#include <fmt/format.h>

namespace radray {

RDGNode* RenderGraph::_ResolveNode(RDGNodeHandle handle) {
    if (handle.Id >= _nodes.size()) {
        throw GpuSystemException(fmt::format("{} requires a valid node handle", "RenderGraph"));
    }
    auto* node = _nodes[handle.Id].get();
    if (node == nullptr) {
        throw GpuSystemException(fmt::format("{} requires a live node handle", "RenderGraph"));
    }
    return node;
}

RDGBufferNode* RenderGraph::_ResolveBufferNode(RDGBufferHandle handle) {
    auto* node = this->_ResolveNode(handle);
    if (!node->IsBufferNode()) {
        throw GpuSystemException(fmt::format("{} requires a buffer node handle", "RenderGraph"));
    }
    return static_cast<RDGBufferNode*>(node);
}

RDGTextureNode* RenderGraph::_ResolveTextureNode(RDGTextureHandle handle) {
    auto* node = this->_ResolveNode(handle);
    if (!node->IsTextureNode()) {
        throw GpuSystemException(fmt::format("{} requires a texture node handle", "RenderGraph"));
    }
    return static_cast<RDGTextureNode*>(node);
}

void RenderGraph::_ValidatePassResourceLink(RDGNode* from, RDGNode* to) {
    const bool fromPass = from->IsPassNode();
    const bool toPass = to->IsPassNode();
    if (fromPass == toPass) {
        throw GpuSystemException(fmt::format("{} only accepts pass-resource links", "RenderGraph"));
    }
}

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
    if (!buffer.IsValid()) {
        throw GpuSystemException(fmt::format("{} requires a valid external buffer handle", "RenderGraph::ImportBuffer"));
    }
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
    if (!texture.IsValid()) {
        throw GpuSystemException(fmt::format("{} requires a valid external texture handle", "RenderGraph::ImportTexture"));
    }
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
    auto* node = this->_ResolveBufferNode(nodeHandle);
    if (node->_ownership == RDGResourceOwnership::Transient) {
        throw GpuSystemException(fmt::format("{} cannot export a transient resource", "RenderGraph::ExportBuffer"));
    }
    if (node->_exportedState.has_value()) {
        throw GpuSystemException(fmt::format("{} does not allow exporting the same resource twice", "RenderGraph::ExportBuffer"));
    }
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
    auto* node = this->_ResolveTextureNode(nodeHandle);
    if (node->_ownership == RDGResourceOwnership::Transient) {
        throw GpuSystemException(fmt::format("{} cannot export a transient resource", "RenderGraph::ExportTexture"));
    }
    if (node->_exportedState.has_value()) {
        throw GpuSystemException(fmt::format("{} does not allow exporting the same resource twice", "RenderGraph::ExportTexture"));
    }
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

void RenderGraph::Link(
    RDGNodeHandle fromHandle,
    RDGNodeHandle toHandle,
    RDGExecutionStage stage,
    RDGMemoryAccess access,
    render::BufferRange bufferRange) {
    auto* from = this->_ResolveNode(fromHandle);
    auto* to = this->_ResolveNode(toHandle);
    this->_ValidatePassResourceLink(from, to);

    auto* resource = from->IsPassNode() ? to : from;
    if (!resource->IsBufferNode()) {
        throw GpuSystemException(fmt::format("{} buffer overload requires a buffer resource node", "RenderGraph::Link"));
    }

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
    auto* from = this->_ResolveNode(fromHandle);
    auto* to = this->_ResolveNode(toHandle);
    this->_ValidatePassResourceLink(from, to);

    auto* resource = from->IsPassNode() ? to : from;
    if (!resource->IsTextureNode()) {
        throw GpuSystemException(fmt::format("{} texture overload requires a texture resource node", "RenderGraph::Link"));
    }

    auto* edge = this->_CreateEdge(from, to, stage, access);
    edge->_textureLayout = layout;
    edge->_textureRange = textureRange;
}

}  // namespace radray
