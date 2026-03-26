#include <radray/runtime/render_graph.h>

#include <array>
#include <iterator>
#include <utility>
#include <variant>

#include <fmt/format.h>

namespace radray {

namespace {

string CopyName(std::string_view name) {
    return string{name};
}

string HandleToString(RGResourceHandle handle) {
    if (!handle.IsValid()) {
        return "invalid";
    }
    return fmt::format("{}", handle.Node.Value);
}

string HandleToString(RGPassHandle handle) {
    if (!handle.IsValid()) {
        return "invalid";
    }
    return fmt::format("{}", handle.Node.Value);
}

const char* ResourceKindName(RGResourceKind kind) noexcept {
    switch (kind) {
        case RGResourceKind::Buffer: return "Buffer";
        case RGResourceKind::Texture: return "Texture";
        case RGResourceKind::AccelerationStructure: return "AccelerationStructure";
    }
    return "UnknownResource";
}

const char* PassKindName(RGPassKind kind) noexcept {
    switch (kind) {
        case RGPassKind::Graphics: return "Graphics";
        case RGPassKind::Compute: return "Compute";
        case RGPassKind::Copy: return "Copy";
        case RGPassKind::RayTracing: return "RayTracing";
    }
    return "UnknownPass";
}

const char* UseKindName(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::VertexBufferRead: return "VertexBufferRead";
        case RGUseKind::IndexBufferRead: return "IndexBufferRead";
        case RGUseKind::IndirectBufferRead: return "IndirectBufferRead";
        case RGUseKind::ConstantBufferRead: return "ConstantBufferRead";
        case RGUseKind::StorageBufferRead: return "StorageBufferRead";
        case RGUseKind::StorageBufferReadWrite: return "StorageBufferReadWrite";
        case RGUseKind::SampledTextureRead: return "SampledTextureRead";
        case RGUseKind::StorageTextureRead: return "StorageTextureRead";
        case RGUseKind::StorageTextureReadWrite: return "StorageTextureReadWrite";
        case RGUseKind::ColorAttachmentWrite: return "ColorAttachmentWrite";
        case RGUseKind::DepthStencilAttachmentWrite: return "DepthStencilAttachmentWrite";
        case RGUseKind::AccelerationStructureRead: return "AccelerationStructureRead";
        case RGUseKind::AccelerationStructureBuild: return "AccelerationStructureBuild";
        case RGUseKind::BuildScratchBuffer: return "BuildScratchBuffer";
        case RGUseKind::ShaderTableRead: return "ShaderTableRead";
        case RGUseKind::CopyBufferRead: return "CopyBufferRead";
        case RGUseKind::CopyBufferWrite: return "CopyBufferWrite";
        case RGUseKind::CopyTextureRead: return "CopyTextureRead";
        case RGUseKind::CopyTextureWrite: return "CopyTextureWrite";
    }
    return "UnknownUse";
}

bool IsBufferUse(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::VertexBufferRead:
        case RGUseKind::IndexBufferRead:
        case RGUseKind::IndirectBufferRead:
        case RGUseKind::ConstantBufferRead:
        case RGUseKind::StorageBufferRead:
        case RGUseKind::StorageBufferReadWrite:
        case RGUseKind::BuildScratchBuffer:
        case RGUseKind::ShaderTableRead:
        case RGUseKind::CopyBufferRead:
        case RGUseKind::CopyBufferWrite: return true;
        default: return false;
    }
}

bool IsTextureUse(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::SampledTextureRead:
        case RGUseKind::StorageTextureRead:
        case RGUseKind::StorageTextureReadWrite:
        case RGUseKind::ColorAttachmentWrite:
        case RGUseKind::DepthStencilAttachmentWrite:
        case RGUseKind::CopyTextureRead:
        case RGUseKind::CopyTextureWrite: return true;
        default: return false;
    }
}

bool IsAccelerationStructureUse(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::AccelerationStructureRead:
        case RGUseKind::AccelerationStructureBuild: return true;
        default: return false;
    }
}

bool IsReadUse(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::VertexBufferRead:
        case RGUseKind::IndexBufferRead:
        case RGUseKind::IndirectBufferRead:
        case RGUseKind::ConstantBufferRead:
        case RGUseKind::StorageBufferRead:
        case RGUseKind::SampledTextureRead:
        case RGUseKind::StorageTextureRead:
        case RGUseKind::AccelerationStructureRead:
        case RGUseKind::ShaderTableRead:
        case RGUseKind::CopyBufferRead:
        case RGUseKind::CopyTextureRead: return true;
        default: return false;
    }
}

bool IsWriteUse(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::ColorAttachmentWrite:
        case RGUseKind::DepthStencilAttachmentWrite:
        case RGUseKind::AccelerationStructureBuild:
        case RGUseKind::CopyBufferWrite:
        case RGUseKind::CopyTextureWrite: return true;
        default: return false;
    }
}

