#include <radray/runtime/render_graph.h>

#include <fmt/format.h>

namespace radray {

RDGPassHandle RDGRasterPassBuilder::_EnsurePass() {
    RADRAY_ASSERT(_graph != nullptr);
    if (_pass.IsValid()) {
        RADRAY_ASSERT(_pass.Id < _graph->_nodes.size());
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
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _pendingBufferUses.emplace_back(PendingBufferUse{
            .Buffer = buffer,
            .Stage = RDGExecutionStage::VertexShader,
            .Access = access,
            .Range = range,
            .Write = false,
        });
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _pendingBufferUses.emplace_back(PendingBufferUse{
            .Buffer = buffer,
            .Stage = RDGExecutionStage::PixelShader,
            .Access = access,
            .Range = range,
            .Write = false,
        });
    }
}

void RDGRasterPassBuilder::_LinkTextureStages(
    RDGTextureHandle texture,
    render::ShaderStages stages,
    RDGMemoryAccess access,
    RDGTextureLayout layout,
    render::SubresourceRange range) {
    _ValidateShaderStages(stages);
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _pendingTextureUses.emplace_back(PendingTextureUse{
            .Texture = texture,
            .Stage = RDGExecutionStage::VertexShader,
            .Access = access,
            .Layout = layout,
            .Range = range,
            .Write = false,
        });
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _pendingTextureUses.emplace_back(PendingTextureUse{
            .Texture = texture,
            .Stage = RDGExecutionStage::PixelShader,
            .Access = access,
            .Layout = layout,
            .Range = range,
            .Write = false,
        });
    }
}

RDGPassHandle RDGRasterPassBuilder::Build() {
    const auto pass = _EnsurePass();
    RADRAY_ASSERT(pass.IsValid() && pass.Id < _graph->_nodes.size());
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::GraphicsPass));
    auto* passNode = static_cast<RDGGraphicsPassNode*>(node);

    for (auto& attachment : _pendingColorAttachments) {
        passNode->_colorAttachments.emplace_back(std::move(attachment));
    }
    _pendingColorAttachments.clear();
    if (_pendingDepthStencilAttachment.has_value()) {
        RADRAY_ASSERT(!passNode->_depthStencilAttachment.has_value());
        passNode->_depthStencilAttachment = std::move(_pendingDepthStencilAttachment);
        _pendingDepthStencilAttachment.reset();
    }

    for (const auto& usage : _pendingBufferUses) {
        if (usage.Write) {
            _graph->Link(pass, usage.Buffer, usage.Stage, usage.Access, usage.Range);
        } else {
            _graph->Link(usage.Buffer, pass, usage.Stage, usage.Access, usage.Range);
        }
    }
    _pendingBufferUses.clear();

    for (const auto& usage : _pendingTextureUses) {
        if (usage.Write) {
            _graph->Link(pass, usage.Texture, usage.Stage, usage.Access, usage.Layout, usage.Range);
        } else {
            _graph->Link(usage.Texture, pass, usage.Stage, usage.Access, usage.Layout, usage.Range);
        }
    }
    _pendingTextureUses.clear();

    return pass;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseColorAttachment(
    uint32_t slot,
    RDGTextureHandle texture,
    render::SubresourceRange range,
    render::LoadAction load,
    render::StoreAction store,
    std::optional<render::ColorClearValue> clearValue) {
    _pendingColorAttachments.emplace_back(RDGColorAttachmentInfo{
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
    _pendingTextureUses.emplace_back(PendingTextureUse{
        .Texture = texture,
        .Stage = RDGExecutionStage::ColorOutput,
        .Access = access,
        .Layout = RDGTextureLayout::ColorAttachment,
        .Range = range,
        .Write = true,
    });
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
    RADRAY_ASSERT(!_pendingDepthStencilAttachment.has_value());
    _pendingDepthStencilAttachment = RDGDepthStencilAttachmentInfo{
        .Texture = texture,
        .Range = range,
        .DepthLoad = depthLoad,
        .DepthStore = depthStore,
        .StencilLoad = stencilLoad,
        .StencilStore = stencilStore,
        .ClearValue = std::move(clearValue),
    };

    const bool isWrite = _pendingDepthStencilAttachment->HasWriteAccess();
    if (isWrite) {
        _pendingTextureUses.emplace_back(PendingTextureUse{
            .Texture = texture,
            .Stage = RDGExecutionStage::DepthStencil,
            .Access = RDGMemoryAccess::DepthStencilWrite,
            .Layout = RDGTextureLayout::DepthStencilAttachment,
            .Range = range,
            .Write = true,
        });
    } else {
        _pendingTextureUses.emplace_back(PendingTextureUse{
            .Texture = texture,
            .Stage = RDGExecutionStage::DepthStencil,
            .Access = RDGMemoryAccess::DepthStencilRead,
            .Layout = RDGTextureLayout::DepthStencilReadOnly,
            .Range = range,
            .Write = false,
        });
    }
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseVertexBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = buffer,
        .Stage = RDGExecutionStage::VertexInput,
        .Access = RDGMemoryAccess::VertexRead,
        .Range = range,
        .Write = false,
    });
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseIndexBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = buffer,
        .Stage = RDGExecutionStage::VertexInput,
        .Access = RDGMemoryAccess::IndexRead,
        .Range = range,
        .Write = false,
    });
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseIndirectBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = buffer,
        .Stage = RDGExecutionStage::Indirect,
        .Access = RDGMemoryAccess::IndirectRead,
        .Range = range,
        .Write = false,
    });
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
    _ValidateShaderStages(stages);
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _pendingBufferUses.emplace_back(PendingBufferUse{
            .Buffer = buffer,
            .Stage = RDGExecutionStage::VertexShader,
            .Access = RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            .Range = range,
            .Write = true,
        });
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _pendingBufferUses.emplace_back(PendingBufferUse{
            .Buffer = buffer,
            .Stage = RDGExecutionStage::PixelShader,
            .Access = RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            .Range = range,
            .Write = true,
        });
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
    _ValidateShaderStages(stages);
    if (stages.HasFlag(render::ShaderStage::Vertex)) {
        _pendingTextureUses.emplace_back(PendingTextureUse{
            .Texture = texture,
            .Stage = RDGExecutionStage::VertexShader,
            .Access = RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            .Layout = RDGTextureLayout::General,
            .Range = range,
            .Write = true,
        });
    }
    if (stages.HasFlag(render::ShaderStage::Pixel)) {
        _pendingTextureUses.emplace_back(PendingTextureUse{
            .Texture = texture,
            .Stage = RDGExecutionStage::PixelShader,
            .Access = RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
            .Layout = RDGTextureLayout::General,
            .Range = range,
            .Write = true,
        });
    }
    return *this;
}

