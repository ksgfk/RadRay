#include <radray/runtime/render_graph.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <tuple>
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

string ResourceDisplayName(const RGCompiledResourceRecord& record) {
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

string PassDisplayName(const RGCompiledPassRecord& record) {
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

string FormatUseLabel(const RGCompiledUseRecord& record) {
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

string ResourceVersionDisplayName(const RGCompiledResourceRecord& record, uint32_t version) {
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

RGValidationIssue MakeIssue(
    RGValidationIssueCode code,
    string message,
    std::optional<RGResourceHandle> resource,
    std::optional<RGPassHandle> pass,
    RGValidationSeverity severity = RGValidationSeverity::Error) {
    return RGValidationIssue{
        .Severity = severity,
        .Code = code,
        .Message = std::move(message),
        .Resource = resource,
        .Pass = pass};
}

void AddIssue(
    RGValidationResult& result,
    RGValidationIssueCode code,
    string message,
    std::optional<RGResourceHandle> resource,
    std::optional<RGPassHandle> pass,
    RGValidationSeverity severity = RGValidationSeverity::Error) {
    result.Issues.push_back(MakeIssue(code, std::move(message), resource, pass, severity));
}

bool IsPassKeepAlive(const RGPassRecord& pass) noexcept {
    return pass.Flags.HasFlag(RGPassFlag::SideEffect) || pass.Flags.HasFlag(RGPassFlag::NoCull);
}

RGCompiledDependencyKind DependencyKindFor(RGUseKind fromUse, RGUseKind toUse) noexcept {
    if (IsReadUse(toUse)) {
        return RGCompiledDependencyKind::ReadAfterWrite;
    }
    if (IsReadUse(fromUse)) {
        return RGCompiledDependencyKind::WriteAfterRead;
    }
    return RGCompiledDependencyKind::WriteAfterWrite;
}

struct RGAnalyzedUseRecord {
    const RGUseRecord* Record{nullptr};
    uint32_t InputVersion{0};
    std::optional<uint32_t> OutputVersion{};
    bool Reads{false};
    bool Writes{false};
};

struct RGAnalyzedResourceVersion {
    std::optional<RGPassHandle> ProducerPass{};
    std::optional<RGUseKind> ProducerUse{};
    std::optional<size_t> ProducerUseIndex{};
    vector<RGPassHandle> ConsumerPasses{};
    vector<RGUseKind> ConsumerUses{};
    vector<size_t> ConsumerUseIndices{};
};

struct RGCompileAnalysis {
    vector<RGAnalyzedUseRecord> Uses{};
    vector<vector<size_t>> PassUses{};
    vector<vector<RGAnalyzedResourceVersion>> ResourceVersions{};
    vector<uint32_t> FinalVersions{};
};

struct RGDependencyKey {
    RGPassHandle FromPass{};
    RGPassHandle ToPass{};
    RGResourceHandle Resource{};
    RGUseKind FromUse{RGUseKind::VertexBufferRead};
    RGUseKind ToUse{RGUseKind::VertexBufferRead};
    RGCompiledDependencyKind Kind{RGCompiledDependencyKind::ReadAfterWrite};

    friend auto operator<=>(const RGDependencyKey&, const RGDependencyKey&) noexcept = default;
};

struct RGPassPair {
    RGPassHandle FromPass{};
    RGPassHandle ToPass{};

    friend auto operator<=>(const RGPassPair&, const RGPassPair&) noexcept = default;
};

}  // namespace

bool RGValidationResult::IsValid() const noexcept {
    for (const auto& issue : Issues) {
        if (issue.Severity == RGValidationSeverity::Error) {
            return false;
        }
    }
    return true;
}

bool RGCompiledGraph::IsEmpty() const noexcept {
    return Resources.empty() &&
        Passes.empty() &&
        Uses.empty() &&
        Exports.empty() &&
        Dependencies.empty() &&
        PassOrder.empty() &&
        ResourceVersions.empty();
}

const RGCompiledResourceRecord* RGCompiledGraph::FindResource(RGResourceHandle handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    for (const auto& resource : Resources) {
        if (resource.Handle == handle) {
            return &resource;
        }
    }
    return nullptr;
}

const RGCompiledPassRecord* RGCompiledGraph::FindPass(RGPassHandle handle) const noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    for (const auto& pass : Passes) {
        if (pass.Handle == handle) {
            return &pass;
        }
    }
    return nullptr;
}

bool RGCompileResult::Succeeded() const noexcept {
    return Graph.has_value();
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

string RGCompiledGraph::ToGraphviz() const {
    fmt::memory_buffer buffer{};
    fmt::format_to(std::back_inserter(buffer), "digraph RGCompiledGraph {{\n");
    fmt::format_to(std::back_inserter(buffer), "  rankdir=LR;\n");
    fmt::format_to(std::back_inserter(buffer), "  node [fontname=\"Segoe UI\"];\n");

    for (const auto& pass : Passes) {
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

    for (size_t exportIndex = 0; exportIndex < Exports.size(); ++exportIndex) {
        const auto& exportRecord = Exports[exportIndex];
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

    for (const auto& version : ResourceVersions) {
        const auto* resource = FindResource(version.Resource);
        if (resource == nullptr) {
            continue;
        }
        string label{};
        AppendLabelLine(label, fmt::format("{} Resource", ResourceKindName(resource->Kind)));
        AppendLabelLine(label, ResourceVersionDisplayName(*resource, version.Version));
        AppendLabelLine(label, resource->Imported ? "imported" : "local");
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} [shape=ellipse, style=\"filled\", fillcolor=\"{}\", color=\"#495057\", label=\"{}\"];\n",
            ResourceVersionNodeId(version.Resource, version.Version),
            ResourceColor(version.Resource),
            label);
    }

    auto emitEdge = [&](std::string_view src, std::string_view dst, const string& label) {
        fmt::format_to(
            std::back_inserter(buffer),
            "  {} -> {} [label=\"{}\"];\n",
            src,
            dst,
            label);
    };

    for (const auto& use : Uses) {
        const auto passNodeId = PassNodeId(use.Pass);
        const auto label = FormatUseLabel(use);
        if (IsReadUse(use.Kind) || IsReadWriteUse(use.Kind)) {
            emitEdge(ResourceVersionNodeId(use.Resource, use.InputVersion), passNodeId, label);
        }
        if (use.OutputVersion.has_value()) {
            emitEdge(passNodeId, ResourceVersionNodeId(use.Resource, *use.OutputVersion), label);
        }
    }

    for (size_t exportIndex = 0; exportIndex < Exports.size(); ++exportIndex) {
        const auto& exportRecord = Exports[exportIndex];
        emitEdge(
            ResourceVersionNodeId(exportRecord.Resource, exportRecord.Version),
            fmt::format("export_{}", exportIndex),
            EscapeGraphviz("Export"));
    }

    fmt::format_to(std::back_inserter(buffer), "}}\n");
    return string{buffer.data(), buffer.size()};
}

RGCompileResult RenderGraph::Compile() const {
    RGCompileResult result{};
    const auto validation = Validate();
    result.Diagnostics = validation.Issues;
    if (!validation.IsValid()) {
        return result;
    }

    RGCompileAnalysis analysis{};
    analysis.Uses.resize(_uses.size());
    analysis.PassUses.resize(_passes.size());
    analysis.ResourceVersions.resize(_resources.size());
    analysis.FinalVersions.assign(_resources.size(), 0);

    for (size_t resourceIndex = 0; resourceIndex < _resources.size(); ++resourceIndex) {
        analysis.ResourceVersions[resourceIndex].push_back(RGAnalyzedResourceVersion{});
    }

    for (size_t useIndex = 0; useIndex < _uses.size(); ++useIndex) {
        const auto& use = _uses[useIndex];
        const auto passIndex = static_cast<size_t>(use.Pass.Node.Value);
        const auto resourceIndex = static_cast<size_t>(use.Resource.Node.Value);
        analysis.PassUses[passIndex].push_back(useIndex);

        auto& analyzedUse = analysis.Uses[useIndex];
        analyzedUse.Record = &use;
        analyzedUse.Reads = IsReadUse(use.Kind) || IsReadWriteUse(use.Kind);
        analyzedUse.Writes = IsWriteUse(use.Kind) || IsReadWriteUse(use.Kind);
        analyzedUse.InputVersion = analysis.FinalVersions[resourceIndex];

        auto& versions = analysis.ResourceVersions[resourceIndex];
        auto& inputVersion = versions[analyzedUse.InputVersion];
        if (analyzedUse.Reads) {
            inputVersion.ConsumerPasses.push_back(use.Pass);
            inputVersion.ConsumerUses.push_back(use.Kind);
            inputVersion.ConsumerUseIndices.push_back(useIndex);
        }

        if (analyzedUse.Writes) {
            const auto outputVersion = static_cast<uint32_t>(versions.size());
            analyzedUse.OutputVersion = outputVersion;
            versions.push_back(RGAnalyzedResourceVersion{
                .ProducerPass = use.Pass,
                .ProducerUse = use.Kind,
                .ProducerUseIndex = useIndex});
            analysis.FinalVersions[resourceIndex] = outputVersion;
        }
    }

    vector<bool> livePasses(_passes.size(), false);
    vector<bool> liveResources(_resources.size(), false);
    vector<bool> liveUses(_uses.size(), false);
    vector<vector<bool>> liveVersions{};
    liveVersions.reserve(_resources.size());
    for (const auto& versions : analysis.ResourceVersions) {
        liveVersions.push_back(vector<bool>(versions.size(), false));
    }

    deque<RGPassHandle> passWork{};
    deque<std::pair<RGResourceHandle, uint32_t>> versionWork{};

    const auto markPassLive = [&](RGPassHandle pass) {
        const auto passIndex = static_cast<size_t>(pass.Node.Value);
        if (livePasses[passIndex]) {
            return;
        }
        livePasses[passIndex] = true;
        passWork.push_back(pass);
    };

    const auto markVersionLive = [&](RGResourceHandle resource, uint32_t version) {
        const auto resourceIndex = static_cast<size_t>(resource.Node.Value);
        if (liveVersions[resourceIndex][version]) {
            return;
        }
        liveVersions[resourceIndex][version] = true;
        liveResources[resourceIndex] = true;
        versionWork.emplace_back(resource, version);
    };

    for (const auto& exportRecord : _exports) {
        const auto resourceIndex = static_cast<size_t>(exportRecord.Resource.Node.Value);
        markVersionLive(exportRecord.Resource, analysis.FinalVersions[resourceIndex]);
    }

    for (const auto& pass : _passes) {
        if (IsPassKeepAlive(pass)) {
            markPassLive(pass.Handle);
        }
    }

    while (!passWork.empty() || !versionWork.empty()) {
        while (!passWork.empty()) {
            const auto pass = passWork.front();
            passWork.pop_front();
            const auto passIndex = static_cast<size_t>(pass.Node.Value);
            for (const auto useIndex : analysis.PassUses[passIndex]) {
                liveUses[useIndex] = true;
                const auto& use = _uses[useIndex];
                const auto resourceIndex = static_cast<size_t>(use.Resource.Node.Value);
                liveResources[resourceIndex] = true;

                const auto& analyzedUse = analysis.Uses[useIndex];
                if (analyzedUse.Reads) {
                    markVersionLive(use.Resource, analyzedUse.InputVersion);
                }
                if (analyzedUse.OutputVersion.has_value()) {
                    markVersionLive(use.Resource, *analyzedUse.OutputVersion);
                }
            }
        }

        while (!versionWork.empty()) {
            const auto [resource, version] = versionWork.front();
            versionWork.pop_front();
            const auto resourceIndex = static_cast<size_t>(resource.Node.Value);
            const auto& analyzedVersion = analysis.ResourceVersions[resourceIndex][version];
            if (analyzedVersion.ProducerPass.has_value()) {
                markPassLive(*analyzedVersion.ProducerPass);
            }
        }
    }

    vector<RGCompiledEdge> dependencies{};
    set<RGDependencyKey> dependencyKeys{};
    const auto appendDependency = [&](RGPassHandle fromPass,
                                      RGPassHandle toPass,
                                      RGResourceHandle resource,
                                      RGUseKind fromUse,
                                      RGUseKind toUse) {
        if (fromPass == toPass) {
            return;
        }
        const auto fromIndex = static_cast<size_t>(fromPass.Node.Value);
        const auto toIndex = static_cast<size_t>(toPass.Node.Value);
        if (!livePasses[fromIndex] || !livePasses[toIndex]) {
            return;
        }
        const auto kind = DependencyKindFor(fromUse, toUse);
        const RGDependencyKey key{
            .FromPass = fromPass,
            .ToPass = toPass,
            .Resource = resource,
            .FromUse = fromUse,
            .ToUse = toUse,
            .Kind = kind};
        if (!dependencyKeys.insert(key).second) {
            return;
        }
        dependencies.push_back(RGCompiledEdge{
            .FromPass = fromPass,
            .ToPass = toPass,
            .Resource = resource,
            .FromUse = fromUse,
            .ToUse = toUse,
            .Kind = kind});
    };

    for (size_t useIndex = 0; useIndex < _uses.size(); ++useIndex) {
        if (!liveUses[useIndex]) {
            continue;
        }

        const auto& use = _uses[useIndex];
        const auto& analyzedUse = analysis.Uses[useIndex];
        const auto resourceIndex = static_cast<size_t>(use.Resource.Node.Value);
        const auto& inputVersion = analysis.ResourceVersions[resourceIndex][analyzedUse.InputVersion];

        if (analyzedUse.Reads && inputVersion.ProducerPass.has_value() && inputVersion.ProducerUse.has_value()) {
            appendDependency(*inputVersion.ProducerPass, use.Pass, use.Resource, *inputVersion.ProducerUse, use.Kind);
        }

        if (analyzedUse.Writes) {
            if (inputVersion.ProducerPass.has_value() && inputVersion.ProducerUse.has_value()) {
                appendDependency(*inputVersion.ProducerPass, use.Pass, use.Resource, *inputVersion.ProducerUse, use.Kind);
            }

            for (size_t consumerIndex = 0; consumerIndex < inputVersion.ConsumerPasses.size(); ++consumerIndex) {
                appendDependency(
                    inputVersion.ConsumerPasses[consumerIndex],
                    use.Pass,
                    use.Resource,
                    inputVersion.ConsumerUses[consumerIndex],
                    use.Kind);
            }
        }
    }

    std::sort(
        dependencies.begin(),
        dependencies.end(),
        [](const RGCompiledEdge& lhs, const RGCompiledEdge& rhs) {
            return std::tie(
                       lhs.FromPass.Node.Value,
                       lhs.ToPass.Node.Value,
                       lhs.Resource.Node.Value,
                       lhs.Kind,
                       lhs.FromUse,
                       lhs.ToUse) <
                std::tie(
                       rhs.FromPass.Node.Value,
                       rhs.ToPass.Node.Value,
                       rhs.Resource.Node.Value,
                       rhs.Kind,
                       rhs.FromUse,
                       rhs.ToUse);
        });

    set<RGPassPair> dependencyPairs{};
    vector<vector<RGPassHandle>> outgoing(_passes.size());
    vector<uint32_t> indegree(_passes.size(), 0);
    for (const auto& dependency : dependencies) {
        const RGPassPair pair{.FromPass = dependency.FromPass, .ToPass = dependency.ToPass};
        if (!dependencyPairs.insert(pair).second) {
            continue;
        }
        const auto fromIndex = static_cast<size_t>(dependency.FromPass.Node.Value);
        const auto toIndex = static_cast<size_t>(dependency.ToPass.Node.Value);
        outgoing[fromIndex].push_back(dependency.ToPass);
        ++indegree[toIndex];
    }

    vector<RGPassHandle> passOrder{};
    passOrder.reserve(_passes.size());
    vector<bool> scheduled(_passes.size(), false);
    size_t livePassCount = 0;
    for (bool live : livePasses) {
        if (live) {
            ++livePassCount;
        }
    }

    while (passOrder.size() < livePassCount) {
        const RGPassRecord* nextPass = nullptr;
        for (const auto& pass : _passes) {
            const auto passIndex = static_cast<size_t>(pass.Handle.Node.Value);
            if (!livePasses[passIndex] || scheduled[passIndex] || indegree[passIndex] != 0) {
                continue;
            }
            nextPass = &pass;
            break;
        }

        if (nextPass == nullptr) {
            string cycleMessage{"Cyclic dependency detected among live render graph passes."};
            for (const auto& pass : _passes) {
                const auto passIndex = static_cast<size_t>(pass.Handle.Node.Value);
                if (!livePasses[passIndex] || scheduled[passIndex]) {
                    continue;
                }
                cycleMessage += "\n";
                cycleMessage += PassDisplayName(pass);
            }
            result.Diagnostics.push_back(MakeIssue(
                RGValidationIssueCode::CyclicDependency,
                cycleMessage,
                std::nullopt,
                std::nullopt));
            return result;
        }

        const auto passIndex = static_cast<size_t>(nextPass->Handle.Node.Value);
        scheduled[passIndex] = true;
        passOrder.push_back(nextPass->Handle);
        for (const auto toPass : outgoing[passIndex]) {
            const auto toIndex = static_cast<size_t>(toPass.Node.Value);
            --indegree[toIndex];
        }
    }

    vector<std::optional<uint32_t>> passOrders(_passes.size());
    for (uint32_t order = 0; order < passOrder.size(); ++order) {
        passOrders[static_cast<size_t>(passOrder[order].Node.Value)] = order;
    }

    result.Graph.emplace();
    auto& compiledGraph = *result.Graph;
    compiledGraph.PassOrder = passOrder;
    compiledGraph.Dependencies = dependencies;

    vector<vector<std::optional<uint32_t>>> versionRemaps(_resources.size());
    for (size_t resourceIndex = 0; resourceIndex < _resources.size(); ++resourceIndex) {
        if (!liveResources[resourceIndex]) {
            continue;
        }

        const auto& resource = _resources[resourceIndex];
        const auto& rawVersions = analysis.ResourceVersions[resourceIndex];
        vector<bool> keepVersions(rawVersions.size(), false);
        keepVersions[0] = true;

        for (size_t useIndex = 0; useIndex < _uses.size(); ++useIndex) {
            if (!liveUses[useIndex] || _uses[useIndex].Resource != resource.Handle) {
                continue;
            }

            const auto& analyzedUse = analysis.Uses[useIndex];
            if (analyzedUse.Reads) {
                keepVersions[analyzedUse.InputVersion] = true;
            }
            if (analyzedUse.OutputVersion.has_value()) {
                keepVersions[*analyzedUse.OutputVersion] = true;
            }
        }

        for (const auto& exportRecord : _exports) {
            if (exportRecord.Resource == resource.Handle) {
                keepVersions[analysis.FinalVersions[resourceIndex]] = true;
            }
        }

        auto& remap = versionRemaps[resourceIndex];
        remap.resize(rawVersions.size());
        for (uint32_t oldVersion = 0, nextVersion = 0; oldVersion < rawVersions.size(); ++oldVersion) {
            if (!keepVersions[oldVersion]) {
                continue;
            }
            remap[oldVersion] = nextVersion;
            const auto& rawVersion = rawVersions[oldVersion];

            vector<RGPassHandle> consumers{};
            for (size_t consumerIndex = 0; consumerIndex < rawVersion.ConsumerPasses.size(); ++consumerIndex) {
                const auto pass = rawVersion.ConsumerPasses[consumerIndex];
                const auto passIndex = static_cast<size_t>(pass.Node.Value);
                if (!livePasses[passIndex]) {
                    continue;
                }
                if (std::find(consumers.begin(), consumers.end(), pass) != consumers.end()) {
                    continue;
                }
                consumers.push_back(pass);
            }

            compiledGraph.ResourceVersions.push_back(RGCompiledResourceVersion{
                .Resource = resource.Handle,
                .Version = nextVersion,
                .ProducerPass = rawVersion.ProducerPass,
                .ProducerUse = rawVersion.ProducerUse,
                .ConsumerPasses = std::move(consumers)});
            ++nextVersion;
        }
    }

    for (const auto& passHandle : passOrder) {
        const auto& pass = _passes[static_cast<size_t>(passHandle.Node.Value)];
        compiledGraph.Passes.push_back(RGCompiledPassRecord{
            .Handle = pass.Handle,
            .Name = pass.Name,
            .Kind = pass.Kind,
            .Queue = pass.Queue,
            .Flags = pass.Flags,
            .Order = *passOrders[static_cast<size_t>(pass.Handle.Node.Value)]});
    }

    for (size_t resourceIndex = 0; resourceIndex < _resources.size(); ++resourceIndex) {
        if (!liveResources[resourceIndex]) {
            continue;
        }

        const auto& resource = _resources[resourceIndex];
        RGCompiledResourceLifetime lifetime{};
        bool exported = false;
        for (const auto& exportRecord : _exports) {
            if (exportRecord.Resource == resource.Handle) {
                exported = true;
                break;
            }
        }

        for (size_t useIndex = 0; useIndex < _uses.size(); ++useIndex) {
            if (!liveUses[useIndex] || _uses[useIndex].Resource != resource.Handle) {
                continue;
            }

            const auto passOrderIndex = *passOrders[static_cast<size_t>(_uses[useIndex].Pass.Node.Value)];
            if (!lifetime.FirstPassOrder.has_value() || passOrderIndex < *lifetime.FirstPassOrder) {
                lifetime.FirstPassOrder = passOrderIndex;
            }
            if (!lifetime.LastPassOrder.has_value() || passOrderIndex > *lifetime.LastPassOrder) {
                lifetime.LastPassOrder = passOrderIndex;
            }
            if ((IsWriteUse(_uses[useIndex].Kind) || IsReadWriteUse(_uses[useIndex].Kind)) &&
                (!lifetime.LastWriter.has_value() ||
                 passOrderIndex >= *passOrders[static_cast<size_t>(lifetime.LastWriter->Node.Value)])) {
                lifetime.LastWriter = _uses[useIndex].Pass;
            }
        }

        compiledGraph.Resources.push_back(RGCompiledResourceRecord{
            .Handle = resource.Handle,
            .Name = resource.Name,
            .Kind = resource.Kind,
            .Imported = resource.Imported,
            .Exported = exported,
            .Options = resource.Options,
            .Desc = resource.Desc,
            .InitialState = resource.InitialState,
            .Lifetime = lifetime});
    }

    for (size_t useIndex = 0; useIndex < _uses.size(); ++useIndex) {
        if (!liveUses[useIndex]) {
            continue;
        }

        const auto& use = _uses[useIndex];
        const auto& analyzedUse = analysis.Uses[useIndex];
        const auto resourceIndex = static_cast<size_t>(use.Resource.Node.Value);
        const auto& remap = versionRemaps[resourceIndex];

        uint32_t inputVersion = 0;
        if (analyzedUse.Reads) {
            inputVersion = *remap[analyzedUse.InputVersion];
        } else {
            for (int64_t version = analyzedUse.InputVersion; version >= 0; --version) {
                if (remap[static_cast<size_t>(version)].has_value()) {
                    inputVersion = *remap[static_cast<size_t>(version)];
                    break;
                }
            }
        }

        std::optional<uint32_t> outputVersion{};
        if (analyzedUse.OutputVersion.has_value()) {
            outputVersion = *remap[*analyzedUse.OutputVersion];
        }

        compiledGraph.Uses.push_back(RGCompiledUseRecord{
            .Pass = use.Pass,
            .Resource = use.Resource,
            .Kind = use.Kind,
            .Name = use.Name,
            .Data = use.Data,
            .InputVersion = inputVersion,
            .OutputVersion = outputVersion});
    }

    for (const auto& exportRecord : _exports) {
        const auto resourceIndex = static_cast<size_t>(exportRecord.Resource.Node.Value);
        const auto version = *versionRemaps[resourceIndex][analysis.FinalVersions[resourceIndex]];
        compiledGraph.Exports.push_back(RGCompiledExportRecord{
            .Resource = exportRecord.Resource,
            .ExpectedKind = exportRecord.ExpectedKind,
            .State = exportRecord.State,
            .Version = version});
    }

    return result;
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