bool IsReadWriteUse(RGUseKind kind) noexcept {
    switch (kind) {
        case RGUseKind::StorageBufferReadWrite:
        case RGUseKind::StorageTextureReadWrite:
        case RGUseKind::BuildScratchBuffer: return true;
        default: return false;
    }
}

bool IsUseAllowedForPass(RGPassKind passKind, RGUseKind useKind) noexcept {
    switch (passKind) {
        case RGPassKind::Graphics:
            switch (useKind) {
                case RGUseKind::VertexBufferRead:
                case RGUseKind::IndexBufferRead:
                case RGUseKind::IndirectBufferRead:
                case RGUseKind::ConstantBufferRead:
                case RGUseKind::StorageBufferRead:
                case RGUseKind::StorageBufferReadWrite:
                case RGUseKind::SampledTextureRead:
                case RGUseKind::StorageTextureRead:
                case RGUseKind::StorageTextureReadWrite:
                case RGUseKind::ColorAttachmentWrite:
                case RGUseKind::DepthStencilAttachmentWrite: return true;
                default: return false;
            }
        case RGPassKind::Compute:
            switch (useKind) {
                case RGUseKind::ConstantBufferRead:
                case RGUseKind::StorageBufferRead:
                case RGUseKind::StorageBufferReadWrite:
                case RGUseKind::SampledTextureRead:
                case RGUseKind::StorageTextureRead:
                case RGUseKind::StorageTextureReadWrite:
                case RGUseKind::AccelerationStructureRead: return true;
                default: return false;
            }
        case RGPassKind::Copy:
            switch (useKind) {
                case RGUseKind::CopyBufferRead:
                case RGUseKind::CopyBufferWrite:
                case RGUseKind::CopyTextureRead:
                case RGUseKind::CopyTextureWrite: return true;
                default: return false;
            }
        case RGPassKind::RayTracing:
            switch (useKind) {
                case RGUseKind::ConstantBufferRead:
                case RGUseKind::StorageBufferRead:
                case RGUseKind::StorageBufferReadWrite:
                case RGUseKind::SampledTextureRead:
                case RGUseKind::StorageTextureRead:
                case RGUseKind::StorageTextureReadWrite:
                case RGUseKind::AccelerationStructureRead:
                case RGUseKind::AccelerationStructureBuild:
                case RGUseKind::BuildScratchBuffer:
                case RGUseKind::ShaderTableRead: return true;
                default: return false;
            }
    }
    return false;
}

string EscapeGraphviz(std::string_view text) {
    string escaped{};
    escaped.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

void AppendLabelLine(string& label, std::string_view line) {
    if (line.empty()) {
        return;
    }
    if (!label.empty()) {
        label += "\\n";
    }
    label += EscapeGraphviz(line);
}

string ResourceDisplayName(const RGResourceRecord& record) {
    if (!record.Name.empty()) {
        return record.Name;
    }
    return fmt::format("{}#{}", ResourceKindName(record.Kind), record.Handle.Node.Value);
}

string PassDisplayName(const RGPassRecord& record) {
    if (!record.Name.empty()) {
        return record.Name;
    }
    return fmt::format("{}Pass#{}", PassKindName(record.Kind), record.Handle.Node.Value);
}

string FormatBufferRange(render::BufferRange range) {
    if (range.Offset == 0 && range.Size == std::numeric_limits<uint64_t>::max()) {
        return "whole-buffer";
    }
    return fmt::format("range={}+{}", range.Offset, range.Size);
}

string FormatSubresourceRange(render::SubresourceRange range) {
    if (range.BaseArrayLayer == 0 &&
        range.ArrayLayerCount == render::SubresourceRange::All &&
        range.BaseMipLevel == 0 &&
        range.MipLevelCount == render::SubresourceRange::All) {
        return "whole-subresource";
    }
    return fmt::format(
        "layers={}+{}, mips={}+{}",
        range.BaseArrayLayer,
        range.ArrayLayerCount,
        range.BaseMipLevel,
        range.MipLevelCount);
}

string FormatExportState(const RGExternalBufferState& state) {
    return fmt::format(
        "queue={}\nstate={}\n{}",
        state.Queue,
        state.State,
        FormatBufferRange(state.Range));
}

string FormatExportState(const RGExternalTextureState& state) {
    return fmt::format(
        "queue={}\nstate={}\n{}",
        state.Queue,
        state.State,
        FormatSubresourceRange(state.Range));
}

string FormatExportState(const RGExternalAccelerationStructureState& state) {
    return fmt::format("queue={}\nstate={}", state.Queue, state.State);
}

string FormatUseLabel(const RGUseRecord& record) {
    string label{};
    AppendLabelLine(label, UseKindName(record.Kind));
    if (!record.Name.empty()) {
        AppendLabelLine(label, record.Name);
    }
    return label;
}

string PassNodeId(RGPassHandle handle) {
    return fmt::format("pass_{}", handle.Node.Value);
}

string ResourceVersionNodeId(RGResourceHandle handle, uint32_t version) {
    return fmt::format("res_{}_v{}", handle.Node.Value, version);
}

string ResourceVersionDisplayName(const RGResourceRecord& record, uint32_t version) {
    return fmt::format("{}@v{}", ResourceDisplayName(record), version);
}

string ResourceColor(RGResourceHandle handle) {
    constexpr std::array<std::string_view, 12> kPalette = {
        "#f4a261",
        "#2a9d8f",
        "#e76f51",
        "#457b9d",
        "#8ab17d",
        "#e9c46a",
        "#90be6d",
        "#b56576",
        "#6d597a",
        "#f28482",
        "#84a59d",
        "#a8dadc"};
    return string{kPalette[handle.Node.Value % kPalette.size()]};
}

void AddIssue(
    RGValidationResult& result,
    RGValidationIssueCode code,
    string message,
    std::optional<RGResourceHandle> resource,
    std::optional<RGPassHandle> pass,
    RGValidationSeverity severity = RGValidationSeverity::Error) {
    result.Issues.push_back(RGValidationIssue{
        .Severity = severity,
        .Code = code,
        .Message = std::move(message),
        .Resource = resource,
        .Pass = pass});
}

}  // namespace

