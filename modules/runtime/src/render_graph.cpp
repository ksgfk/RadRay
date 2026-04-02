#include <radray/runtime/render_graph.h>

#include <iterator>

#include <radray/utility.h>

namespace radray {

namespace {

void AppendGraphvizEscapedText(fmt::memory_buffer& buffer, std::string_view text) {
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

void AppendImportExportNode(
    fmt::memory_buffer& buffer,
    uint64_t id,
    std::string_view suffix,
    std::string_view kind,
    RDGResourceOwnership ownership) {
    fmt::format_to(
        std::back_inserter(buffer),
        "    n{}_{} [shape=oval, style=dashed, label=\"kind: {}\\nownership: {}\"];\n",
        id,
        suffix,
        kind,
        ownership);
}

void AppendImportExportEdge(
    fmt::memory_buffer& buffer,
    uint64_t id,
    std::string_view suffix,
    bool fromResource,
    RDGExecutionStages stage,
    RDGMemoryAccesses access) {
    if (fromResource) {
        fmt::format_to(
            std::back_inserter(buffer),
            "    n{} -> n{}_{} [label=\"stage: {}\\naccess: {}\"];\n",
            id,
            id,
            suffix,
            stage,
            access);
    } else {
        fmt::format_to(
            std::back_inserter(buffer),
            "    n{}_{} -> n{} [label=\"stage: {}\\naccess: {}\"];\n",
            id,
            suffix,
            id,
            stage,
            access);
    }
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
    builder.Build(this, node.get());
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGPassHandle{raw->_id};
}

RDGPassHandle RenderGraph::AddComputePass(std::string_view name, unique_ptr<IRDGComputePass> pass) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGComputePassNode>(id, name);
    node->_impl = std::move(pass);
    IRDGComputePass::Builder builder{};
    node->_impl->Setup(builder);
    builder.Build(this, node.get());
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGPassHandle{raw->_id};
}

RDGPassHandle RenderGraph::AddCopyPass(std::string_view name) {
    const uint64_t id = _nodes.size();
    auto node = make_unique<RDGCopyPassNode>(id, name);
    auto raw = node.get();
    _nodes.emplace_back(std::move(node));
    return RDGPassHandle{raw->_id};
}

RDGEdge* RenderGraph::Link(RDGNodeHandle from_, RDGNodeHandle to_, RDGExecutionStages stage, RDGMemoryAccesses access, render::BufferRange bufferRange) {
    auto* from = _nodes[from_.Id].get();
    auto* to = _nodes[to_.Id].get();
    auto* edge = this->CreateEdge(from, to, stage, access);
    edge->_bufferRange = bufferRange;
    return edge;
}

RDGEdge* RenderGraph::Link(RDGNodeHandle from_, RDGNodeHandle to_, RDGExecutionStages stage, RDGMemoryAccesses access, RDGTextureLayout layout, render::SubresourceRange textureRange) {
    auto* from = _nodes[from_.Id].get();
    auto* to = _nodes[to_.Id].get();
    auto* edge = this->CreateEdge(from, to, stage, access);
    edge->_textureLayout = layout;
    edge->_textureRange = textureRange;
    return edge;
}

RDGEdge* RenderGraph::CreateEdge(RDGNode* from, RDGNode* to, RDGExecutionStages stage, RDGMemoryAccesses access) {
    auto edge = make_unique<RDGEdge>(from, to, stage, access);
    auto* raw = edge.get();
    _edges.emplace_back(std::move(edge));
    from->_outEdges.emplace_back(raw);
    to->_inEdges.emplace_back(raw);
    return raw;
}

RenderGraph::ValidateResult RenderGraph::Validate() const {
    // TODO:
    return {false, ""};
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
        fmt::format_to(out, "    n{} [label=\"id: {}\\nname: ", node->_id, node->_id);
        AppendGraphvizEscapedText(buffer, node->_name);
        node->AppendGraphvizNodeLabel(buffer);
        fmt::format_to(out, "\\ntag: {}\"];\n", node->GetTag());
    }
    for (const auto& edge : _edges) {
        fmt::format_to(
            out,
            "    n{} -> n{} [label=\"stage: {}\\naccess: {}\"];\n",
            edge->_from->_id,
            edge->_to->_id,
            edge->_stage,
            edge->_access);
    }
    for (const auto& node : _nodes) {
        node->AppendGraphvizExtraStatements(buffer);
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

void RDGResourceNode::AppendGraphvizNodeLabel(fmt::memory_buffer& buffer) const {
    fmt::format_to(
        std::back_inserter(buffer),
        "\\nkind: Resource\\nownership: {}",
        _ownership);
}

void RDGResourceNode::AppendGraphvizExtraStatements(fmt::memory_buffer& buffer) const {
    (void)buffer;
}

void RDGBufferNode::AppendGraphvizExtraStatements(fmt::memory_buffer& buffer) const {
    if (_importState.has_value()) {
        AppendImportExportNode(buffer, _id, "import", "Import", _ownership);
        AppendImportExportEdge(buffer, _id, "import", false, _importState->Stage, _importState->Access);
    }
    if (_exportState.has_value()) {
        AppendImportExportNode(buffer, _id, "export", "Export", _ownership);
        AppendImportExportEdge(buffer, _id, "export", true, _exportState->Stage, _exportState->Access);
    }
}

void RDGTextureNode::AppendGraphvizExtraStatements(fmt::memory_buffer& buffer) const {
    if (_importState.has_value()) {
        AppendImportExportNode(buffer, _id, "import", "Import", _ownership);
        AppendImportExportEdge(buffer, _id, "import", false, _importState->Stage, _importState->Access);
    }
    if (_exportState.has_value()) {
        AppendImportExportNode(buffer, _id, "export", "Export", _ownership);
        AppendImportExportEdge(buffer, _id, "export", true, _exportState->Stage, _exportState->Access);
    }
}

void RDGPassNode::AppendGraphvizNodeLabel(fmt::memory_buffer& buffer) const {
    fmt::format_to(std::back_inserter(buffer), "\\nkind: Pass");
}

void RDGPassNode::AppendGraphvizExtraStatements(fmt::memory_buffer& buffer) const {
    (void)buffer;
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