RDGPassHandle RDGComputePassBuilder::_EnsurePass() {
    RADRAY_ASSERT(_graph != nullptr);
    if (_pass.IsValid()) {
        RADRAY_ASSERT(_pass.Id < _graph->_nodes.size());
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
    const auto pass = _EnsurePass();
    for (const auto& usage : _pendingBufferUses) {
        if (usage.Write) {
            _graph->Link(pass, usage.Buffer, usage.Stage, usage.Access, usage.Range);
        } else {
            _graph->Link(usage.Buffer, pass, usage.Stage, usage.Access, usage.Range);
        }
    }
    _pendingBufferUses.clear();

    for (const auto& usage : _pendingTextureUses) {
        if (usage.Write) {
            _graph->Link(pass, usage.Texture, usage.Stage, usage.Access, usage.Layout, usage.Range);
        } else {
            _graph->Link(usage.Texture, pass, usage.Stage, usage.Access, usage.Layout, usage.Range);
        }
    }
    _pendingTextureUses.clear();
    return pass;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseCBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = buffer,
        .Stage = RDGExecutionStage::ComputeShader,
        .Access = RDGMemoryAccess::ConstantRead,
        .Range = range,
        .Write = false,
    });
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = buffer,
        .Stage = RDGExecutionStage::ComputeShader,
        .Access = RDGMemoryAccess::ShaderRead,
        .Range = range,
        .Write = false,
    });
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseRWBuffer(RDGBufferHandle buffer, render::BufferRange range) {
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = buffer,
        .Stage = RDGExecutionStage::ComputeShader,
        .Access = RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
        .Range = range,
        .Write = true,
    });
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseTexture(RDGTextureHandle texture, render::SubresourceRange range) {
    _pendingTextureUses.emplace_back(PendingTextureUse{
        .Texture = texture,
        .Stage = RDGExecutionStage::ComputeShader,
        .Access = RDGMemoryAccess::ShaderRead,
        .Layout = RDGTextureLayout::ShaderReadOnly,
        .Range = range,
        .Write = false,
    });
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::UseRWTexture(RDGTextureHandle texture, render::SubresourceRange range) {
    _pendingTextureUses.emplace_back(PendingTextureUse{
        .Texture = texture,
        .Stage = RDGExecutionStage::ComputeShader,
        .Access = RDGMemoryAccess::ShaderRead | RDGMemoryAccess::ShaderWrite,
        .Layout = RDGTextureLayout::General,
        .Range = range,
        .Write = true,
    });
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
    const auto pass = _EnsurePass();
    RADRAY_ASSERT(pass.IsValid() && pass.Id < _graph->_nodes.size());
    auto* node = _graph->_nodes[pass.Id].get();
    RADRAY_ASSERT(node != nullptr && node->GetTag().HasFlag(RDGNodeTag::CopyPass));
    auto* passNode = static_cast<RDGCopyPassNode*>(node);

    for (auto& op : _pendingOps) {
        passNode->_ops.emplace_back(std::move(op));
    }
    _pendingOps.clear();

    for (const auto& usage : _pendingBufferUses) {
        if (usage.Write) {
            _graph->Link(pass, usage.Buffer, usage.Stage, usage.Access, usage.Range);
        } else {
            _graph->Link(usage.Buffer, pass, usage.Stage, usage.Access, usage.Range);
        }
    }
    _pendingBufferUses.clear();

    for (const auto& usage : _pendingTextureUses) {
        if (usage.Write) {
            _graph->Link(pass, usage.Texture, usage.Stage, usage.Access, usage.Layout, usage.Range);
        } else {
            _graph->Link(usage.Texture, pass, usage.Stage, usage.Access, usage.Layout, usage.Range);
        }
    }
    _pendingTextureUses.clear();

    return pass;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyBufferToBuffer(
    RDGBufferHandle dst,
    uint64_t dstOffset,
    RDGBufferHandle src,
    uint64_t srcOffset,
    uint64_t size) {
    _pendingOps.emplace_back(RDGCopyBufferToBufferInfo{
        .Dst = dst,
        .DstOffset = dstOffset,
        .Src = src,
        .SrcOffset = srcOffset,
        .Size = size,
    });
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = src,
        .Stage = RDGExecutionStage::Copy,
        .Access = RDGMemoryAccess::TransferRead,
        .Range = render::BufferRange{srcOffset, size},
        .Write = false,
    });
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = dst,
        .Stage = RDGExecutionStage::Copy,
        .Access = RDGMemoryAccess::TransferWrite,
        .Range = render::BufferRange{dstOffset, size},
        .Write = true,
    });
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyBufferToTexture(
    RDGTextureHandle dst,
    render::SubresourceRange dstRange,
    RDGBufferHandle src,
    uint64_t srcOffset) {
    _pendingOps.emplace_back(RDGCopyBufferToTextureInfo{
        .Dst = dst,
        .DstRange = dstRange,
        .Src = src,
        .SrcOffset = srcOffset,
    });
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = src,
        .Stage = RDGExecutionStage::Copy,
        .Access = RDGMemoryAccess::TransferRead,
        .Range = render::BufferRange{srcOffset, render::BufferRange::All()},
        .Write = false,
    });
    _pendingTextureUses.emplace_back(PendingTextureUse{
        .Texture = dst,
        .Stage = RDGExecutionStage::Copy,
        .Access = RDGMemoryAccess::TransferWrite,
        .Layout = RDGTextureLayout::TransferDestination,
        .Range = dstRange,
        .Write = true,
    });
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::CopyTextureToBuffer(
    RDGBufferHandle dst,
    uint64_t dstOffset,
    RDGTextureHandle src,
    render::SubresourceRange srcRange) {
    _pendingOps.emplace_back(RDGCopyTextureToBufferInfo{
        .Dst = dst,
        .DstOffset = dstOffset,
        .Src = src,
        .SrcRange = srcRange,
    });
    _pendingTextureUses.emplace_back(PendingTextureUse{
        .Texture = src,
        .Stage = RDGExecutionStage::Copy,
        .Access = RDGMemoryAccess::TransferRead,
        .Layout = RDGTextureLayout::TransferSource,
        .Range = srcRange,
        .Write = false,
    });
    _pendingBufferUses.emplace_back(PendingBufferUse{
        .Buffer = dst,
        .Stage = RDGExecutionStage::Copy,
        .Access = RDGMemoryAccess::TransferWrite,
        .Range = render::BufferRange{dstOffset, render::BufferRange::All()},
        .Write = true,
    });
    return *this;
}

}  // namespace radray