bool RGValidationResult::IsValid() const noexcept {
    for (const auto& issue : Issues) {
        if (issue.Severity == RGValidationSeverity::Error) {
            return false;
        }
    }
    return true;
}

RGPassContext::RGPassContext(
    RenderGraph* graph,
    const RGCompiledGraph* compiledGraph,
    const RGExecutionEnvironment* environment,
    RGPassHandle pass) noexcept
    : _graph(graph),
      _compiledGraph(compiledGraph),
      _environment(environment),
      _pass(pass) {}

RenderGraph& RGPassContext::GetGraph() const noexcept { return *_graph; }
const RGCompiledGraph& RGPassContext::GetCompiledGraph() const noexcept { return *_compiledGraph; }
RGPassHandle RGPassContext::GetPass() const noexcept { return _pass; }
const RGExecutionEnvironment& RGPassContext::GetEnvironment() const noexcept { return *_environment; }
render::Device& RGPassContext::GetDevice() const noexcept { return *GetEnvironment().Device; }
render::CommandBuffer& RGPassContext::GetCommandBuffer() const noexcept { return *GetEnvironment().CommandBuffer; }
Nullable<GpuFrameContext*> RGPassContext::GetFrameContext() const noexcept { return GetEnvironment().FrameContext; }
render::Buffer* RGPassContext::ResolveBuffer(RGResourceHandle) const noexcept { return nullptr; }
render::Texture* RGPassContext::ResolveTexture(RGResourceHandle) const noexcept { return nullptr; }
render::TextureView* RGPassContext::ResolveTextureView(RGResourceHandle, const RGTextureViewDesc&) const noexcept { return nullptr; }
render::AccelerationStructure* RGPassContext::ResolveAccelerationStructure(RGResourceHandle) const noexcept { return nullptr; }
render::AccelerationStructureView* RGPassContext::ResolveAccelerationStructureView(RGResourceHandle) const noexcept { return nullptr; }

RGGraphicsPassContext::RGGraphicsPassContext(
    RenderGraph* graph,
    const RGCompiledGraph* compiledGraph,
    const RGExecutionEnvironment* environment,
    RGPassHandle pass,
    render::GraphicsCommandEncoder* encoder) noexcept
    : RGPassContext(graph, compiledGraph, environment, pass),
      _encoder(encoder) {}

render::GraphicsCommandEncoder& RGGraphicsPassContext::GetEncoder() const noexcept { return *_encoder; }

RGComputePassContext::RGComputePassContext(
    RenderGraph* graph,
    const RGCompiledGraph* compiledGraph,
    const RGExecutionEnvironment* environment,
    RGPassHandle pass,
    render::ComputeCommandEncoder* encoder) noexcept
    : RGPassContext(graph, compiledGraph, environment, pass),
      _encoder(encoder) {}

render::ComputeCommandEncoder& RGComputePassContext::GetEncoder() const noexcept { return *_encoder; }

RGRayTracingPassContext::RGRayTracingPassContext(
    RenderGraph* graph,
    const RGCompiledGraph* compiledGraph,
    const RGExecutionEnvironment* environment,
    RGPassHandle pass,
    render::RayTracingCommandEncoder* encoder) noexcept
    : RGPassContext(graph, compiledGraph, environment, pass),
      _encoder(encoder) {}

render::RayTracingCommandEncoder& RGRayTracingPassContext::GetEncoder() const noexcept { return *_encoder; }

RGGraphicsPassBuilder::RGGraphicsPassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept : _graph(graph), _pass(pass) {}
RGComputePassBuilder::RGComputePassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept : _graph(graph), _pass(pass) {}
RGRayTracingPassBuilder::RGRayTracingPassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept : _graph(graph), _pass(pass) {}
RGCopyPassBuilder::RGCopyPassBuilder(RenderGraph* graph, RGPassHandle pass) noexcept : _graph(graph), _pass(pass) {}

RenderGraph::RenderGraph() = default;
RenderGraph::~RenderGraph() = default;
RenderGraph::RenderGraph(RenderGraph&&) noexcept = default;
RenderGraph& RenderGraph::operator=(RenderGraph&&) noexcept = default;

const RGResourceRecord* RenderGraph::FindResource(RGResourceHandle handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const auto index = static_cast<size_t>(handle.Node.Value);
    if (index >= _resources.size()) {
        return nullptr;
    }
    return _resources[index].Handle == handle ? &_resources[index] : nullptr;
}

const RGPassRecord* RenderGraph::FindPass(RGPassHandle handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const auto index = static_cast<size_t>(handle.Node.Value);
    if (index >= _passes.size()) {
        return nullptr;
    }
    return _passes[index].Handle == handle ? &_passes[index] : nullptr;
}

void RenderGraph::AppendUseRecord(
    RGPassHandle pass,
    RGResourceHandle resource,
    RGUseKind kind,
    string name,
    RGUseData data) {
    _uses.push_back(RGUseRecord{
        .Pass = pass,
        .Resource = resource,
        .Kind = kind,
        .Name = std::move(name),
        .Data = std::move(data)});
}

RGResourceHandle RenderGraph::AddBuffer(
    std::string_view name,
    const RGBufferDesc& desc,
    RGResourceOptions options) {
    RGResourceHandle handle{RGNodeId{_nextResourceId++}};
    _resources.push_back(RGResourceRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGResourceKind::Buffer,
        .Imported = false,
        .External = GpuResourceHandle::Invalid(),
        .Options = options,
        .Desc = desc,
        .InitialState = std::monostate{}});
    return handle;
}

RGResourceHandle RenderGraph::AddTexture(
    std::string_view name,
    const RGTextureDesc& desc,
    RGResourceOptions options) {
    RGResourceHandle handle{RGNodeId{_nextResourceId++}};
    _resources.push_back(RGResourceRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGResourceKind::Texture,
        .Imported = false,
        .External = GpuResourceHandle::Invalid(),
        .Options = options,
        .Desc = desc,
        .InitialState = std::monostate{}});
    return handle;
}

RGResourceHandle RenderGraph::AddAccelerationStructure(
    std::string_view name,
    const RGAccelerationStructureDesc& desc,
    RGResourceOptions options) {
    RGResourceHandle handle{RGNodeId{_nextResourceId++}};
    _resources.push_back(RGResourceRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGResourceKind::AccelerationStructure,
        .Imported = false,
        .External = GpuResourceHandle::Invalid(),
        .Options = options,
        .Desc = desc,
        .InitialState = std::monostate{}});
    return handle;
}

RGResourceHandle RenderGraph::ImportBuffer(
    std::string_view name,
    const RGImportedBufferDesc& desc,
    RGResourceOptions options) {
    RGResourceHandle handle{RGNodeId{_nextResourceId++}};
    _resources.push_back(RGResourceRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGResourceKind::Buffer,
        .Imported = true,
        .External = desc.External,
        .Options = options,
        .Desc = desc.Desc,
        .InitialState = desc.InitialState});
    return handle;
}

RGResourceHandle RenderGraph::ImportTexture(
    std::string_view name,
    const RGImportedTextureDesc& desc,
    RGResourceOptions options) {
    RGResourceHandle handle{RGNodeId{_nextResourceId++}};
    _resources.push_back(RGResourceRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGResourceKind::Texture,
        .Imported = true,
        .External = desc.External,
        .Options = options,
        .Desc = desc.Desc,
        .InitialState = desc.InitialState});
    return handle;
}

RGResourceHandle RenderGraph::ImportAccelerationStructure(
    std::string_view name,
    const RGImportedAccelerationStructureDesc& desc,
    RGResourceOptions options) {
    RGResourceHandle handle{RGNodeId{_nextResourceId++}};
    _resources.push_back(RGResourceRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGResourceKind::AccelerationStructure,
        .Imported = true,
        .External = desc.External,
        .Options = options,
        .Desc = desc.Desc,
        .InitialState = desc.InitialState});
    return handle;
}

void RenderGraph::ExportBuffer(
    RGResourceHandle buffer,
    const RGExternalBufferState& finalState) {
    _exports.push_back(RGExportRecord{
        .Resource = buffer,
        .ExpectedKind = RGResourceKind::Buffer,
        .State = finalState});
}

void RenderGraph::ExportTexture(
    RGResourceHandle texture,
    const RGExternalTextureState& finalState) {
    _exports.push_back(RGExportRecord{
        .Resource = texture,
        .ExpectedKind = RGResourceKind::Texture,
        .State = finalState});
}

void RenderGraph::ExportAccelerationStructure(
    RGResourceHandle accelerationStructure,
    const RGExternalAccelerationStructureState& finalState) {
    _exports.push_back(RGExportRecord{
        .Resource = accelerationStructure,
        .ExpectedKind = RGResourceKind::AccelerationStructure,
        .State = finalState});
}

RGGraphicsPassBuilder RenderGraph::AddGraphicsPass(
    std::string_view name,
    RGGraphicsPassOptions options) {
    RGPassHandle handle{RGNodeId{_nextPassId++}};
    _passes.push_back(RGPassRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGPassKind::Graphics,
        .Queue = render::QueueType::Direct,
        .Flags = options.Flags});
    return RGGraphicsPassBuilder(this, handle);
}

RGComputePassBuilder RenderGraph::AddComputePass(
    std::string_view name,
    RGComputePassOptions options) {
    RGPassHandle handle{RGNodeId{_nextPassId++}};
    _passes.push_back(RGPassRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGPassKind::Compute,
        .Queue = options.Queue,
        .Flags = options.Flags});
    return RGComputePassBuilder(this, handle);
}

RGCopyPassBuilder RenderGraph::AddCopyPass(
    std::string_view name,
    RGCopyPassOptions options) {
    RGPassHandle handle{RGNodeId{_nextPassId++}};
    _passes.push_back(RGPassRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGPassKind::Copy,
        .Queue = options.Queue,
        .Flags = options.Flags});
    return RGCopyPassBuilder(this, handle);
}

RGRayTracingPassBuilder RenderGraph::AddRayTracingPass(
    std::string_view name,
    RGRayTracingPassOptions options) {
    RGPassHandle handle{RGNodeId{_nextPassId++}};
    _passes.push_back(RGPassRecord{
        .Handle = handle,
        .Name = CopyName(name),
        .Kind = RGPassKind::RayTracing,
        .Queue = render::QueueType::Direct,
        .Flags = options.Flags});
    return RGRayTracingPassBuilder(this, handle);
}

RGValidationResult RenderGraph::Validate() const {
    RGValidationResult result{};
    for (const auto& resource : _resources) {
        if (resource.Imported && !resource.External.IsValid()) {
            AddIssue(
                result,
                RGValidationIssueCode::MissingImportedExternalHandle,
                fmt::format(
                    "Imported {} resource '{}' is missing an external GpuResourceHandle.",
                    ResourceKindName(resource.Kind),
                    ResourceDisplayName(resource)),
                resource.Handle,
                std::nullopt);
        }
    }

    for (const auto& use : _uses) {
        const auto* pass = FindPass(use.Pass);
        if (pass == nullptr) {
            AddIssue(
                result,
                RGValidationIssueCode::MissingPass,
                fmt::format(
                    "Use '{}' references missing pass handle {}.",
                    UseKindName(use.Kind),
                    HandleToString(use.Pass)),
                use.Resource,
                use.Pass);
        }

        const auto* resource = FindResource(use.Resource);
        if (resource == nullptr) {
            AddIssue(
                result,
                RGValidationIssueCode::MissingResource,
                fmt::format(
                    "Use '{}' references missing resource handle {}.",
                    UseKindName(use.Kind),
                    HandleToString(use.Resource)),
                use.Resource,
                use.Pass);
            continue;
        }

        bool resourceKindValid = false;
        if (IsBufferUse(use.Kind)) {
            resourceKindValid = resource->Kind == RGResourceKind::Buffer;
        } else if (IsTextureUse(use.Kind)) {
            resourceKindValid = resource->Kind == RGResourceKind::Texture;
        } else if (IsAccelerationStructureUse(use.Kind)) {
            resourceKindValid = resource->Kind == RGResourceKind::AccelerationStructure;
        }
        if (!resourceKindValid) {
            AddIssue(
                result,
                RGValidationIssueCode::InvalidResourceUseForKind,
                fmt::format(
                    "Use '{}' is incompatible with {} resource '{}'.",
                    UseKindName(use.Kind),
                    ResourceKindName(resource->Kind),
                    ResourceDisplayName(*resource)),
                use.Resource,
                use.Pass);
        }

        if (pass != nullptr && !IsUseAllowedForPass(pass->Kind, use.Kind)) {
            AddIssue(
                result,
                RGValidationIssueCode::InvalidPassUseForKind,
                fmt::format(
                    "Use '{}' is incompatible with {} pass '{}'.",
                    UseKindName(use.Kind),
                    PassKindName(pass->Kind),
                    PassDisplayName(*pass)),
                use.Resource,
                use.Pass);
        }

        if (use.Kind == RGUseKind::AccelerationStructureBuild) {
            const auto* buildDesc = std::get_if<RGAccelerationStructureBuildDesc>(&use.Data);
            if (buildDesc == nullptr || !buildDesc->ScratchBuffer.IsValid()) {
                AddIssue(
                    result,
                    RGValidationIssueCode::InvalidBuildScratchBuffer,
                    fmt::format(
                        "AccelerationStructureBuild on resource '{}' is missing a valid scratch buffer handle.",
                        ResourceDisplayName(*resource)),
                    use.Resource,
                    use.Pass);
            }
        }
    }

    for (size_t exportIndex = 0; exportIndex < _exports.size(); ++exportIndex) {
        const auto& exportRecord = _exports[exportIndex];
        const auto* resource = FindResource(exportRecord.Resource);
        if (resource == nullptr) {
            AddIssue(
                result,
                RGValidationIssueCode::MissingResource,
                fmt::format(
                    "Export references missing resource handle {}.",
                    HandleToString(exportRecord.Resource)),
                exportRecord.Resource,
                std::nullopt);
            continue;
        }

        if (resource->Kind != exportRecord.ExpectedKind) {
            AddIssue(
                result,
                RGValidationIssueCode::InvalidExportForKind,
                fmt::format(
                    "Export kind does not match {} resource '{}'.",
                    ResourceKindName(resource->Kind),
                    ResourceDisplayName(*resource)),
                exportRecord.Resource,
                std::nullopt);
        }

        for (size_t previous = 0; previous < exportIndex; ++previous) {
            if (_exports[previous].Resource == exportRecord.Resource) {
                AddIssue(
                    result,
                    RGValidationIssueCode::DuplicateExport,
                    fmt::format(
                        "Resource '{}' is exported more than once.",
                        ResourceDisplayName(*resource)),
                    exportRecord.Resource,
                    std::nullopt);
                break;
            }
        }
    }

    return result;
}

string RenderGraph::ToGraphviz() const {
    fmt::memory_buffer buffer{};
    fmt::format_to(std::back_inserter(buffer), "digraph RenderGraph {{\n");
    fmt::format_to(std::back_inserter(buffer), "  rankdir=LR;\n");
    fmt::format_to(std::back_inserter(buffer), "  node [fontname=\"Segoe UI\"];\n");

    struct MissingResourceNode {
        RGResourceHandle Handle{};
        string Id{};
    };

    struct MissingPassNode {
        RGPassHandle Handle{};
        string Id{};
    };

    vector<MissingResourceNode> missingResources{};
    vector<MissingPassNode> missingPasses{};

    auto findOrAddMissingResource = [&](RGResourceHandle handle) -> string {
        for (const auto& entry : missingResources) {
            if (entry.Handle == handle) {
                return entry.Id;
            }
        }
        const auto id = fmt::format("missing_res_{}", missingResources.size());
        missingResources.push_back(MissingResourceNode{.Handle = handle, .Id = id});
        return id;
    };

    auto findOrAddMissingPass = [&](RGPassHandle handle) -> string {
        for (const auto& entry : missingPasses) {
            if (entry.Handle == handle) {
                return entry.Id;
            }
        }
        const auto id = fmt::format("missing_pass_{}", missingPasses.size());
        missingPasses.push_back(MissingPassNode{.Handle = handle, .Id = id});
        return id;
    };

    for (const auto& use : _uses) {
        if (FindResource(use.Resource) == nullptr) {
            findOrAddMissingResource(use.Resource);
        }
        if (FindPass(use.Pass) == nullptr) {
            findOrAddMissingPass(use.Pass);
        }
    }

    for (const auto& exportRecord : _exports) {
        if (FindResource(exportRecord.Resource) == nullptr) {
            findOrAddMissingResource(exportRecord.Resource);
        }
    }

    for (const auto& pass : _passes) {
        string label{};
        AppendLabelLine(label, fmt::format("{} Pass", PassKindName(pass.Kind)));
        AppendLabelLine(label, PassDisplayName(pass));
        AppendLabelLine(label, fmt::format("queue={}", pass.Queue));
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} [shape=box, style=\"rounded,filled\", fillcolor=\"#bde0fe\", color=\"#5b7c99\", label=\"{}\"];\n",
            PassNodeId(pass.Handle),
            label);
    }

    for (size_t exportIndex = 0; exportIndex < _exports.size(); ++exportIndex) {
        const auto& exportRecord = _exports[exportIndex];
        const auto exportNodeId = fmt::format("export_{}", exportIndex);
        string label{};
        AppendLabelLine(label, fmt::format("Export {}", ResourceKindName(exportRecord.ExpectedKind)));
        if (const auto* resource = FindResource(exportRecord.Resource)) {
            AppendLabelLine(label, ResourceDisplayName(*resource));
        }
        std::visit(
            [&](const auto& state) {
                AppendLabelLine(label, FormatExportState(state));
            },
            exportRecord.State);
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} [shape=diamond, style=\"filled\", fillcolor=\"#f1f3f5\", color=\"#6c757d\", label=\"{}\"];\n",
            exportNodeId,
            label);
    }

    for (const auto& missingResource : missingResources) {
        string label{};
        if (!missingResource.Handle.IsValid()) {
            AppendLabelLine(label, "Invalid Resource");
        } else {
            AppendLabelLine(label, "Missing Resource");
            AppendLabelLine(label, fmt::format("handle={}", missingResource.Handle.Node.Value));
        }
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} [shape=box, style=\"dashed,rounded\", color=\"#d62828\", label=\"{}\"];\n",
            missingResource.Id,
            label);
    }

    for (const auto& missingPass : missingPasses) {
        string label{};
        if (!missingPass.Handle.IsValid()) {
            AppendLabelLine(label, "Invalid Pass");
        } else {
            AppendLabelLine(label, "Missing Pass");
            AppendLabelLine(label, fmt::format("handle={}", missingPass.Handle.Node.Value));
        }
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} [shape=box, style=\"dashed,rounded\", color=\"#d62828\", label=\"{}\"];\n",
            missingPass.Id,
            label);
    }

    struct ResourceVersionState {
        uint32_t CurrentVersion{0};
    };

    unordered_map<uint32_t, ResourceVersionState> versionStates{};
    versionStates.reserve(_resources.size());

    auto ensureVersionNode = [&](const RGResourceRecord& resource, uint32_t version) {
        string label{};
        AppendLabelLine(label, fmt::format("{} Resource", ResourceKindName(resource.Kind)));
        AppendLabelLine(label, ResourceVersionDisplayName(resource, version));
        AppendLabelLine(label, resource.Imported ? "imported" : "local");
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} [shape=ellipse, style=\"filled\", fillcolor=\"{}\", color=\"#495057\", label=\"{}\"];\n",
            ResourceVersionNodeId(resource.Handle, version),
            ResourceColor(resource.Handle),
            label);
    };

    auto getPassNodeId = [&](RGPassHandle handle) -> string {
        if (FindPass(handle) != nullptr) {
            return PassNodeId(handle);
        }
        return findOrAddMissingPass(handle);
    };

    auto emitEdge = [&](std::string_view src, std::string_view dst, const string& label) {
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} -> {} [label=\"{}\"];\n",
            src,
            dst,
            label);
    };

    for (const auto& resource : _resources) {
        versionStates.emplace(resource.Handle.Node.Value, ResourceVersionState{});
        ensureVersionNode(resource, 0);
    }

    auto getCurrentResourceNodeId = [&](RGResourceHandle handle) -> string {
        const auto* resource = FindResource(handle);
        if (resource == nullptr) {
            return findOrAddMissingResource(handle);
        }
        const auto it = versionStates.find(handle.Node.Value);
        if (it == versionStates.end()) {
            return findOrAddMissingResource(handle);
        }
        return ResourceVersionNodeId(handle, it->second.CurrentVersion);
    };

    auto createNextResourceNodeId = [&](RGResourceHandle handle) -> string {
        const auto* resource = FindResource(handle);
        if (resource == nullptr) {
            return findOrAddMissingResource(handle);
        }
        auto& state = versionStates[handle.Node.Value];
        ++state.CurrentVersion;
        ensureVersionNode(*resource, state.CurrentVersion);
        return ResourceVersionNodeId(handle, state.CurrentVersion);
    };

    for (const auto& use : _uses) {
        const auto passNodeId = getPassNodeId(use.Pass);
        const auto label = FormatUseLabel(use);
        if (IsReadUse(use.Kind) || IsReadWriteUse(use.Kind)) {
            emitEdge(getCurrentResourceNodeId(use.Resource), passNodeId, label);
        }
        if (IsWriteUse(use.Kind) || IsReadWriteUse(use.Kind)) {
            emitEdge(passNodeId, createNextResourceNodeId(use.Resource), label);
        }
    }

    for (size_t exportIndex = 0; exportIndex < _exports.size(); ++exportIndex) {
        const auto& exportRecord = _exports[exportIndex];
        emitEdge(
            getCurrentResourceNodeId(exportRecord.Resource),
            fmt::format("export_{}", exportIndex),
            EscapeGraphviz("Export"));
    }

    fmt::format_to(std::back_inserter(buffer), "}}\n");
    return string{buffer.data(), buffer.size()};
}

RGCompiledGraph RenderGraph::Compile() const {
    return {};
}

void RenderGraph::Execute(
    const RGCompiledGraph&,
    const RGExecutionEnvironment&) {
    throw RenderGraphException("RenderGraph::Execute is not implemented");
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadVertexBuffer(
    RGResourceHandle buffer,
    render::BufferRange range,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::VertexBufferRead, CopyName(name), range);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadIndexBuffer(
    RGResourceHandle buffer,
    render::BufferRange range,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::IndexBufferRead, CopyName(name), range);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadIndirectBuffer(
    RGResourceHandle buffer,
    render::BufferRange range,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::IndirectBufferRead, CopyName(name), range);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadConstantBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::ConstantBufferRead, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadStorageBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::StorageBufferRead, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadWriteStorageBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::StorageBufferReadWrite, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadSampledTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::SampledTextureRead, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadStorageTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::StorageTextureRead, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::ReadWriteStorageTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::StorageTextureReadWrite, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::BindColorAttachment(
    RGResourceHandle texture,
    const RGColorAttachmentDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::ColorAttachmentWrite, CopyName(name), desc);
    return *this;
}

RGGraphicsPassBuilder& RGGraphicsPassBuilder::BindDepthStencilAttachment(
    RGResourceHandle texture,
    const RGDepthStencilAttachmentDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGGraphicsPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::DepthStencilAttachmentWrite, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadConstantBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::ConstantBufferRead, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadStorageBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::StorageBufferRead, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadWriteStorageBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::StorageBufferReadWrite, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadSampledTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::SampledTextureRead, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadStorageTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::StorageTextureRead, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadWriteStorageTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::StorageTextureReadWrite, CopyName(name), desc);
    return *this;
}

RGComputePassBuilder& RGComputePassBuilder::ReadAccelerationStructure(
    RGResourceHandle accelerationStructure,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGComputePassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(
        _pass,
        accelerationStructure,
        RGUseKind::AccelerationStructureRead,
        CopyName(name),
        std::monostate{});
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadShaderTableBuffer(
    RGResourceHandle buffer,
    render::BufferRange range,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::ShaderTableRead, CopyName(name), range);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadConstantBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::ConstantBufferRead, CopyName(name), desc);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadStorageBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::StorageBufferRead, CopyName(name), desc);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadWriteStorageBuffer(
    RGResourceHandle buffer,
    const RGBufferShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, buffer, RGUseKind::StorageBufferReadWrite, CopyName(name), desc);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadSampledTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::SampledTextureRead, CopyName(name), desc);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadStorageTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::StorageTextureRead, CopyName(name), desc);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadWriteStorageTexture(
    RGResourceHandle texture,
    const RGTextureShaderAccessDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(_pass, texture, RGUseKind::StorageTextureReadWrite, CopyName(name), desc);
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::ReadAccelerationStructure(
    RGResourceHandle accelerationStructure,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    _graph->AppendUseRecord(
        _pass,
        accelerationStructure,
        RGUseKind::AccelerationStructureRead,
        CopyName(name),
        std::monostate{});
    return *this;
}

RGRayTracingPassBuilder& RGRayTracingPassBuilder::BuildAccelerationStructure(
    RGResourceHandle accelerationStructure,
    const RGAccelerationStructureBuildDesc& desc,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGRayTracingPassBuilder is not bound to a RenderGraph");
    }
    const auto edgeName = CopyName(name);
    _graph->AppendUseRecord(_pass, accelerationStructure, RGUseKind::AccelerationStructureBuild, edgeName, desc);
    _graph->AppendUseRecord(_pass, desc.ScratchBuffer, RGUseKind::BuildScratchBuffer, edgeName, desc.ScratchRange);
    return *this;
}

RGCopyPassBuilder& RGCopyPassBuilder::Copy(
    RGResourceHandle src,
    render::BufferRange srcRange,
    RGResourceHandle dst,
    render::BufferRange dstRange,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGCopyPassBuilder is not bound to a RenderGraph");
    }
    const auto edgeName = CopyName(name);
    _graph->AppendUseRecord(_pass, src, RGUseKind::CopyBufferRead, edgeName, srcRange);
    _graph->AppendUseRecord(_pass, dst, RGUseKind::CopyBufferWrite, edgeName, dstRange);
    return *this;
}

RGCopyPassBuilder& RGCopyPassBuilder::CopyBufferToTexture(
    RGResourceHandle src,
    render::BufferRange srcRange,
    RGResourceHandle dst,
    render::SubresourceRange dstRange,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGCopyPassBuilder is not bound to a RenderGraph");
    }
    const auto edgeName = CopyName(name);
    _graph->AppendUseRecord(_pass, src, RGUseKind::CopyBufferRead, edgeName, srcRange);
    _graph->AppendUseRecord(_pass, dst, RGUseKind::CopyTextureWrite, edgeName, dstRange);
    return *this;
}

RGCopyPassBuilder& RGCopyPassBuilder::CopyTextureToBuffer(
    RGResourceHandle src,
    render::SubresourceRange srcRange,
    RGResourceHandle dst,
    render::BufferRange dstRange,
    std::string_view name) {
    if (_graph == nullptr) {
        throw RenderGraphException("RGCopyPassBuilder is not bound to a RenderGraph");
    }
    const auto edgeName = CopyName(name);
    _graph->AppendUseRecord(_pass, src, RGUseKind::CopyTextureRead, edgeName, srcRange);
    _graph->AppendUseRecord(_pass, dst, RGUseKind::CopyBufferWrite, edgeName, dstRange);
    return *this;
}

}  // namespace radray
