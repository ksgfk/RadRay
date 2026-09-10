#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/utility.h>

namespace radray {
struct RgReadbackTicket::Storage {
    shared_ptr<render::Buffer> Buffer;
    uint64_t Size{0};
};
bool RgReadbackTicket::Read(vector<byte>& destination) const {
    if (Status() != FrameOperationStatus::GpuCompleted || !_storage || !_storage->Buffer) return false;
    ScopedBufferMap map{_storage->Buffer.get(), {0, _storage->Size}};
    if (!map) return false;
    const auto* data = static_cast<const byte*>(map.Data());
    destination.assign(data, data + _storage->Size);
    return true;
}
namespace {
std::atomic<uint64_t> NextGraphGeneration{1};
constexpr uint32_t InvalidIndex = UINT32_MAX;

uint32_t StateFor(render::TextureViewUsage usage) {
    using enum render::TextureViewUsage;
    switch (usage) {
        case Resource: return static_cast<uint32_t>(render::TextureState::ShaderRead);
        case RenderTarget: return static_cast<uint32_t>(render::TextureState::RenderTarget);
        case DepthRead: return static_cast<uint32_t>(render::TextureState::DepthRead);
        case DepthWrite: return static_cast<uint32_t>(render::TextureState::DepthWrite);
        case UnorderedAccess: return static_cast<uint32_t>(render::TextureState::UnorderedAccess);
        default: return 0;
    }
}
render::TextureUse UsageFor(render::TextureViewUsage usage) {
    using enum render::TextureViewUsage;
    switch (usage) {
        case Resource: return render::TextureUse::Resource;
        case RenderTarget: return render::TextureUse::RenderTarget;
        case DepthRead: return render::TextureUse::DepthStencilRead;
        case DepthWrite: return render::TextureUse::DepthStencilWrite;
        case UnorderedAccess: return render::TextureUse::UnorderedAccess;
        default: return render::TextureUse::UNKNOWN;
    }
}
std::pair<render::BufferState, render::BufferUse> BufferAccessInfo(RgBufferAccess access) {
    using enum RgBufferAccess;
    switch (access) {
        case Vertex: return {render::BufferState::Vertex, render::BufferUse::Vertex};
        case Index: return {render::BufferState::Index, render::BufferUse::Index};
        case Constant: return {render::BufferState::CBuffer, render::BufferUse::CBuffer};
        case ShaderRead: return {render::BufferState::ShaderRead, render::BufferUse::Resource};
        case UnorderedAccess: return {render::BufferState::UnorderedAccess, render::BufferUse::UnorderedAccess};
        case Indirect: return {render::BufferState::Indirect, render::BufferUse::Indirect};
        case CopySource: return {render::BufferState::CopySource, render::BufferUse::CopySource};
        case CopyDestination: return {render::BufferState::CopyDestination, render::BufferUse::CopyDestination};
        case HostRead: return {render::BufferState::HostRead, render::BufferUse::MapRead};
    }
    return {render::BufferState::UNKNOWN, render::BufferUse::UNKNOWN};
}
bool ValidTextureState(const render::TextureDescriptor& desc, render::TextureStates state, bool allowUndefined) {
    using enum render::TextureState;
    if (state == Undefined) return allowUndefined;
    constexpr uint32_t known = uint32_t(Common) | uint32_t(Present) | uint32_t(CopySource) | uint32_t(CopyDestination) | uint32_t(ShaderRead) | uint32_t(RenderTarget) | uint32_t(DepthRead) | uint32_t(DepthWrite) | uint32_t(UnorderedAccess) | uint32_t(ResolveSource) | uint32_t(ResolveDestination);
    if (!state || (state.value() & ~known)) return false;
    const auto depthSampling = render::TextureState::DepthRead | render::TextureState::ShaderRead;
    if ((state.value() & (state.value() - 1)) && state != depthSampling) return false;
    if (state.HasFlag(Present) && !desc.Hints.HasFlag(render::ResourceHint::External)) return false;
    for (const auto [bit, usage] : {std::pair{CopySource, render::TextureUse::CopySource}, {CopyDestination, render::TextureUse::CopyDestination}, {ShaderRead, render::TextureUse::Resource}, {RenderTarget, render::TextureUse::RenderTarget}, {DepthRead, render::TextureUse::DepthStencilRead}, {DepthWrite, render::TextureUse::DepthStencilWrite}, {UnorderedAccess, render::TextureUse::UnorderedAccess}, {ResolveSource, render::TextureUse::CopySource}, {ResolveDestination, render::TextureUse::CopyDestination}})
        if (state.HasFlag(bit) && !desc.Usage.HasFlag(usage)) return false;
    return true;
}
void AddUnique(vector<uint32_t>& values, uint32_t value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

bool IsReadAccess(RgParameterAccess access) noexcept {
    return access == RgParameterAccess::Read || access == RgParameterAccess::ReadWrite;
}

bool IsWriteAccess(RgParameterAccess access) noexcept {
    return access == RgParameterAccess::Write || access == RgParameterAccess::ReadWrite;
}

render::ShaderStages ProgramStages(const ShaderProgram& program) noexcept {
    render::ShaderStages stages{render::ShaderStage::UNKNOWN};
    for (const shader::WireEntryRecord& entry : program.GetArtifact().Generic().Entries()) {
        switch (static_cast<shader::ShaderStage>(entry.Stage)) {
            case shader::ShaderStage::Vertex: stages |= render::ShaderStage::Vertex; break;
            case shader::ShaderStage::Pixel: stages |= render::ShaderStage::Pixel; break;
            case shader::ShaderStage::Compute: stages |= render::ShaderStage::Compute; break;
        }
    }
    return stages;
}

struct GraphCompileWorkspace {
    RenderGraphCompilerWorkspace Compiler;
    vector<RgResourceVersionNode> Versions;
    vector<RgExecutionNode> Nodes;
    vector<vector<vector<uint32_t>>> Values;
    vector<vector<vector<uint32_t>>> Producers;
    vector<vector<vector<uint8_t>>> Initialized;
    vector<uint32_t> Roots;
    struct PlanCache {
        uint64_t Hash{0};
        uint32_t ResourceCount{0};
        vector<RgResourceVersionNode> Versions;
        vector<RgExecutionNode> Nodes;
        vector<uint32_t> Roots;
        RenderGraphCompileOptions Options{};
        CompiledRenderGraph Result;
        uint64_t Hits{0}, Misses{0};
        bool Occupied{false};
    } Cache;
};

void PlotGraphCpuStats(const RenderGraphExecutionReport& report) {
    RADRAY_PROFILE_PLOT("RG.DeclaredPasses", static_cast<int64_t>(report.DeclaredPasses));
    RADRAY_PROFILE_PLOT("RG.LivePasses", static_cast<int64_t>(report.LivePasses));
    RADRAY_PROFILE_PLOT("RG.CompilePlanReused", report.CompilePlanReused ? int64_t{1} : int64_t{0});
    RADRAY_PROFILE_PLOT("RG.PhysicalAllocations", static_cast<int64_t>(report.PhysicalAllocations));
    RADRAY_PROFILE_PLOT("RG.GraphicsPipelinePreparations", static_cast<int64_t>(report.GraphicsPipelinePreparations));
    RADRAY_PROFILE_PLOT("RG.GraphicsPipelineCreations", static_cast<int64_t>(report.GraphicsPipelineCreations));
    RADRAY_PROFILE_PLOT("RG.MergedRasterPasses", static_cast<int64_t>(report.MergedRasterPasses));
    RADRAY_PROFILE_PLOT("RG.BarrierBatches", static_cast<int64_t>(report.BarrierBatches));
}
}  // namespace

struct RenderGraphFrameResources::Impl {
    struct ParameterValue {
        string Declaration;
        uint32_t ArrayElement{0};
        render::ShaderParameterValue Value;

        friend bool operator==(const ParameterValue&, const ParameterValue&) = default;
    };
    struct ParameterSetKey {
        render::PipelineLayout* Layout{nullptr};
        uint32_t Group{0};
        vector<ParameterValue> Values;

        friend bool operator==(const ParameterSetKey&, const ParameterSetKey&) = default;
    };
    struct ParameterSetKeyHash {
        size_t operator()(const ParameterSetKey& key) const noexcept {
            size_t result = std::hash<render::PipelineLayout*>{}(key.Layout);
            const auto mix = [&](size_t value) {
                result ^= value + size_t{0x9e3779b9u} + (result << 6) + (result >> 2);
            };
            mix(key.Group);
            for (const ParameterValue& binding : key.Values) {
                mix(std::hash<string>{}(binding.Declaration));
                mix(binding.ArrayElement);
                mix(binding.Value.index());
                std::visit(
                    [&](const auto& value) {
                        using T = std::decay_t<decltype(value)>;
                        if constexpr (std::is_same_v<T, render::ShaderBufferBinding>) {
                            mix(std::hash<render::Buffer*>{}(value.Target));
                            mix(std::hash<uint64_t>{}(value.Range.Offset));
                            mix(std::hash<uint64_t>{}(value.Range.Size));
                            mix(value.StructureByteStride);
                        } else if constexpr (std::is_same_v<T, render::ShaderTexelBufferBinding>) {
                            mix(std::hash<render::Buffer*>{}(value.Target));
                            mix(std::hash<uint64_t>{}(value.Range.Offset));
                            mix(std::hash<uint64_t>{}(value.Range.Size));
                            mix(static_cast<size_t>(value.Format));
                        } else {
                            mix(std::hash<T>{}(value));
                        }
                    },
                    binding.Value);
            }
            return result;
        }
    };

    Impl(render::Device& device, render::RenderPassRegistry& registry)
        : Device(device), Pool(device, registry) {}

    render::Device& Device;
    RenderResourcePool Pool;
    Nullable<HostWriteBatch*> HostWrites{nullptr};
    unique_ptr<DynamicCBufferArena> Arena;
    // Sets are released before the upload pages and pooled resources they reference.
    vector<unique_ptr<render::ShaderParameterSet>> Sets;
    unordered_map<ParameterSetKey, render::ShaderParameterSet*, ParameterSetKeyHash> SetCache;
    GraphCompileWorkspace CompileWorkspace;
};

RenderGraphFrameResources::RenderGraphFrameResources(
    render::Device& device, render::RenderPassRegistry& registry)
    : _impl(make_unique<Impl>(device, registry)) {}

RenderGraphFrameResources::~RenderGraphFrameResources() noexcept = default;

void RenderGraphFrameResources::BeginFlight(uint64_t serial, HostWriteBatch& hostWrites) {
    auto& impl = *_impl;
    impl.SetCache.clear();
    impl.Sets.clear();
    if (!impl.Arena || impl.HostWrites.Get() != &hostWrites) {
        DynamicCBufferArena::Descriptor desc;
        desc.Alignment = std::max<uint64_t>(
            desc.Alignment, std::max<uint64_t>(impl.Device.GetDetail().CBufferAlignment, 1));
        desc.NamePrefix = "RenderGraph.Parameters";
        impl.Arena = make_unique<DynamicCBufferArena>(&impl.Device, &hostWrites, desc);
        impl.HostWrites = &hostWrites;
    } else {
        impl.Arena->Reset();
    }
    impl.Pool.BeginFlight(serial);
}

RenderResourcePool& RenderGraphFrameResources::GetPool() noexcept { return _impl->Pool; }
const RenderResourcePoolStats& RenderGraphFrameResources::GetPoolStats() const noexcept { return _impl->Pool.GetStats(); }
size_t RenderGraphFrameResources::GetParameterSetCount() const noexcept { return _impl->Sets.size(); }

void RenderGraphFrameResources::Clear() {
    _impl->SetCache.clear();
    _impl->Sets.clear();
    if (_impl->Arena) _impl->Arena->Clear();
    _impl->Pool.Clear();
    _impl->CompileWorkspace = {};
}

struct RenderGraph::Impl {
    struct Resource {
        string Name;
        std::source_location Location;
        bool IsTexture{false};
        uint32_t Physical{InvalidIndex};
        vector<uint32_t> VersionParents{InvalidIndex, 0};
        vector<uint64_t> BufferBoundaries;
        bool Port{false}, Immutable{false};
        uint32_t Connection{InvalidIndex}, ConnectionVersion{0};
        vector<std::pair<uint32_t, uint32_t>> ResolvedValues;
        render::TextureDescriptor TextureDesc;
        render::BufferDescriptor BufferDesc;
        Nullable<RenderExternalTexture*> ExternalTexture{nullptr};
        Nullable<RenderExternalBuffer*> ExternalBuffer{nullptr};
        vector<RenderExternalTexture*> TextureImports;
        vector<RenderExternalBuffer*> BufferImports;
        RenderGraphExternalAccess ExternalAccess{RenderGraphExternalAccess::ReadWrite};
        Nullable<PooledTexture*> PoolTexture{nullptr};
        Nullable<PooledBuffer*> PoolBuffer{nullptr};
        shared_ptr<RgReadbackTicket::Storage> Readback;
        vector<uint32_t> States;
        vector<uint8_t> Valid;
        bool Written{false};
        uint64_t ViewId{0};
        uint32_t SubresourceCount() const { return TextureDesc.MipLevels * (TextureDesc.Dim == render::TextureDimension::Dim3D ? 1 : TextureDesc.DepthOrArraySize); }
        uint32_t AspectCount() const { return render::GetTextureFormatAspects(TextureDesc.Format).HasFlag(render::TextureAspect::Stencil) ? 2u : 1u; }
        uint32_t PhysicalCell(uint32_t cell) const { return IsTexture ? cell % SubresourceCount() : 0; }
        uint32_t CellCount() const { return IsTexture ? SubresourceCount() * AspectCount() : static_cast<uint32_t>(BufferBoundaries.size() > 1 ? BufferBoundaries.size() - 1 : 1); }
        render::Texture* NativeTexture() const { return ExternalTexture ? ExternalTexture->Texture : PoolTexture->Texture.get(); }
        render::Buffer* NativeBuffer() const { return ExternalBuffer ? ExternalBuffer->Buffer : Readback ? Readback->Buffer.get()
                                                                                                         : PoolBuffer->Buffer.get(); }
        bool External() const { return ExternalTexture || ExternalBuffer; }
    };
    struct View {
        uint32_t Resource;
        TextureViewKey Key;
        Nullable<render::TextureView*> Native;
        uint32_t Version{0};
    };
    struct Access {
        uint32_t Resource;
        render::SubresourceRange Range;
        uint32_t State;
        bool Read, Write, ValidAfter;
        uint32_t Version{0};
        render::BufferRange Bytes{render::BufferRange::AllRange()};
        render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    };
    struct CellAccess {
        uint32_t Resource, Cell, State;
        bool Read, Write, ValidAfter;
        uint32_t Version{0};
        render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
    };
    struct PhysicalAccess {
        uint32_t Resource, Physical, Cell, State;
        bool Write;
        render::ShaderStages Stages;
    };
    struct PassExecutionPlan {
        vector<PhysicalAccess> Accesses;
        vector<render::ResourceBarrierDescriptor> Barriers;
    };
    struct SubmissionState {
        struct TextureCommit {
            Nullable<PooledTexture*> Pool{nullptr};
            vector<RenderExternalTexture*> Imports;
            vector<render::TextureStates> States;
            vector<uint8_t> Valid;
            bool Written{false};
        };
        struct BufferCommit {
            Nullable<PooledBuffer*> Pool{nullptr};
            vector<RenderExternalBuffer*> Imports;
            render::BufferStates State;
            bool Valid{false}, Written{false};
        };
        vector<TextureCommit> Textures;
        vector<BufferCommit> Buffers;

        void Commit() {
            for (const auto& texture : Textures) {
                if (texture.Pool) std::copy(texture.States.begin(), texture.States.end(), texture.Pool->States.begin());
                for (auto* sink : texture.Imports) {
                    std::copy(texture.States.begin(), texture.States.end(), sink->SubresourceStates.begin());
                    std::copy(texture.Valid.begin(), texture.Valid.end(), sink->ContentValid.begin());
                    sink->Written = texture.Written;
                }
            }
            for (const auto& buffer : Buffers) {
                if (buffer.Pool) buffer.Pool->State = buffer.State;
                for (auto* sink : buffer.Imports) {
                    sink->State = buffer.State;
                    sink->ContentValid = buffer.Valid;
                    sink->Written = buffer.Written;
                }
            }
        }
    };
    struct SubmissionResources {
        vector<shared_ptr<void>> Owners;
        vector<shared_ptr<RgReadbackTicket::Storage>> Readbacks;
        // Generic payloads may own GPU resources, so their lifetime still extends to completion.
        vector<unique_ptr<Payload>> Payloads;
        vector<shared_ptr<FrameSubmission>> Tickets;

        void Submit(uint64_t serial) {
            for (const auto& ticket : Tickets) {
                if (ticket->Status() == FrameOperationStatus::Recorded) ticket->Submit(serial);
                else ticket->Cancel();
            }
        }
        void Complete(uint64_t serial, bool success) {
            for (const auto& ticket : Tickets) {
                if (ticket->Status() == FrameOperationStatus::Submitted) ticket->Complete(serial, success);
                else ticket->Cancel();
            }
        }
    };
    struct SubmissionData {
        shared_ptr<SubmissionState> State;
        shared_ptr<SubmissionResources> Retained;
    };
    struct Color {
        uint32_t View;
        RgColorAttachmentDesc Desc;
    };
    struct Depth {
        uint32_t View;
        RgDepthAttachmentDesc Desc;
    };
    enum class CopyType { Buffer,
                          Texture,
                          TextureToBuffer,
                          BufferToTexture,
                          Resolve };
    struct Copy {
        CopyType Type;
        uint32_t Source, Destination;
        uint64_t Size{0}, SourceOffset{0}, DestinationOffset{0};
        render::SubresourceRange SourceRange{0, 1, 0, 1}, DestinationRange{0, 1, 0, 1};
        render::BufferTextureCopyRegion Upload{};
    };
    struct IndirectArguments {
        uint32_t Pass{InvalidIndex};
        uint32_t Resource{InvalidIndex};
        RgIndirectCommand Command{RgIndirectCommand::Draw};
        uint64_t Offset{0};
        uint32_t Count{0};
    };
    struct ComputeProgram {
        uint32_t Pass{InvalidIndex};
        ShaderProgram* Program{nullptr};
        Nullable<render::ComputePipelineState*> PipelineState{nullptr};
    };
    struct GraphicsProgram {
        uint32_t Pass;
        ShaderProgram* Program;
        MaterialPipelineState State;
        PrimitiveVertexLayout Layout;
        PrimitiveTopology Topology;
        Nullable<render::GraphicsPipelineState*> PipelineState{nullptr};
    };
    struct CBufferBytes {
        vector<byte> Bytes;
    };
    struct TextureParameter {
        uint32_t View{InvalidIndex};
        uint32_t Resource{InvalidIndex};
    };
    struct BufferParameter {
        uint32_t Resource{InvalidIndex};
        render::BufferRange Range{};
        uint32_t StructureByteStride{0};
        render::TextureFormat Format{render::TextureFormat::UNKNOWN};
    };
    struct SamplerParameter {
        render::SamplerDescriptor Desc{};
    };
    using ParameterValue = std::variant<CBufferBytes, TextureParameter, BufferParameter, SamplerParameter>;
    struct ParameterBinding {
        string Declaration;
        uint32_t ArrayElement{0};
        render::ShaderBindingInfo Info{};
        ParameterValue Value;
    };
    struct ParameterSet {
        uint32_t Pass{InvalidIndex};
        ShaderProgram* Program{nullptr};
        uint32_t Group{0};
        vector<ParameterBinding> Bindings;
        Nullable<render::ShaderParameterSet*> Native{nullptr};
        vector<render::ShaderParameterDynamicOffset> DynamicOffsets;
    };
    struct Pass {
        std::source_location Location;
        unique_ptr<Payload> Data;
        vector<Access> Accesses;
        vector<CellAccess> Cells;
        vector<uint32_t> DeclaredViews, DeclaredBuffers;
        unordered_map<render::Buffer*, uint32_t> BufferReadStates;
        vector<std::optional<Color>> Colors;
        std::optional<Depth> DepthAttachment;
        std::optional<Copy> CopyOp;
        bool SideEffect{false};
        uint32_t MergeTail{InvalidIndex};
        Nullable<render::RenderPass*> NativePass{nullptr};
        Nullable<render::Framebuffer*> Framebuffer{nullptr};
        std::optional<GraphicsPassState> PassState;
        vector<render::ColorClearValue> Clears;
        uint32_t Width{0}, Height{0}, Layers{0}, Samples{0};
        render::ShaderStages UavWriteStages{render::ShaderStage::UNKNOWN};
        bool AllowUavWrites{false};
        vector<byte> UploadBytes;
        RgOperationTicket Ticket;
    };
    render::Device& Device;
    RenderResourcePool& Pool;
    render::RenderPassRegistry& Registry;
    Nullable<RenderGraphFrameResources*> FrameResources{nullptr};
    uint64_t Generation;
    uint64_t GenerationSerial{0};
    uint64_t ResourceView{0};
    bool Frozen{false}, Compiled{false}, Executed{false};
    vector<Resource> Resources;
    vector<View> Views;
    vector<Pass> Passes;
    vector<IndirectArguments> IndirectArgumentsRecords;
    vector<ComputeProgram> ComputePrograms;
    vector<GraphicsProgram> GraphicsPrograms;
    unordered_map<ShaderProgram*, vector<uint32_t>> GraphicsProgramIndices;
    unordered_map<render::Buffer*, uint32_t> NativeBuffers;
    vector<ParameterSet> ParameterSets;
    vector<shared_ptr<void>> Owners;
    RenderGraphCompileOptions Options;
    CompiledRenderGraph CompiledGraph;
    vector<PassExecutionPlan> ExecutionPlan;
    RenderGraphExecutionReport OwnedReport;
    RenderGraphExecutionReport& Report;

    Impl(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry,
         Nullable<RenderGraphFrameResources*> frameResources, std::string_view name, Nullable<RenderGraphExecutionReport*> report = nullptr)
        : Device(device), Pool(pool), Registry(registry), FrameResources(frameResources), Generation(NextGraphGeneration.fetch_add(1, std::memory_order_relaxed)),
          Report(report ? *report : OwnedReport) {
        if (Generation == 0 || Generation == UINT64_MAX) RADRAY_ABORT("RenderGraph generation exhausted");
        GenerationSerial = Pool.GetFrameSerial();
        Report.Name = name;
    }
    void Error(std::string_view code, std::string_view message, uint32_t pass = InvalidIndex,
               uint32_t resource = InvalidIndex, std::string_view binding = {}) {
        auto location = pass < Passes.size() ? Passes[pass].Location : resource < Resources.size() ? Resources[resource].Location
                                                                                                   : std::source_location::current();
        string detail{message};
        if (resource < Resources.size()) {
            const auto& entry = Resources[resource];
            if (entry.IsTexture) {
                const auto& desc = entry.TextureDesc;
                detail += fmt::format(" [dimension={}, {}x{}x{}, mips={}, samples={}, format={}, usage={}]", EnumName(desc.Dim), desc.Width, desc.Height,
                                      desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, EnumName(desc.Format), desc.Usage.value());
            } else
                detail += fmt::format(" [buffer size={}, memory={}, usage={}]", entry.BufferDesc.Size, EnumName(entry.BufferDesc.Memory), entry.BufferDesc.Usage.value());
        }
        Report.Diagnostics.push_back({string{code}, Report.Name, pass < Passes.size() ? Report.Passes[pass].Name : string{}, string{binding},
                                      resource < Resources.size() ? Resources[resource].Name : string{}, std::move(detail), string{location.file_name()}, location.line()});
    }
    bool Mutable() {
        if (!Frozen) return true;
        Error("GraphFrozen", "Setup cannot change a compiled or executed graph");
        return false;
    }
    bool Handle(uint32_t index, uint64_t generation, bool texture, uint32_t pass = InvalidIndex) {
        if (generation != Generation || index >= Resources.size() || Resources[index].IsTexture != texture) {
            Error("InvalidHandle", fmt::format("Handle generation {} does not belong to graph generation {}, or its type/index is invalid", generation, Generation), pass);
            return false;
        }
        return true;
    }
    SubmissionData DetachSubmissionResources();
    bool ValidateResources();
    bool ResolvePorts();
    bool NormalizePasses();
    void Cull();
    void PlanStorage();
    void BuildExecutionPlan();
    bool Realize();
    void PlanBarriers();
    void OptimizeRaster();
};

RenderGraph::Impl::SubmissionData RenderGraph::Impl::DetachSubmissionResources() {
    SubmissionData submission{make_shared<SubmissionState>(), make_shared<SubmissionResources>()};
    auto& retained = submission.Retained;
    retained->Owners = Owners;
    for (auto& pass : Passes) {
        if (pass.Data) retained->Payloads.push_back(std::move(pass.Data));
        if (pass.Ticket._state) retained->Tickets.push_back(pass.Ticket._state);
    }
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        if (resource.Readback) retained->Readbacks.push_back(resource.Readback);
        if (resource.States.empty() || resource.Physical != index) continue;
        if (resource.IsTexture) {
            if (!resource.ExternalTexture && !resource.PoolTexture) continue;
            auto& commit = submission.State->Textures.emplace_back();
            commit.Pool = resource.PoolTexture;
            commit.Imports = std::move(resource.TextureImports);
            commit.Written = resource.Written;
            commit.States.reserve(resource.States.size());
            for (const auto state : resource.States) commit.States.push_back(static_cast<render::TextureState>(state));
            if (resource.ExternalTexture) {
                const auto count = resource.ExternalTexture->ContentValid.size();
                commit.Valid.resize(count);
                for (uint32_t cell = 0; cell < count; ++cell) {
                    commit.Valid[cell] = resource.Valid[cell];
                    if (count == resource.SubresourceCount() && resource.AspectCount() == 2)
                        commit.Valid[cell] &= resource.Valid[cell + resource.SubresourceCount()];
                }
            }
        } else if (resource.ExternalBuffer || resource.PoolBuffer) {
            auto& commit = submission.State->Buffers.emplace_back();
            commit.Pool = resource.PoolBuffer;
            commit.Imports = std::move(resource.BufferImports);
            commit.State = static_cast<render::BufferState>(resource.States[0]);
            commit.Valid = std::all_of(resource.Valid.begin(), resource.Valid.end(), [](uint8_t valid) { return valid != 0; });
            commit.Written = resource.Written;
        }
    }
    return submission;
}

RenderGraph::RenderGraph(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry, std::string_view name)
    : _impl(make_unique<Impl>(device, pool, registry, nullptr, name)) {}
RenderGraph::RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                         render::RenderPassRegistry& registry, std::string_view name)
    : _impl(make_unique<Impl>(device, resources.GetPool(), registry, &resources, name)) {}
RenderGraph::RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                         render::RenderPassRegistry& registry, std::string_view name, uint64_t& generation, RenderGraphExecutionReport& report)
    : _impl(make_unique<Impl>(device, resources.GetPool(), registry, &resources, name, &report)) { generation = _impl->Generation; }
RenderGraph::~RenderGraph() = default;
uint64_t RenderGraph::GetGeneration() const noexcept { return _impl->Generation; }
uint64_t RenderGraph::SetResourceView(uint64_t viewId) {
    if (!_impl->Mutable()) return _impl->ResourceView;
    return std::exchange(_impl->ResourceView, viewId);
}
bool RenderGraph::WasPassExecuted(RgPassHandle handle) const noexcept {
    return handle.Generation == _impl->Generation && handle.Index < _impl->Report.Passes.size() && _impl->Report.Passes[handle.Index].Executed;
}
bool RenderGraph::PassWroteTexture(RgPassHandle pass, RgTextureValue texture) const noexcept {
    if (!WasPassExecuted(pass) || texture.Generation != _impl->Generation || texture.Index >= _impl->Resources.size()) return false;
    if (_impl->Resources[texture.Index].Port && texture.Version < _impl->Resources[texture.Index].ResolvedValues.size()) {
        const auto mapped = _impl->Resources[texture.Index].ResolvedValues[texture.Version];
        texture.Index = mapped.first;
        texture.Version = mapped.second;
    }
    if (texture.Index >= _impl->Resources.size()) return false;
    const auto& resource = _impl->Resources[texture.Index];
    if (!resource.IsTexture || !resource.Written) return false;
    for (const auto& access : _impl->Passes[pass.Index].Cells)
        if (access.Resource == texture.Index && access.Version == texture.Version && access.Write && access.ValidAfter && resource.Valid[access.Cell]) return true;
    return false;
}

RgPassHandle RenderGraphPassBuilder::GetPassHandle() const noexcept { return {_pass, _graph.GetGeneration()}; }
bool RenderGraphPassBuilder::OwnsParameterSet(RgParameterSetHandle handle, const ShaderProgram& program, uint32_t group) const noexcept {
    const auto& impl = *_graph._impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.ParameterSets.size()) return false;
    const auto& set = impl.ParameterSets[handle.Index];
    return set.Pass == _pass && set.Program == &program && set.Group == group;
}
void RenderGraphPassBuilder::Reject(std::string_view code, std::string_view message, std::string_view binding) {
    _graph._impl->Error(code, message, _pass, InvalidIndex, binding);
}

RgPassHandle RenderGraphRasterContext::GetPassHandle() const noexcept { return {_pass, _graph.GetGeneration()}; }
bool RenderGraphRasterContext::OwnsParameterSet(RgParameterSetHandle handle, const ShaderProgram& program, uint32_t group) const noexcept {
    const auto& impl = *_graph._impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.ParameterSets.size()) return false;
    const auto& set = impl.ParameterSets[handle.Index];
    return set.Pass == _pass && set.Program == &program && set.Group == group && set.Native.HasValue();
}
const RenderGraphExecutionReport& RenderGraph::GetReport() const noexcept { return _impl->Report; }

RgTextureValue RenderGraph::CreateTexture(const render::TextureDescriptor& desc, std::string_view name, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    const auto index = static_cast<uint32_t>(impl.Resources.size());
    Impl::Resource resource{};
    resource.Name = name;
    resource.Location = location;
    resource.IsTexture = true;
    resource.TextureDesc = desc;
    resource.ViewId = impl.ResourceView;
    impl.Resources.push_back(std::move(resource));
    return {index, impl.Generation, 1};
}
RgBufferValue RenderGraph::CreateBuffer(const render::BufferDescriptor& desc, std::string_view name, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    const auto index = static_cast<uint32_t>(impl.Resources.size());
    Impl::Resource resource{};
    resource.Name = name;
    resource.Location = location;
    resource.BufferDesc = desc;
    resource.ViewId = impl.ResourceView;
    impl.Resources.push_back(std::move(resource));
    return {index, impl.Generation, 1};
}
RgTextureValue RenderGraph::ImportTexture(RenderExternalTexture& texture, std::string_view name, RenderGraphExternalAccess access, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    for (uint32_t i = 0; i < impl.Resources.size(); ++i) {
        auto& existing = impl.Resources[i];
        if (!existing.ExternalTexture || existing.ExternalTexture->Texture != texture.Texture) continue;
        const auto& first = *existing.ExternalTexture;
        if (!(TexturePoolKey{first.Desc} == TexturePoolKey{texture.Desc}) || !std::equal(first.SubresourceStates.begin(), first.SubresourceStates.end(), texture.SubresourceStates.begin(), texture.SubresourceStates.end()) ||
            !std::equal(first.ContentValid.begin(), first.ContentValid.end(), texture.ContentValid.begin(), texture.ContentValid.end())) {
            impl.Error("ConflictingImport", "One native texture identity has conflicting descriptors, states or content validity");
            return {};
        }
        existing.ExternalAccess = static_cast<RenderGraphExternalAccess>(std::max(uint8_t(existing.ExternalAccess), uint8_t(access)));
        if (std::find(existing.TextureImports.begin(), existing.TextureImports.end(), &texture) == existing.TextureImports.end()) existing.TextureImports.push_back(&texture);
        Retain(texture.Owner);
        return {i, impl.Generation, 0};
    }
    auto result = CreateTexture(texture.Desc, name, location);
    if (result.IsValid()) {
        auto& resource = impl.Resources[result.Index];
        resource.ExternalTexture = &texture;
        resource.TextureImports.push_back(&texture);
        Retain(texture.Owner);
        resource.VersionParents.resize(1);
        result.Version = 0;
        resource.ExternalAccess = access;
    }
    return result;
}
RgBufferValue RenderGraph::ImportBuffer(RenderExternalBuffer& buffer, std::string_view name, RenderGraphExternalAccess access, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    for (uint32_t i = 0; i < impl.Resources.size(); ++i) {
        auto& existing = impl.Resources[i];
        if (!existing.ExternalBuffer || existing.ExternalBuffer->Buffer != buffer.Buffer) continue;
        if (existing.Immutable && access != RenderGraphExternalAccess::ReadOnly) {
            impl.Error("ImmutableAssetWrite", "An immutable asset cannot be imported for writing");
            return {};
        }
        const auto& first = *existing.ExternalBuffer;
        if (!(BufferPoolKey{first.Desc} == BufferPoolKey{buffer.Desc}) || first.State != buffer.State || first.ContentValid != buffer.ContentValid) {
            impl.Error("ConflictingImport", "One native buffer identity has conflicting descriptors, states or content validity");
            return {};
        }
        existing.ExternalAccess = static_cast<RenderGraphExternalAccess>(std::max(uint8_t(existing.ExternalAccess), uint8_t(access)));
        if (std::find(existing.BufferImports.begin(), existing.BufferImports.end(), &buffer) == existing.BufferImports.end()) existing.BufferImports.push_back(&buffer);
        Retain(buffer.Owner);
        return {i, impl.Generation, 0};
    }
    auto result = CreateBuffer(buffer.Desc, name, location);
    if (result.IsValid()) {
        auto& resource = impl.Resources[result.Index];
        resource.ExternalBuffer = &buffer;
        resource.BufferImports.push_back(&buffer);
        Retain(buffer.Owner);
        resource.VersionParents.resize(1);
        result.Version = 0;
        resource.ExternalAccess = access;
    }
    return result;
}
void RenderGraph::Retain(shared_ptr<void> owner) {
    if (owner && _impl->Mutable()) _impl->Owners.push_back(std::move(owner));
}
RgBufferValue RenderGraph::UploadBuffer(std::string_view name, std::span<const byte> bytes, render::BufferUses usage) {
    if (bytes.empty()) {
        AddDiagnostic("UploadSize", "An upload must contain bytes");
        return {};
    }
    const auto value = CreateBuffer({bytes.size(), render::MemoryType::Upload, usage | render::BufferUse::MapWrite, {}}, name);
    const auto pass = AddPass(name, RgPassType::Upload, std::source_location::current());
    if (!value.IsValid() || !pass.IsValid()) return {};
    auto& data = _impl->Passes[pass.Index];
    data.UploadBytes.assign(bytes.begin(), bytes.end());
    data.Accesses.push_back({value.Index, {0, 1, 0, 1}, uint32_t(render::BufferState::HostWrite), false, true, true, value.Version, {0, bytes.size()}});
    return value;
}
RgOperationTicket RenderGraph::Track(RgPassHandle pass) {
    auto& impl = *_impl;
    if (!impl.Mutable() || pass.Generation != impl.Generation || pass.Index >= impl.Passes.size()) {
        impl.Error("OperationTicket", "Operation ticket requires a pass from this graph");
        return {};
    }
    auto& ticket = impl.Passes[pass.Index].Ticket;
    if (!ticket._state) ticket._state = make_shared<FrameSubmission>(impl.GenerationSerial);
    return ticket;
}
RgOperationTicket RenderGraph::ExportTexture(RgTextureValue value, render::TextureStates finalState, render::SubresourceRange range) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, true)) return {};
    const auto normalized = render::NormalizeSubresourceRange(impl.Resources[value.Index].TextureDesc, range);
    if (!normalized || !ValidTextureState(impl.Resources[value.Index].TextureDesc, finalState, false)) {
        impl.Error("ExportState", "Export requires a valid range and defined final state");
        return {};
    }
    const auto pass = AddPass("Export." + impl.Resources[value.Index].Name, RgPassType::Export, std::source_location::current());
    auto& data = impl.Passes[pass.Index];
    data.SideEffect = true;
    data.Accesses.push_back({value.Index, *normalized, finalState.value(), true, false, true, value.Version});
    return Track(pass);
}
RgOperationTicket RenderGraph::ExportBuffer(RgBufferValue value, RgBufferAccess finalAccess, render::BufferRange range) {
    auto& impl = *_impl;
    const auto pass = AddPass("Export.Buffer", RgPassType::Export, std::source_location::current());
    if (!pass.IsValid()) return {};
    if (!UseBuffer(pass.Index, value, finalAccess, false, false, render::ShaderStage::UNKNOWN, range).IsValid()) return {};
    impl.Passes[pass.Index].Accesses.back().Read = true;
    impl.Passes[pass.Index].SideEffect = true;
    return Track(pass);
}
RgReadbackTicket RenderGraph::ReadbackTexture(std::string_view name, RgTextureValue value, render::SubresourceRange range) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, true)) return {};
    const auto& desc = impl.Resources[value.Index].TextureDesc;
    const auto normalized = render::NormalizeSubresourceRange(desc, range);
    if (!normalized || normalized->ArrayLayerCount != 1 || normalized->MipLevelCount != 1) {
        impl.Error("ReadbackRange", "Texture readback requires one mip and layer");
        return {};
    }
    const uint32_t texelBytes = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (!texelBytes || render::IsDepthStencilFormat(desc.Format)) {
        impl.Error("ReadbackFormat", "Texture readback requires a color format");
        return {};
    }
    const auto pitch = Align(uint64_t{std::max(1u, desc.Width >> normalized->BaseMipLevel)} * texelBytes, std::max(1u, impl.Device.GetDetail().TextureDataPitchAlignment));
    const auto size = pitch * std::max(1u, desc.Height >> normalized->BaseMipLevel);
    auto target = CreateBuffer({size, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}}, name);
    RgReadbackTicket ticket;
    ticket._storage = make_shared<RgReadbackTicket::Storage>();
    ticket._storage->Size = size;
    impl.Resources[target.Index].Readback = ticket._storage;
    AddCopyTextureToBufferPass(name, value, target, *normalized);
    ticket._operation = ExportBuffer(target, RgBufferAccess::HostRead);
    return ticket;
}
RgReadbackTicket RenderGraph::ReadbackBuffer(std::string_view name, RgBufferValue value, render::BufferRange range) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, false)) return {};
    const auto size = impl.Resources[value.Index].BufferDesc.Size;
    if (range.Offset > size) {
        impl.Error("ReadbackRange", "Buffer readback offset exceeds size");
        return {};
    }
    if (range.Size == render::BufferRange::All()) range.Size = size - range.Offset;
    if (!range.Size || range.Size > size - range.Offset) {
        impl.Error("ReadbackRange", "Buffer readback range exceeds size");
        return {};
    }
    auto target = CreateBuffer({range.Size, render::MemoryType::ReadBack, render::BufferUse::CopyDestination | render::BufferUse::MapRead, {}}, name);
    RgReadbackTicket ticket;
    ticket._storage = make_shared<RgReadbackTicket::Storage>();
    ticket._storage->Size = range.Size;
    impl.Resources[target.Index].Readback = ticket._storage;
    AddCopyBufferPass(name, value, target, range.Size, range.Offset);
    ticket._operation = ExportBuffer(target, RgBufferAccess::HostRead);
    return ticket;
}

RgTexturePort RenderGraph::DeclareTexturePort(const render::TextureDescriptor& desc, std::string_view name) {
    const auto value = CreateTexture(desc, name);
    if (!value.IsValid()) return {};
    auto& resource = _impl->Resources[value.Index];
    resource.Port = true;
    resource.VersionParents.resize(1);
    return {value.Index, value.Generation};
}
RgBufferPort RenderGraph::DeclareBufferPort(const render::BufferDescriptor& desc, std::string_view name) {
    const auto value = CreateBuffer(desc, name);
    if (!value.IsValid()) return {};
    auto& resource = _impl->Resources[value.Index];
    resource.Port = true;
    resource.VersionParents.resize(1);
    return {value.Index, value.Generation};
}
RgTextureValue RenderGraph::Value(RgTexturePort port) const noexcept {
    const auto& impl = *_impl;
    if (port.Generation != impl.Generation || port.Index >= impl.Resources.size() || !impl.Resources[port.Index].Port || !impl.Resources[port.Index].IsTexture) return {};
    return {port.Index, port.Generation, 0};
}
RgBufferValue RenderGraph::Value(RgBufferPort port) const noexcept {
    const auto& impl = *_impl;
    if (port.Generation != impl.Generation || port.Index >= impl.Resources.size() || !impl.Resources[port.Index].Port || impl.Resources[port.Index].IsTexture) return {};
    return {port.Index, port.Generation, 0};
}
bool RenderGraph::Connect(RgTexturePort input, RgTextureValue output) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(input.Index, input.Generation, true) || !impl.Handle(output.Index, output.Generation, true)) return false;
    auto& port = impl.Resources[input.Index];
    const auto& source = impl.Resources[output.Index];
    if (!port.Port || port.Connection != InvalidIndex || output.Version >= source.VersionParents.size() || !(TexturePoolKey{port.TextureDesc} == TexturePoolKey{source.TextureDesc})) {
        impl.Error("PortConnection", "Texture port must have one producer with an identical descriptor", InvalidIndex, input.Index);
        return false;
    }
    port.Connection = output.Index;
    port.ConnectionVersion = output.Version;
    return true;
}
bool RenderGraph::Connect(RgBufferPort input, RgBufferValue output) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(input.Index, input.Generation, false) || !impl.Handle(output.Index, output.Generation, false)) return false;
    auto& port = impl.Resources[input.Index];
    const auto& source = impl.Resources[output.Index];
    if (!port.Port || port.Connection != InvalidIndex || output.Version >= source.VersionParents.size() || !(BufferPoolKey{port.BufferDesc} == BufferPoolKey{source.BufferDesc})) {
        impl.Error("PortConnection", "Buffer port must have one producer with an identical descriptor", InvalidIndex, input.Index);
        return false;
    }
    port.Connection = output.Index;
    port.ConnectionVersion = output.Version;
    return true;
}

bool RenderGraph::Impl::ResolvePorts() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::ResolvePorts");
    vector<vector<uint8_t>> visiting(Resources.size());
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        resource.ResolvedValues.assign(resource.VersionParents.size(), {InvalidIndex, InvalidIndex});
        visiting[r].assign(resource.VersionParents.size(), 0);
        if (resource.Port && resource.Connection == InvalidIndex) Error("UnconnectedPort", "Every declared input port requires one connection", InvalidIndex, r);
    }
    if (!Report.Diagnostics.empty()) return false;
    for (const auto& pass : Passes)
        for (const auto& access : pass.Accesses)
            if (access.Resource >= Resources.size() || access.Version >= Resources[access.Resource].VersionParents.size()) Error("InvalidVersion", "Pass access names an unreserved resource version");
    for (const auto& view : Views)
        if (view.Resource >= Resources.size() || view.Version >= Resources[view.Resource].VersionParents.size()) Error("InvalidVersion", "View names an unreserved resource version");
    if (!Report.Diagnostics.empty()) return false;
    const auto resolve = [&](auto&& self, uint32_t r, uint32_t v) -> std::pair<uint32_t, uint32_t> {
        if (r >= Resources.size() || v >= Resources[r].VersionParents.size()) {
            Error("InvalidVersion", "Port connects an unreserved version");
            return {InvalidIndex, InvalidIndex};
        }
        auto& resource = Resources[r];
        if (!resource.Port) return {r, v};
        auto& result = resource.ResolvedValues[v];
        if (result.first != InvalidIndex) return result;
        if (visiting[r][v]) {
            Error("PortCycle", "Resource port connections contain a cycle", InvalidIndex, r);
            return {InvalidIndex, InvalidIndex};
        }
        visiting[r][v] = 1;
        const auto parent = v == 0 ? self(self, resource.Connection, resource.ConnectionVersion) : self(self, r, resource.VersionParents[v]);
        if (parent.first != InvalidIndex) {
            if (v == 0)
                result = parent;
            else {
                auto& parents = Resources[parent.first].VersionParents;
                result = {parent.first, static_cast<uint32_t>(parents.size())};
                parents.push_back(parent.second);
            }
        }
        visiting[r][v] = 0;
        return result;
    };
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        const auto count = static_cast<uint32_t>(Resources[r].ResolvedValues.size());
        for (uint32_t v = 0; v < count; ++v) {
            const auto value = resolve(resolve, r, v);
            Resources[r].ResolvedValues[v] = value;
        }
    }
    if (!Report.Diagnostics.empty()) return false;
    const auto mapped = [&](uint32_t r, uint32_t v) { return Resources[r].Port ? Resources[r].ResolvedValues[v] : std::pair{r, v}; };
    for (auto& pass : Passes) {
        for (auto& access : pass.Accesses) {
            const auto value = mapped(access.Resource, access.Version);
            access.Resource = value.first;
            access.Version = value.second;
        }
        for (auto& r : pass.DeclaredBuffers) r = mapped(r, 0).first;
        if (pass.CopyOp) {
            pass.CopyOp->Source = mapped(pass.CopyOp->Source, 0).first;
            pass.CopyOp->Destination = mapped(pass.CopyOp->Destination, 0).first;
        }
    }
    for (auto& view : Views) {
        const auto value = mapped(view.Resource, view.Version);
        view.Resource = value.first;
        view.Version = value.second;
    }
    for (auto& arguments : IndirectArgumentsRecords) arguments.Resource = mapped(arguments.Resource, 0).first;
    for (auto& set : ParameterSets)
        for (auto& binding : set.Bindings) {
            if (auto* buffer = std::get_if<BufferParameter>(&binding.Value)) buffer->Resource = mapped(buffer->Resource, 0).first;
            if (auto* texture = std::get_if<TextureParameter>(&binding.Value)) texture->Resource = mapped(texture->Resource, 0).first;
        }
    return true;
}

RgTextureValue RenderGraph::NextVersion(RgTextureValue value) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, true)) return {};
    auto& parents = impl.Resources[value.Index].VersionParents;
    if (value.Version >= parents.size()) {
        impl.Error("InvalidVersion", "Texture version is out of range");
        return {};
    }
    if (value.Version + 1 != parents.size()) {
        impl.Error("VersionBranch", "In-place versions form one storage chain; copy to another resource to branch contents");
        return {};
    }
    parents.push_back(value.Version);
    value.Version = static_cast<uint32_t>(parents.size() - 1);
    return value;
}
RgBufferValue RenderGraph::NextVersion(RgBufferValue value) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, false)) return {};
    auto& parents = impl.Resources[value.Index].VersionParents;
    if (value.Version >= parents.size()) {
        impl.Error("InvalidVersion", "Buffer version is out of range");
        return {};
    }
    if (value.Version + 1 != parents.size()) {
        impl.Error("VersionBranch", "In-place versions form one storage chain; copy to another resource to branch contents");
        return {};
    }
    parents.push_back(value.Version);
    value.Version = static_cast<uint32_t>(parents.size() - 1);
    return value;
}

RgPassHandle RenderGraph::AddPass(std::string_view name, RgPassType type, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    const auto index = static_cast<uint32_t>(impl.Passes.size());
    Impl::Pass pass{};
    pass.Location = location;
    impl.Passes.push_back(std::move(pass));
    impl.Report.Passes.push_back({string{name}, string{location.file_name()}, location.line(), type});
    return {index, impl.Generation};
}
void RenderGraph::SetPayload(RgPassHandle pass, unique_ptr<Payload> payload) { _impl->Passes[pass.Index].Data = std::move(payload); }

RgTextureViewHandle RenderGraph::UseTexture(uint32_t pass, RgTextureValue texture, RgTextureViewDesc view,
                                            render::TextureViewUsage usage, bool read, bool write, bool validAfter,
                                            render::ShaderStages uavWriteStages) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(texture.Index, texture.Generation, true, pass)) return {};
    const auto& desc = impl.Resources[texture.Index].TextureDesc;
    if (!view.Range.Aspects && usage == render::TextureViewUsage::Resource && render::IsDepthStencilFormat(desc.Format)) view.Range.Aspects = render::TextureAspect::Depth;
    auto range = render::NormalizeSubresourceRange(desc, view.Range);
    if (!range) {
        impl.Error("InvalidRange", "Texture view range is empty or outside the resource", pass, texture.Index);
        return {};
    }
    if (view.Format == render::TextureFormat::UNKNOWN) view.Format = desc.Format;
    if (view.Dimension == render::TextureDimension::UNKNOWN) view.Dimension = desc.Dim;
    if (view.Format != desc.Format || !EnumContains(view.Dimension) || !desc.Usage.HasFlag(UsageFor(usage))) {
        impl.Error("InvalidView", "Texture view format, dimension or usage is incompatible with its resource", pass, texture.Index);
        return {};
    }
    const bool attachment = usage == render::TextureViewUsage::RenderTarget || usage == render::TextureViewUsage::DepthRead || usage == render::TextureViewUsage::DepthWrite;
    const bool dimensionMatches = view.Dimension == desc.Dim ||
                                  (view.Dimension == render::TextureDimension::Dim2D && desc.Dim == render::TextureDimension::Dim2DArray && range->ArrayLayerCount == 1) ||
                                  (view.Dimension == render::TextureDimension::Dim2DArray && (desc.Dim == render::TextureDimension::Cube || desc.Dim == render::TextureDimension::CubeArray));
    if (!dimensionMatches || (attachment && (range->MipLevelCount != 1 || (view.Dimension != render::TextureDimension::Dim2D && view.Dimension != render::TextureDimension::Dim2DArray))) ||
        (usage == render::TextureViewUsage::UnorderedAccess && range->MipLevelCount != 1)) {
        impl.Error("InvalidView", "View dimensions or mip count are unsupported for this access", pass, texture.Index);
        return {};
    }
    if ((attachment && range->Aspects != render::GetTextureFormatAspects(desc.Format)) ||
        (usage == render::TextureViewUsage::Resource && range->Aspects.HasFlag(render::TextureAspect::Depth) && range->Aspects.HasFlag(render::TextureAspect::Stencil))) {
        impl.Error("InvalidAspect", "Attachments require all format aspects; sampled views require one aspect", pass, texture.Index);
        return {};
    }
    const TextureViewKey key{view.Dimension, view.Format, *range, usage};
    uint32_t index = 0;
    for (; index < impl.Views.size(); ++index)
        if (impl.Views[index].Resource == texture.Index && impl.Views[index].Version == texture.Version && impl.Views[index].Key == key) break;
    if (index == impl.Views.size()) impl.Views.push_back({texture.Index, key, nullptr, texture.Version});
    impl.Passes[pass].Accesses.push_back({texture.Index, *range, StateFor(usage), read, write, validAfter, texture.Version});
    impl.Passes[pass].Accesses.back().Stages = view.Stages ? view.Stages : uavWriteStages ? uavWriteStages
                                                                                          : render::ShaderStages{impl.Report.Passes[pass].Type == RgPassType::Compute ? render::ShaderStage::Compute : render::ShaderStage::Graphics};
    AddUnique(impl.Passes[pass].DeclaredViews, index);
    if (write && usage == render::TextureViewUsage::UnorderedAccess &&
        impl.Report.Passes[pass].Type == RgPassType::Raster) {
        auto& rasterPass = impl.Passes[pass];
        rasterPass.AllowUavWrites = true;
        rasterPass.UavWriteStages |= uavWriteStages ? uavWriteStages : render::ShaderStages{render::ShaderStage::Graphics};
    }
    return {index, impl.Generation};
}
RgBufferValue RenderGraph::UseBuffer(uint32_t pass, RgBufferValue buffer, RgBufferAccess access, bool read, bool write,
                                     render::ShaderStages uavWriteStages, render::BufferRange range) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(buffer.Index, buffer.Generation, false, pass)) return {};
    const auto [state, usage] = BufferAccessInfo(access);
    const auto& desc = impl.Resources[buffer.Index].BufferDesc;
    if (usage == render::BufferUse::UNKNOWN || !desc.Usage.HasFlag(usage) ||
        (write && access != RgBufferAccess::UnorderedAccess && access != RgBufferAccess::CopyDestination) ||
        (read && access == RgBufferAccess::CopyDestination)) {
        impl.Error("InvalidBufferAccess", "Buffer access is incompatible with its usage or read/write mode", pass, buffer.Index);
        return {};
    }
    impl.Passes[pass].Accesses.push_back({buffer.Index, {0, 1, 0, 1}, static_cast<uint32_t>(state), read, write, true, buffer.Version, range});
    impl.Passes[pass].Accesses.back().Stages = uavWriteStages ? uavWriteStages : render::ShaderStages{impl.Report.Passes[pass].Type == RgPassType::Compute ? render::ShaderStage::Compute : render::ShaderStage::Graphics};
    AddUnique(impl.Passes[pass].DeclaredBuffers, buffer.Index);
    if (write && access == RgBufferAccess::UnorderedAccess &&
        impl.Report.Passes[pass].Type == RgPassType::Raster) {
        auto& rasterPass = impl.Passes[pass];
        rasterPass.AllowUavWrites = true;
        rasterPass.UavWriteStages |= uavWriteStages ? uavWriteStages : render::ShaderStages{render::ShaderStage::Graphics};
    }
    return buffer;
}
RgTextureViewHandle RenderGraphPassBuilder::ReadTexture(RgTextureValue texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::Resource, true, false, true); }
RgBufferValue RenderGraphPassBuilder::ReadImmutableBuffer(render::Buffer& buffer, render::BufferStates state, RgBufferAccess access, render::BufferRange range, shared_ptr<void> owner) {
    auto& impl = *_graph._impl;
    if (!impl.Mutable()) return {};
    const auto required = BufferAccessInfo(access).first;
    const uint32_t writes = uint32_t(render::BufferState::UnorderedAccess) | uint32_t(render::BufferState::CopyDestination) | uint32_t(render::BufferState::Undefined);
    if (!state || (state.value() & writes) || (buffer.GetDesc().Memory == render::MemoryType::Device && !state.HasFlag(required))) {
        impl.Error("ImmutableAssetState", "Immutable geometry requires a compatible persistent read state", _pass);
        return {};
    }
    for (uint32_t i = 0; i < impl.Resources.size(); ++i) {
        const auto& resource = impl.Resources[i];
        if (!resource.ExternalBuffer || resource.ExternalBuffer->Buffer != &buffer) continue;
        if (!resource.Immutable) {
            for (const auto& declared : impl.Passes[_pass].Accesses)
                if (declared.Resource == i && declared.Read && (declared.State & uint32_t(required)) != 0) return {i, impl.Generation, declared.Version};
            impl.Error("ImmutableAssetIdentity", "Graph geometry requires an explicit read of its produced value", _pass, i);
            return {};
        }
        _graph.Retain(std::move(owner));
        return ReadBuffer({i, impl.Generation, 0}, access, range);
    }
    auto external = make_shared<RenderExternalBuffer>(RenderExternalBuffer{&buffer, buffer.GetDesc(), state, true, false, std::move(owner)});
    const auto value = _graph.ImportBuffer(*external, "Immutable.Geometry", RenderGraphExternalAccess::ReadOnly);
    if (!value.IsValid()) return {};
    impl.Resources[value.Index].Immutable = true;
    _graph.Retain(external);
    return ReadBuffer(value, access, range);
}
RgBufferValue RenderGraphPassBuilder::ReadBuffer(RgBufferValue buffer, RgBufferAccess access, render::BufferRange range) { return _graph.UseBuffer(_pass, buffer, access, true, false, {}, range); }
RgBufferValue RenderGraphPassBuilder::WriteBuffer(RgBufferValue buffer, RgBufferAccess access, render::BufferRange range) { return _graph.UseBuffer(_pass, buffer, access, false, true, {}, range); }
RgBufferValue RenderGraphPassBuilder::ReadWriteBuffer(RgBufferValue buffer, RgBufferAccess access, render::BufferRange range) { return _graph.UseBuffer(_pass, buffer, access, true, true, {}, range); }
RgIndirectArgumentsHandle RenderGraphPassBuilder::ReadIndirectArguments(
    RgBufferValue buffer, RgIndirectCommand command, uint64_t offset, uint32_t count) {
    return _graph.AddIndirectArguments(_pass, buffer, command, offset, count);
}
RgParameterSetHandle RenderGraphPassBuilder::CreateParameterSet(
    ShaderProgram& program, uint32_t group, std::span<const RgParameterBinding> bindings) {
    return _graph.AddParameterSet(_pass, program, group, bindings);
}
void RenderGraphPassBuilder::SetSideEffect() { _graph._impl->Passes[_pass].SideEffect = true; }
RgTextureViewHandle RenderGraphComputeBuilder::WriteTexture(RgTextureValue texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess, false, true, true); }
RgTextureViewHandle RenderGraphComputeBuilder::ReadWriteTexture(RgTextureValue texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess, true, true, true); }
RgComputeProgramHandle RenderGraphComputeBuilder::UseComputeProgram(ShaderProgram& program) { return _graph.AddComputeProgram(_pass, program); }
RgTextureViewHandle RenderGraphRasterBuilder::WriteTexture(
    RgTextureValue texture, render::ShaderStages stages, const RgTextureViewDesc& view) {
    return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess,
                             false, true, true, stages);
}
RgTextureViewHandle RenderGraphRasterBuilder::ReadWriteTexture(
    RgTextureValue texture, render::ShaderStages stages, const RgTextureViewDesc& view) {
    return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess,
                             true, true, true, stages);
}

RgIndirectArgumentsHandle RenderGraph::AddIndirectArguments(
    uint32_t pass, RgBufferValue buffer, RgIndirectCommand command,
    uint64_t offset, uint32_t count) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(buffer.Index, buffer.Generation, false, pass)) return {};
    if (!EnumContains(command)) {
        impl.Error("IndirectCommand", "Indirect command kind is invalid", pass, buffer.Index);
        return {};
    }
    const bool dispatch = command == RgIndirectCommand::Dispatch;
    const bool supported = dispatch ? impl.Device.GetCapabilities().Features.IndirectDispatch
                                    : impl.Device.GetCapabilities().Features.IndirectDraw;
    const uint64_t stride = command == RgIndirectCommand::Draw
                                ? sizeof(render::DrawIndirectArguments)
                            : command == RgIndirectCommand::DrawIndexed
                                ? sizeof(render::DrawIndexedIndirectArguments)
                                : sizeof(render::DispatchIndirectArguments);
    const uint64_t size = impl.Resources[buffer.Index].BufferDesc.Size;
    const bool countValid = !dispatch || count <= 1;
    const bool multiplicationValid = count == 0 || stride <= std::numeric_limits<uint64_t>::max() / count;
    const uint64_t required = multiplicationValid ? stride * count : std::numeric_limits<uint64_t>::max();
    if (!supported || !countValid || offset % 4 != 0 || offset > size ||
        required > size - offset) {
        impl.Error("IndirectArguments", "Indirect usage, capability, alignment, count or argument range is invalid", pass, buffer.Index);
        return {};
    }
    if (!UseBuffer(pass, buffer, RgBufferAccess::Indirect, true, false).IsValid()) return {};
    const uint32_t index = static_cast<uint32_t>(impl.IndirectArgumentsRecords.size());
    impl.IndirectArgumentsRecords.push_back({pass, buffer.Index, command, offset, count});
    return {index, impl.Generation};
}

RgComputeProgramHandle RenderGraph::AddComputeProgram(uint32_t pass, ShaderProgram& program) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    if (program.GetDevice() != &impl.Device ||
        !ProgramStages(program).HasFlag(render::ShaderStage::Compute) ||
        ProgramStages(program).HasFlag(render::ShaderStage::Graphics)) {
        impl.Error("ComputeProgram", "Compute passes require a compute-only ShaderProgram from this graph's device", pass);
        return {};
    }
    const uint32_t index = static_cast<uint32_t>(impl.ComputePrograms.size());
    impl.ComputePrograms.push_back({pass, &program, nullptr});
    return {index, impl.Generation};
}

RgGraphicsProgramHandle RenderGraphRasterBuilder::UseGraphicsProgram(ShaderProgram& program, const MaterialPipelineState& state,
                                                                     const PrimitiveVertexLayout& layout, PrimitiveTopology topology) {
    return _graph.AddGraphicsProgram(_pass, program, state, layout, topology);
}

RgGraphicsProgramHandle RenderGraph::AddGraphicsProgram(uint32_t pass, ShaderProgram& program, const MaterialPipelineState& state,
                                                        const PrimitiveVertexLayout& layout, PrimitiveTopology topology) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    ++impl.Report.GraphicsPipelineRequests;
    if (program.GetDevice() != &impl.Device || !ProgramStages(program).HasFlag(render::ShaderStage::Vertex) ||
        ProgramStages(program).HasFlag(render::ShaderStage::Compute)) {
        impl.Error("GraphicsProgram", "Raster passes require a graphics ShaderProgram from this graph's device", pass);
        return {};
    }
    auto& entries = impl.GraphicsProgramIndices[&program];
    for (const auto entry : entries) {
        const auto& value = impl.GraphicsPrograms[entry];
        if (value.Pass == pass && value.State == state && value.Layout == layout && value.Topology == topology)
            return {entry, impl.Generation};
    }
    const auto index = static_cast<uint32_t>(impl.GraphicsPrograms.size());
    impl.GraphicsPrograms.push_back({pass, &program, state, layout, topology});
    entries.push_back(index);
    return {index, impl.Generation};
}

RgParameterSetHandle RenderGraph::AddParameterSet(
    uint32_t pass, ShaderProgram& program, uint32_t group,
    std::span<const RgParameterBinding> bindings) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    if (pass >= impl.Passes.size() || program.GetDevice() != &impl.Device) {
        impl.Error("ParameterProgram", "Parameter sets require a ShaderProgram from this graph's device", pass);
        return {};
    }

    const RgPassType passType = impl.Report.Passes[pass].Type;
    const render::ShaderStages programStages = ProgramStages(program);
    const bool stageCompatible =
        (passType == RgPassType::Raster && programStages.HasFlag(render::ShaderStage::Graphics) &&
         !programStages.HasFlag(render::ShaderStage::Compute)) ||
        (passType == RgPassType::Compute && programStages.HasFlag(render::ShaderStage::Compute) &&
         !programStages.HasFlag(render::ShaderStage::Graphics));
    if (!stageCompatible) {
        impl.Error("ParameterProgram", "The parameter program's shader stages do not match the pass type", pass);
        return {};
    }

    Impl::ParameterSet parameterSet{
        .Pass = pass,
        .Program = &program,
        .Group = group,
        .Bindings = {},
        .Native = nullptr,
        .DynamicOffsets = {}};
    const auto fail = [&](std::string_view code, std::string_view message,
                          std::string_view declaration, uint32_t resource = InvalidIndex) {
        impl.Error(code, message, pass, resource, declaration);
    };
    const auto findCBuffer = [&](std::string_view declaration) -> const ShaderParameterBufferLayout* {
        for (const ShaderParameterBufferLayout& buffer : program.GetParameterLayout().Buffers()) {
            if (buffer.Name == declaration) return &buffer;
        }
        return nullptr;
    };
    const auto normalizedBufferRange = [&](const RgBufferParameterBinding& value,
                                           std::string_view declaration)
        -> std::optional<render::BufferRange> {
        if (!impl.Handle(value.Buffer.Index, value.Buffer.Generation, false, pass)) return std::nullopt;
        const uint64_t bufferSize = impl.Resources[value.Buffer.Index].BufferDesc.Size;
        if (value.Range.Offset > bufferSize) {
            fail("ParameterBufferRange", "Buffer parameter offset is outside the resource", declaration,
                 value.Buffer.Index);
            return std::nullopt;
        }
        const uint64_t available = bufferSize - value.Range.Offset;
        const uint64_t size = value.Range.Size == render::BufferRange::All()
                                  ? available
                                  : value.Range.Size;
        if (size == 0 || size > available) {
            fail("ParameterBufferRange", "Buffer parameter range is empty or outside the resource", declaration,
                 value.Buffer.Index);
            return std::nullopt;
        }
        return render::BufferRange{value.Range.Offset, size};
    };

    bool groupKnown = false;

    for (const RgParameterBinding& source : bindings) {
        const string declaration{source.Declaration};
        const std::optional<render::ShaderBindingInfo> info =
            program.GetArtifact().FindBindingInfo(declaration);
        if (declaration.empty() || !info.has_value()) {
            fail("ParameterDeclaration", "Binding is not a canonical descriptor declaration in this program",
                 declaration);
            return {};
        }
        if (info->Group != group) {
            fail("ParameterGroup", "Binding belongs to a different parameter group", declaration);
            return {};
        }
        groupKnown = true;
        if (info->Immutable) {
            fail("ImmutableBinding", "Static or immutable samplers must not be supplied by the caller", declaration);
            return {};
        }
        if (source.ArrayElement >= info->Count) {
            fail("ParameterArrayElement", "Binding array element is outside the declaration count", declaration);
            return {};
        }
        if (std::find_if(parameterSet.Bindings.begin(), parameterSet.Bindings.end(),
                         [&](const Impl::ParameterBinding& value) {
                             return value.Declaration == declaration &&
                                    value.ArrayElement == source.ArrayElement;
                         }) != parameterSet.Bindings.end()) {
            fail("DuplicateParameterBinding", "The same binding array element was supplied more than once", declaration);
            return {};
        }

        Impl::ParameterBinding destination{
            .Declaration = declaration,
            .ArrayElement = source.ArrayElement,
            .Info = *info,
            .Value = Impl::CBufferBytes{}};
        const shader::ShaderBindingKind kind = info->LogicalKind;
        if (const auto* bytes = std::get_if<RgCBufferParameterBinding>(&source.Value)) {
            const ShaderParameterBufferLayout* layout = findCBuffer(declaration);
            if (kind != shader::ShaderBindingKind::CBuffer || source.ArrayElement != 0 || layout == nullptr ||
                layout->Group != group || bytes->Bytes.size() != layout->Size) {
                fail("ParameterType", "Copied cbuffer bytes must exactly match a scalar cbuffer declaration", declaration);
                return {};
            }
            destination.Value = Impl::CBufferBytes{{bytes->Bytes.begin(), bytes->Bytes.end()}};
        } else if (const auto* texture = std::get_if<RgTextureParameterBinding>(&source.Value)) {
            if (!shader::IsImageKind(kind) || !EnumContains(texture->Access) ||
                (!shader::IsWritableKind(kind) && texture->Access != RgParameterAccess::Read)) {
                fail("ParameterType", "Texture value or access does not match the shader declaration", declaration,
                     texture->Texture.Index);
                return {};
            }
            const bool writable = shader::IsWritableKind(kind);
            const bool read = writable ? IsReadAccess(texture->Access) : true;
            const bool write = writable && IsWriteAccess(texture->Access);
            const render::TextureViewUsage usage = writable
                                                       ? render::TextureViewUsage::UnorderedAccess
                                                       : render::TextureViewUsage::Resource;
            const RgTextureViewHandle view = UseTexture(
                pass, texture->Texture, texture->View, usage, read, write, true, info->Stages);
            if (!view.IsValid()) return {};
            destination.Value = Impl::TextureParameter{view.Index, texture->Texture.Index};
        } else if (const auto* buffer = std::get_if<RgBufferParameterBinding>(&source.Value)) {
            const bool bufferKind = kind == shader::ShaderBindingKind::CBuffer ||
                                    kind == shader::ShaderBindingKind::TypedBuffer ||
                                    kind == shader::ShaderBindingKind::RWTypedBuffer ||
                                    kind == shader::ShaderBindingKind::StructuredBuffer ||
                                    kind == shader::ShaderBindingKind::RWStructuredBuffer ||
                                    kind == shader::ShaderBindingKind::RawBuffer ||
                                    kind == shader::ShaderBindingKind::RWRawBuffer;
            const bool writable = shader::IsWritableKind(kind);
            if (!bufferKind || !EnumContains(buffer->Access) ||
                (!writable && buffer->Access != RgParameterAccess::Read)) {
                fail("ParameterType", "Buffer value or access does not match the shader declaration", declaration,
                     buffer->Buffer.Index);
                return {};
            }
            const std::optional<render::BufferRange> range = normalizedBufferRange(*buffer, declaration);
            if (!range.has_value()) return {};

            bool representationValid = buffer->Format == render::TextureFormat::UNKNOWN;
            if (kind == shader::ShaderBindingKind::CBuffer) {
                const ShaderParameterBufferLayout* layout = findCBuffer(declaration);
                const uint64_t alignment = std::max<uint64_t>(1, impl.Device.GetCapabilities().Limits.CBufferOffsetAlignment);
                representationValid = representationValid && buffer->StructureByteStride == 0 &&
                                      layout != nullptr && layout->Group == group && range->Size == layout->Size &&
                                      range->Offset % alignment == 0;
            } else if (kind == shader::ShaderBindingKind::StructuredBuffer ||
                       kind == shader::ShaderBindingKind::RWStructuredBuffer) {
                const uint64_t storageAlignment = std::max<uint64_t>(
                    1, impl.Device.GetCapabilities().Limits.StorageBufferOffsetAlignment);
                representationValid = representationValid && buffer->StructureByteStride != 0 &&
                                      buffer->StructureByteStride % 4 == 0 && buffer->StructureByteStride <= 2048 &&
                                      range->Offset % buffer->StructureByteStride == 0 &&
                                      range->Size % buffer->StructureByteStride == 0 &&
                                      range->Offset % storageAlignment == 0;
            } else if (kind == shader::ShaderBindingKind::RawBuffer ||
                       kind == shader::ShaderBindingKind::RWRawBuffer) {
                const uint64_t storageAlignment = std::max<uint64_t>(
                    1, impl.Device.GetCapabilities().Limits.StorageBufferOffsetAlignment);
                representationValid = representationValid && buffer->StructureByteStride == 0 &&
                                      range->Offset % 4 == 0 && range->Size % 4 == 0 &&
                                      range->Offset % storageAlignment == 0;
            } else {
                const uint32_t elementSize = render::GetTextureFormatBytesPerPixel(buffer->Format);
                representationValid = buffer->StructureByteStride == 0 && elementSize != 0 &&
                                      range->Offset % elementSize == 0 && range->Size % elementSize == 0;
            }
            if (!representationValid) {
                fail("ParameterBufferLayout", "Buffer range, stride or format is incompatible with the declaration",
                     declaration, buffer->Buffer.Index);
                return {};
            }

            const bool read = writable ? IsReadAccess(buffer->Access) : true;
            const bool write = writable && IsWriteAccess(buffer->Access);
            const RgBufferAccess graphAccess = kind == shader::ShaderBindingKind::CBuffer
                                                   ? RgBufferAccess::Constant
                                               : writable
                                                   ? RgBufferAccess::UnorderedAccess
                                                   : RgBufferAccess::ShaderRead;
            if (!UseBuffer(pass, buffer->Buffer, graphAccess, read, write, info->Stages, *range).IsValid()) return {};
            destination.Value = Impl::BufferParameter{
                buffer->Buffer.Index, *range, buffer->StructureByteStride, buffer->Format};
        } else if (const auto* sampler = std::get_if<RgSamplerParameterBinding>(&source.Value)) {
            if (kind != shader::ShaderBindingKind::Sampler) {
                fail("ParameterType", "Sampler value does not match the shader declaration", declaration);
                return {};
            }
            destination.Value = Impl::SamplerParameter{sampler->Sampler};
        }
        parameterSet.Bindings.push_back(std::move(destination));
    }

    const shader::ShaderArtifactView& artifact = program.GetArtifact().Generic();
    for (const shader::WireBindingRecord& binding : artifact.Bindings()) {
        const std::optional<std::string_view> name = artifact.GetName(binding.Name);
        if (!name.has_value()) continue;
        const std::optional<render::ShaderBindingInfo> info =
            program.GetArtifact().FindBindingInfo(name.value());
        if (!info.has_value() || info->Group != group) continue;
        groupKnown = true;
        if (info->Immutable) continue;
        for (uint32_t element = 0; element < info->Count; ++element) {
            const bool found = std::any_of(
                parameterSet.Bindings.begin(), parameterSet.Bindings.end(),
                [&](const Impl::ParameterBinding& value) {
                    return value.Declaration == name.value() && value.ArrayElement == element;
                });
            if (!found) {
                fail("MissingParameterBinding", fmt::format("Required binding array element {} is missing", element), name.value());
                return {};
            }
        }
    }
    if (!groupKnown) {
        fail("ParameterGroup", "The program has no descriptor declarations in this parameter group", {});
        return {};
    }

    const uint32_t index = static_cast<uint32_t>(impl.ParameterSets.size());
    impl.ParameterSets.push_back(std::move(parameterSet));
    return {index, impl.Generation};
}
RgTextureViewHandle RenderGraphRasterBuilder::SetColorAttachment(uint32_t slot, RgTextureValue texture, const RgColorAttachmentDesc& desc) {
    auto& impl = *_graph._impl;
    if (slot >= 8 || (slot < impl.Passes[_pass].Colors.size() && impl.Passes[_pass].Colors[slot])) {
        impl.Error("InvalidAttachmentSlot", "Color slots must be unique and less than 8", _pass);
        return {};
    }
    auto result = _graph.UseTexture(_pass, texture, desc.View, render::TextureViewUsage::RenderTarget, desc.Load == render::LoadAction::Load, true, desc.Store == render::StoreAction::Store);
    if (result.IsValid()) {
        auto& colors = impl.Passes[_pass].Colors;
        if (slot >= colors.size()) colors.resize(slot + 1);
        colors[slot] = RenderGraph::Impl::Color{result.Index, desc};
    }
    return result;
}
RgTextureViewHandle RenderGraphRasterBuilder::SetDepthAttachment(RgTextureValue texture, const RgDepthAttachmentDesc& desc) {
    auto& impl = *_graph._impl;
    if (impl.Passes[_pass].DepthAttachment || (desc.ReadOnly && (desc.Load != render::LoadAction::Load || desc.Store != render::StoreAction::Store))) {
        impl.Error("InvalidDepthAttachment", "Depth is already bound, or read-only depth would clear/discard", _pass);
        return {};
    }
    auto result = _graph.UseTexture(_pass, texture, desc.View, desc.ReadOnly ? render::TextureViewUsage::DepthRead : render::TextureViewUsage::DepthWrite,
                                    desc.Load == render::LoadAction::Load, !desc.ReadOnly, desc.Store == render::StoreAction::Store);
    if (result.IsValid()) impl.Passes[_pass].DepthAttachment = RenderGraph::Impl::Depth{result.Index, desc};
    return result;
}

RgPassHandle RenderGraph::AddCopyBufferPass(std::string_view name, RgBufferValue source, RgBufferValue destination,
                                            uint64_t size, uint64_t sourceOffset, uint64_t destinationOffset, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    if (!pass.IsValid()) return pass;
    if (!UseBuffer(pass.Index, source, RgBufferAccess::CopySource, true, false).IsValid() ||
        !UseBuffer(pass.Index, destination, RgBufferAccess::CopyDestination, false, true).IsValid()) return pass;
    const auto sourceSize = _impl->Resources[source.Index].BufferDesc.Size;
    const auto destinationSize = _impl->Resources[destination.Index].BufferDesc.Size;
    if (size == 0 || sourceOffset > sourceSize || size > sourceSize - sourceOffset || destinationOffset > destinationSize || size > destinationSize - destinationOffset) {
        _impl->Error("CopyRange", "Buffer copy range exceeds its source or destination", pass.Index);
        return pass;
    }
    _impl->Passes[pass.Index].Accesses[0].Bytes = {sourceOffset, size};
    _impl->Passes[pass.Index].Accesses[1].Bytes = {destinationOffset, size};
    _impl->Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::Buffer, source.Index, destination.Index, size, sourceOffset, destinationOffset};
    return pass;
}
RgPassHandle RenderGraph::AddCopyTexturePass(std::string_view name, RgTextureValue source, RgTextureValue destination,
                                             render::SubresourceRange sourceRange, render::SubresourceRange destinationRange, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) || !impl.Handle(destination.Index, destination.Generation, true, pass.Index)) return pass;
    const auto& src = impl.Resources[source.Index].TextureDesc;
    const auto& dst = impl.Resources[destination.Index].TextureDesc;
    const auto sr = render::NormalizeSubresourceRange(src, sourceRange);
    const auto dr = render::NormalizeSubresourceRange(dst, destinationRange);
    if (!sr || !dr || sr->MipLevelCount != 1 || dr->MipLevelCount != 1 || sr->ArrayLayerCount != dr->ArrayLayerCount ||
        src.Format != dst.Format || render::IsDepthStencilFormat(src.Format) || src.SampleCount != 1 || dst.SampleCount != 1 || src.Dim == render::TextureDimension::Dim3D || dst.Dim == render::TextureDimension::Dim3D ||
        std::max(1u, src.Width >> sr->BaseMipLevel) != std::max(1u, dst.Width >> dr->BaseMipLevel) ||
        std::max(1u, src.Height >> sr->BaseMipLevel) != std::max(1u, dst.Height >> dr->BaseMipLevel) ||
        !src.Usage.HasFlag(render::TextureUse::CopySource) || !dst.Usage.HasFlag(render::TextureUse::CopyDestination)) {
        impl.Error("CopyTextureDescriptor", "Texture copy needs matching color format, mip extents, layer counts and copy usages", pass.Index);
        return pass;
    }
    impl.Passes[pass.Index].Accesses.push_back({source.Index, *sr, static_cast<uint32_t>(render::TextureState::CopySource), true, false, true, source.Version});
    impl.Passes[pass.Index].Accesses.push_back({destination.Index, *dr, static_cast<uint32_t>(render::TextureState::CopyDestination), false, true, true, destination.Version});
    impl.Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::Texture, source.Index, destination.Index, 0, 0, 0, *sr, *dr};
    return pass;
}
RgPassHandle RenderGraph::AddResolveTexturePass(
    std::string_view name, RgTextureValue source, RgTextureValue destination,
    render::SubresourceRange sourceRange, render::SubresourceRange destinationRange,
    std::source_location location) {
    auto pass = AddPass(name, RgPassType::Resolve, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) ||
        !impl.Handle(destination.Index, destination.Generation, true, pass.Index)) return pass;
    const auto& src = impl.Resources[source.Index].TextureDesc;
    const auto& dst = impl.Resources[destination.Index].TextureDesc;
    const auto sr = render::NormalizeSubresourceRange(src, sourceRange);
    const auto dr = render::NormalizeSubresourceRange(dst, destinationRange);
    const bool sourceDimension = src.Dim == render::TextureDimension::Dim2D || src.Dim == render::TextureDimension::Dim2DArray;
    const bool destinationDimension = dst.Dim == render::TextureDimension::Dim2D || dst.Dim == render::TextureDimension::Dim2DArray;
    if (!sr || !dr || sr->MipLevelCount != 1 || dr->MipLevelCount != 1 ||
        sr->ArrayLayerCount != dr->ArrayLayerCount || src.Format != dst.Format ||
        render::IsDepthStencilFormat(src.Format) ||
        src.SampleCount <= 1 || dst.SampleCount != 1 || !sourceDimension || !destinationDimension ||
        std::max(1u, src.Width >> sr->BaseMipLevel) != std::max(1u, dst.Width >> dr->BaseMipLevel) ||
        std::max(1u, src.Height >> sr->BaseMipLevel) != std::max(1u, dst.Height >> dr->BaseMipLevel) ||
        !src.Usage.HasFlag(render::TextureUse::CopySource) ||
        !dst.Usage.HasFlag(render::TextureUse::CopyDestination)) {
        impl.Error("ResolveTextureDescriptor", "Resolve needs matching 2D color subresources, an MSAA source, a single-sample destination and copy usages", pass.Index);
        return pass;
    }
    impl.Passes[pass.Index].Accesses.push_back({source.Index, *sr, static_cast<uint32_t>(render::TextureState::ResolveSource), true, false, true, source.Version});
    impl.Passes[pass.Index].Accesses.push_back({destination.Index, *dr, static_cast<uint32_t>(render::TextureState::ResolveDestination), false, true, true, destination.Version});
    impl.Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::Resolve, source.Index, destination.Index, 0, 0, 0, *sr, *dr};
    return pass;
}
RgPassHandle RenderGraph::AddCopyTextureToBufferPass(std::string_view name, RgTextureValue source, RgBufferValue destination,
                                                     render::SubresourceRange range, uint64_t destinationOffset, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) ||
        !UseBuffer(pass.Index, destination, RgBufferAccess::CopyDestination, false, true).IsValid()) return pass;
    const auto& src = impl.Resources[source.Index].TextureDesc;
    auto normalized = render::NormalizeSubresourceRange(src, range);
    const auto& detail = impl.Device.GetDetail();
    const uint32_t texelBytes = render::GetTextureFormatBytesPerPixel(src.Format);
    if (!normalized || normalized->MipLevelCount != 1 || normalized->ArrayLayerCount != 1 || src.Dim == render::TextureDimension::Dim3D ||
        src.SampleCount != 1 || render::IsDepthStencilFormat(src.Format) || !src.Usage.HasFlag(render::TextureUse::CopySource) || texelBytes == 0 ||
        destinationOffset % detail.TextureDataPlacementAlignment != 0 || destinationOffset % texelBytes != 0) {
        impl.Error("CopyTextureRange", "Texture readback requires one non-MSAA 2D color subresource and aligned destination", pass.Index);
        return pass;
    }
    const uint64_t row = Align(uint64_t{std::max(1u, src.Width >> normalized->BaseMipLevel)} * render::GetTextureFormatBytesPerPixel(src.Format), detail.TextureDataPitchAlignment);
    const uint64_t size = row * std::max(1u, src.Height >> normalized->BaseMipLevel);
    const auto destinationSize = impl.Resources[destination.Index].BufferDesc.Size;
    if (destinationOffset > destinationSize || size > destinationSize - destinationOffset) {
        impl.Error("CopyRange", "Readback buffer is too small for the aligned texture footprint", pass.Index);
        return pass;
    }
    impl.Passes[pass.Index].Accesses.back().Bytes = {destinationOffset, size};
    impl.Passes[pass.Index].Accesses.push_back({source.Index, *normalized, static_cast<uint32_t>(render::TextureState::CopySource), true, false, true, source.Version});
    impl.Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::TextureToBuffer, source.Index, destination.Index, size, 0, destinationOffset, *normalized};
    return pass;
}

RgPassHandle RenderGraph::AddCopyBufferToTexturePass(std::string_view name, RgBufferValue source, RgTextureValue destination,
                                                     const render::BufferTextureCopyRegion& region, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(destination.Index, destination.Generation, true, pass.Index) ||
        !UseBuffer(pass.Index, source, RgBufferAccess::CopySource, true, false).IsValid()) return pass;
    const auto& dst = impl.Resources[destination.Index].TextureDesc;
    const auto validation = render::ValidateBufferTextureCopyRegion(impl.Resources[source.Index].BufferDesc, dst, region, impl.Device.GetDetail());
    if (!validation.Supported) {
        impl.Error("CopyBufferTextureRegion", validation.Reason, pass.Index);
        return pass;
    }
    const bool partial = region.X != 0 || region.Y != 0 || region.Width != std::max(1u, dst.Width >> region.MipLevel) ||
                         region.Height != std::max(1u, dst.Height >> region.MipLevel);
    const render::SubresourceRange range{region.ArrayLayer, 1, region.MipLevel, 1};
    impl.Passes[pass.Index].Accesses.back().Bytes = {region.SourceOffset, uint64_t{region.RowPitch} * (region.Height - 1) + uint64_t{region.Width} * render::GetTextureFormatBytesPerPixel(dst.Format)};
    impl.Passes[pass.Index].Accesses.push_back({destination.Index, range, static_cast<uint32_t>(render::TextureState::CopyDestination), partial, true, true, destination.Version});
    Impl::Copy copy{Impl::CopyType::BufferToTexture, source.Index, destination.Index};
    copy.Upload = region;
    impl.Passes[pass.Index].CopyOp = copy;
    return pass;
}

bool RenderGraph::Impl::ValidateResources() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::ValidateResources");
    Report.Resources.reserve(Resources.size());
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        string descriptor;
        if (resource.IsTexture) {
            ++Report.Textures;
            auto desc = resource.TextureDesc;
            if (resource.External() || resource.Port) desc.Hints = desc.Hints & render::ResourceHint::Dedicated;
            const auto validation = render::ValidateTextureDescriptor(desc, Device);
            if (!validation.Supported) {
                Error("UnsupportedTexture", validation.Reason, InvalidIndex, index);
                continue;
            }
            descriptor = fmt::format("{} {}x{}x{} mips={} samples={} usage={}", desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Usage);
            resource.Valid.assign(resource.CellCount(), 0);
            if (resource.ExternalTexture) {
                const auto& external = *resource.ExternalTexture;
                if (!(TexturePoolKey{external.Texture->GetDesc()} == TexturePoolKey{external.Desc}) || external.SubresourceStates.size() != resource.SubresourceCount() || (external.ContentValid.size() != resource.CellCount() && external.ContentValid.size() != resource.SubresourceCount())) {
                    Error("ExternalStorage", "External descriptor or state/validity storage does not match native texture", InvalidIndex, index);
                    continue;
                }
                for (uint32_t cell = 0; cell < resource.CellCount(); ++cell) resource.Valid[cell] = external.ContentValid[cell % external.ContentValid.size()];
                for (size_t cell = 0; cell < external.SubresourceStates.size(); ++cell) {
                    const auto state = external.SubresourceStates[cell];
                    if (!ValidTextureState(external.Desc, state, !resource.Valid[cell])) Error("ExternalState", "Valid external contents require a defined state", InvalidIndex, index);
                }
            }
        } else {
            ++Report.Buffers;
            const auto& desc = resource.BufferDesc;
            constexpr uint32_t knownUses = 2047;
            if (desc.Size == 0 || desc.Size > Device.GetCapabilities().Limits.MaxBufferSize || !EnumContains(desc.Memory) || !desc.Usage ||
                (desc.Usage.value() & ~knownUses) != 0 || (desc.Hints.value() & ~uint32_t{7}) != 0 ||
                (!resource.External() && !resource.Port && desc.Hints.HasFlag(render::ResourceHint::External)) ||
                (desc.Memory == render::MemoryType::Upload && !desc.Usage.HasFlag(render::BufferUse::MapWrite)) ||
                (desc.Memory == render::MemoryType::ReadBack && !desc.Usage.HasFlag(render::BufferUse::MapRead))) {
                Error("UnsupportedBuffer", "Invalid buffer size, usage, memory or hints", InvalidIndex, index);
                continue;
            }
            descriptor = fmt::format("size={} memory={} usage={}", desc.Size, EnumName(desc.Memory), desc.Usage);
            resource.Valid.assign(resource.CellCount(), resource.ExternalBuffer && resource.ExternalBuffer->ContentValid ? 1 : 0);
            if (resource.ExternalBuffer && (!(BufferPoolKey{resource.ExternalBuffer->Buffer->GetDesc()} == BufferPoolKey{desc}) || !resource.ExternalBuffer->State ||
                                            (resource.Valid[0] && resource.ExternalBuffer->State.HasFlag(render::BufferState::Undefined)))) {
                Error("ExternalStorage", "External buffer descriptor or initial state is invalid", InvalidIndex, index);
            }
        }
        if (!EnumContains(resource.ExternalAccess)) Error("ExternalAccess", "Invalid external access mode", InvalidIndex, index);
        Report.Resources.push_back({resource.Name, std::move(descriptor), resource.IsTexture, resource.External()});
        Report.Resources.back().ViewId = resource.ViewId;
        Report.Resources.back().Port = resource.Port;
        Report.Resources.back().Immutable = resource.Immutable;
        Report.Resources.back().RetainedOwner = resource.ExternalTexture ? bool(resource.ExternalTexture->Owner) : resource.ExternalBuffer ? bool(resource.ExternalBuffer->Owner)
                                                                                                                                           : false;
        Report.Resources.back().EstimatedBytes = resource.IsTexture ? EstimateTextureBytes(resource.TextureDesc) : resource.BufferDesc.Size;
    }
    return Report.Diagnostics.empty();
}

bool RenderGraph::Impl::NormalizePasses() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::NormalizePasses");
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        auto& pass = Passes[p];
        unordered_map<uint64_t, uint32_t> cellMap;
        vector<uint32_t> cells;
        for (const auto& access : pass.Accesses) {
            auto& resource = Resources[access.Resource];
            if (access.Write && resource.External() && resource.ExternalAccess == RenderGraphExternalAccess::ReadOnly) Error("ReadOnlyExternal", "Cannot write a read-only external resource", p, access.Resource);
            if ((access.Stages.value() & ~uint32_t{7}) != 0) Error("ShaderStages", "Access contains unsupported shader-stage bits", p, access.Resource);
            cells.clear();
            if (resource.IsTexture) {
                for (uint32_t aspect = 0; aspect < resource.AspectCount(); ++aspect) {
                    const auto bit = aspect == 1 ? render::TextureAspect::Stencil : render::IsDepthStencilFormat(resource.TextureDesc.Format) ? render::TextureAspect::Depth
                                                                                                                                              : render::TextureAspect::Color;
                    if (access.Range.Aspects && !access.Range.Aspects.HasFlag(bit)) continue;
                    for (uint32_t layer = access.Range.BaseArrayLayer; layer < access.Range.BaseArrayLayer + access.Range.ArrayLayerCount; ++layer)
                        for (uint32_t mip = access.Range.BaseMipLevel; mip < access.Range.BaseMipLevel + access.Range.MipLevelCount; ++mip)
                            cells.push_back(aspect * resource.SubresourceCount() + layer * resource.TextureDesc.MipLevels + mip);
                }
            } else {
                for (uint32_t c = 0; c < resource.CellCount(); ++c)
                    if (resource.BufferBoundaries[c] >= access.Bytes.Offset && resource.BufferBoundaries[c + 1] <= access.Bytes.Offset + access.Bytes.Size)
                        cells.push_back(c);
            }
            for (const auto cell : cells) {
                const uint64_t key = (uint64_t{access.Resource} << 32) | cell;
                auto [it, inserted] = cellMap.emplace(key, static_cast<uint32_t>(pass.Cells.size()));
                if (inserted)
                    pass.Cells.push_back({access.Resource, cell, access.State, access.Read, access.Write, access.ValidAfter, access.Version, access.Stages});
                else {
                    auto& existing = pass.Cells[it->second];
                    if (existing.Write || access.Write)
                        Error("OverlappingAccess", "Overlapping writes require one explicit read-write declaration; attachment feedback is unsupported", p, access.Resource);
                    else if (existing.Version != access.Version)
                        Error("VersionFeedback", "One pass cannot access overlapping versions of the same physical range", p, access.Resource);
                    else {
                        const auto uav = resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
                        if (existing.State != access.State && ((existing.State | access.State) & uav)) Error("IncompatibleReadStates", "UAV and other read layouts cannot be combined", p, access.Resource);
                        existing.State |= access.State;
                        existing.Stages |= access.Stages;
                    }
                }
            }
        }
        if (pass.AllowUavWrites) {
            const render::ShaderStages supported = Device.GetCapabilities().Features.UavWriteStages;
            const uint32_t requiredBits = pass.UavWriteStages.value();
            if (requiredBits == 0 || (requiredBits & ~supported.value()) != 0) {
                Error("UnsupportedUavStage",
                      fmt::format("Raster UAV writes require shader stages {} but the device supports {}",
                                  requiredBits, supported.value()),
                      p);
            }
        }
        for (const auto& cell : pass.Cells)
            if (Resources[cell.Resource].IsTexture && !ValidTextureState(Resources[cell.Resource].TextureDesc, static_cast<render::TextureState>(cell.State), false))
                Error("IncompatibleTextureStates", "Texture access requires an invalid usage or incompatible layouts", p, cell.Resource);
        // The current Direct-queue RHI tracks a whole buffer state. Content validity
        // remains byte-granular, but simultaneous disjoint accesses must share one
        // legal native state. Never widen a write layout into an incompatible read.
        unordered_map<uint32_t, uint32_t> bufferStates;
        for (const auto& cell : pass.Cells) {
            if (Resources[cell.Resource].IsTexture) continue;
            auto [it, inserted] = bufferStates.emplace(cell.Resource, cell.State);
            constexpr uint32_t exclusive = uint32_t(render::BufferState::UnorderedAccess) | uint32_t(render::BufferState::CopyDestination) | uint32_t(render::BufferState::HostRead);
            if (!inserted && it->second != cell.State && ((it->second | cell.State) & exclusive))
                Error("IncompatibleBufferStates", "Disjoint buffer ranges require incompatible whole-buffer states in one pass", p, cell.Resource);
            it->second |= cell.State;
            const auto& resource = Resources[cell.Resource];
            if (resource.Immutable && resource.BufferDesc.Memory == render::MemoryType::Device) it->second |= resource.ExternalBuffer->State.value();
        }
        for (auto& cell : pass.Cells)
            if (!Resources[cell.Resource].IsTexture) cell.State = bufferStates[cell.Resource];
        unordered_map<uint64_t, uint32_t> textureStates;
        for (const auto& cell : pass.Cells) {
            const auto& resource = Resources[cell.Resource];
            if (!resource.IsTexture || resource.AspectCount() == 1) continue;
            const uint64_t key = uint64_t(cell.Resource) << 32 | resource.PhysicalCell(cell.Cell);
            auto [it, inserted] = textureStates.emplace(key, cell.State);
            if (!inserted && it->second != cell.State) Error("IncompatibleAspectStates", "Depth and stencil share a native layout; incompatible simultaneous aspect accesses are unsupported", p, cell.Resource);
        }
        if (Report.Passes[p].Type != RgPassType::Raster) continue;
        const auto checkAttachment = [&](uint32_t viewIndex, render::LoadAction load, render::StoreAction store) {
            const auto& view = Views[viewIndex];
            const auto& desc = Resources[view.Resource].TextureDesc;
            const uint32_t width = std::max(1u, desc.Width >> view.Key.Range.BaseMipLevel), height = std::max(1u, desc.Height >> view.Key.Range.BaseMipLevel);
            if (!EnumContains(load) || !EnumContains(store)) Error("AttachmentAction", "Invalid attachment load/store action", p, view.Resource);
            if (pass.Width != 0 && (pass.Width != width || pass.Height != height || pass.Samples != desc.SampleCount || pass.Layers != view.Key.Range.ArrayLayerCount)) {
                Error("AttachmentMismatch", "All raster attachments must match extent, samples and layer count", p, view.Resource);
            }
            pass.Width = width;
            pass.Height = height;
            pass.Samples = desc.SampleCount;
            pass.Layers = view.Key.Range.ArrayLayerCount;
        };
        for (const auto& color : pass.Colors) {
            if (!color)
                Error("AttachmentHole", "Color attachment slots must be contiguous", p);
            else
                checkAttachment(color->View, color->Desc.Load, color->Desc.Store);
        }
        if (pass.DepthAttachment) checkAttachment(pass.DepthAttachment->View, pass.DepthAttachment->Desc.Load, pass.DepthAttachment->Desc.Store);
        if (pass.Width == 0) Error("MissingAttachment", "Raster passes require at least one attachment", p);
    }
    return Report.Diagnostics.empty();
}

void RenderGraph::Impl::Cull() {
    GraphCompileWorkspace localWorkspace;
    auto& workspace = FrameResources ? FrameResources->_impl->CompileWorkspace : localWorkspace;
    auto& versions = workspace.Versions;
    auto& nodes = workspace.Nodes;
    auto& values = workspace.Values;
    auto& producers = workspace.Producers;
    auto& initialized = workspace.Initialized;
    uint64_t hash = 0;
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::BuildIR");
        versions.clear();
        nodes.resize(Passes.size());
        for (auto& node : nodes) {
            node.Reads.clear();
            node.Writes.clear();
        }
        values.resize(Resources.size());
        producers.resize(Resources.size());
        initialized.resize(Resources.size());
        for (uint32_t r = 0; r < Resources.size(); ++r) {
            const auto count = Resources[r].VersionParents.size();
            values[r].resize(count);
            producers[r].resize(count);
            initialized[r].resize(count);
            for (auto& cells : producers[r]) cells.assign(Resources[r].CellCount(), InvalidIndex);
            for (auto& cells : initialized[r]) cells.assign(Resources[r].CellCount(), 0);
        }
        for (uint32_t p = 0; p < Passes.size(); ++p) {
            nodes[p].SideEffect = Passes[p].SideEffect;
            for (const auto& access : Passes[p].Cells) {
                if (access.Version >= values[access.Resource].size() || (access.Write && access.Version == 0)) {
                    Error("InvalidVersion", "Access references an unknown version or writes an imported initial value; reserve a successor with NextVersion", p, access.Resource);
                    continue;
                }
                if (!access.Write) continue;
                auto& producer = producers[access.Resource][access.Version][access.Cell];
                if (producer != InvalidIndex && producer != p)
                    Error("MultipleProducers", "A content version/range can have only one producer; reserve a successor with NextVersion", p, access.Resource);
                producer = p;
                initialized[access.Resource][access.Version][access.Cell] = access.ValidAfter ? 1 : 0;
            }
        }
        if (!Report.Diagnostics.empty()) return;
        for (uint32_t r = 0; r < Resources.size(); ++r) {
            const auto& resource = Resources[r];
            for (uint32_t v = 0; v < values[r].size(); ++v) {
                auto& cells = values[r][v];
                cells.resize(resource.CellCount());
                for (uint32_t c = 0; c < resource.CellCount(); ++c) {
                    const uint32_t predecessor = v == 0 ? InvalidIndex : values[r][resource.VersionParents[v]][c];
                    const uint32_t producer = producers[r][v][c];
                    if (v != 0 && producer == InvalidIndex) {
                        cells[c] = predecessor;
                        continue;
                    }
                    cells[c] = static_cast<uint32_t>(versions.size());
                    versions.push_back({r, c, v, producer, predecessor, v == 0 ? resource.Valid[c] != 0 : initialized[r][v][c] != 0});
                }
            }
        }
        for (uint32_t p = 0; p < Passes.size(); ++p) {
            for (const auto& access : Passes[p].Cells) {
                const auto& resource = Resources[access.Resource];
                const auto version = access.Write ? resource.VersionParents[access.Version] : access.Version;
                if (access.Read) AddUnique(nodes[p].Reads, values[access.Resource][version][access.Cell]);
                if (access.Write) AddUnique(nodes[p].Writes, values[access.Resource][access.Version][access.Cell]);
            }
        }
        auto& roots = workspace.Roots;
        roots.clear();
        for (uint32_t r = 0; r < Resources.size(); ++r)
            if (Resources[r].ExternalAccess == RenderGraphExternalAccess::ObservableOutput)
                for (const auto v : values[r].back())
                    if (versions[v].Producer != InvalidIndex) roots.push_back(v);
        hash = HashRenderGraphCompileInput(static_cast<uint32_t>(Resources.size()), versions, nodes, roots, Options);
    }
    auto& cache = workspace.Cache;
    auto& roots = workspace.Roots;
    if (Options.ReuseCompiledPlan && cache.Occupied && cache.Hash == hash &&
        EqualRenderGraphCompileInput(static_cast<uint32_t>(Resources.size()), versions, nodes, roots, Options,
                                      cache.ResourceCount, cache.Versions, cache.Nodes, cache.Roots, cache.Options)) {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::CompilePlanHit");
        CompiledGraph = cache.Result;
        ++cache.Hits;
        Report.CompilePlanReused = true;
    } else {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::CompilePlanMiss");
        CompiledGraph = CompileRenderGraph(static_cast<uint32_t>(Resources.size()), versions, nodes, roots, Options, workspace.Compiler);
        cache.Hash = hash;
        cache.ResourceCount = static_cast<uint32_t>(Resources.size());
        cache.Versions = versions;
        cache.Nodes = nodes;
        cache.Roots = roots;
        cache.Options = Options;
        cache.Result = CompiledGraph;
        cache.Occupied = true;
        ++cache.Misses;
        Report.CompilePlanReused = false;
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::ApplyCompiledPlan");
        for (const auto& diagnostic : CompiledGraph.Diagnostics)
            Error(diagnostic.Code, diagnostic.Message, diagnostic.Pass, diagnostic.Resource);
        for (uint32_t p = 0; p < Passes.size(); ++p) {
            const auto& compiled = CompiledGraph.Passes[p];
            auto& report = Report.Passes[p];
            report.Live = compiled.Live;
            report.DataDependencies = compiled.DataDependencies;
            report.HazardDependencies = compiled.HazardDependencies;
            report.LivenessReason = compiled.LivenessReason;
            report.Reads = compiled.Reads;
            report.Writes = compiled.Writes;
            if (compiled.Live) ++Report.LivePasses;
        }
        for (uint32_t r = 0; r < Resources.size(); ++r) {
            Report.Resources[r].FirstUse = CompiledGraph.Lifetimes[r].FirstUse;
            Report.Resources[r].LastUse = CompiledGraph.Lifetimes[r].LastUse;
        }
        Report.Versions = CompiledGraph.Versions;
        Report.ExecutionOrder = CompiledGraph.ExecutionOrder;
        Report.CulledPasses = Report.DeclaredPasses - Report.LivePasses;
    }
}

void RenderGraph::SetCompileOptions(RenderGraphCompileOptions options) {
    if (_impl->Mutable()) _impl->Options = options;
}
const CompiledRenderGraph& RenderGraph::GetCompiledGraph() const noexcept { return _impl->CompiledGraph; }

bool RenderGraph::Compile() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Compile");
    auto& impl = *_impl;
    if (impl.Frozen) return impl.Compiled && impl.Report.Diagnostics.empty();
    impl.Frozen = true;
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Validate");
        if (!impl.ResolvePorts()) return false;
        impl.Report.DeclaredPasses = static_cast<uint32_t>(impl.Passes.size());
        for (auto& resource : impl.Resources)
            if (!resource.IsTexture) resource.BufferBoundaries = {0, resource.BufferDesc.Size};
        for (auto& pass : impl.Passes)
            for (auto& access : pass.Accesses) {
                auto& resource = impl.Resources[access.Resource];
                if (resource.IsTexture) continue;
                const uint64_t size = resource.BufferDesc.Size;
                if (access.Bytes.Offset > size) {
                    impl.Error("BufferRange", "Buffer range starts outside resource", InvalidIndex, access.Resource);
                    continue;
                }
                if (access.Bytes.Size == render::BufferRange::All()) access.Bytes.Size = size - access.Bytes.Offset;
                if (access.Bytes.Size == 0 || access.Bytes.Size > size - access.Bytes.Offset) {
                    impl.Error("BufferRange", "Buffer range is empty or outside resource", InvalidIndex, access.Resource);
                    continue;
                }
                resource.BufferBoundaries.push_back(access.Bytes.Offset);
                resource.BufferBoundaries.push_back(access.Bytes.Offset + access.Bytes.Size);
            }
        for (auto& resource : impl.Resources) {
            auto& boundaries = resource.BufferBoundaries;
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        }
        if (!impl.Report.Diagnostics.empty() || !impl.ValidateResources() || !impl.NormalizePasses()) return false;
        for (uint32_t p = 0; p < impl.Passes.size(); ++p)
            for (const auto& access : impl.Passes[p].Accesses)
                impl.Report.Passes[p].Accesses.push_back({access.Resource, access.Version, access.State, access.Range, access.Bytes, access.Stages, access.Read, access.Write});
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Cull");
        impl.Cull();
    }
    if (impl.Report.Diagnostics.empty()) {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Optimize");
        impl.PlanStorage();
        impl.OptimizeRaster();
        impl.BuildExecutionPlan();
    }
    impl.Compiled = impl.Report.Diagnostics.empty();
    return impl.Compiled;
}

void RenderGraph::Impl::PlanStorage() {
    vector<uint32_t> order;
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        Resources[r].Physical = r;
        if (Report.Resources[r].FirstUse >= 0) order.push_back(r);
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return Report.Resources[a].FirstUse < Report.Resources[b].FirstUse; });
    struct Slot {
        uint32_t Resource;
        int32_t LastUse;
    };
    vector<Slot> slots;
    for (const uint32_t r : order) {
        auto& resource = Resources[r];
        const auto& life = Report.Resources[r];
        const bool reusable = Options.ReuseResources && !resource.External() &&
                              (resource.IsTexture ? resource.TextureDesc.Memory : resource.BufferDesc.Memory) == render::MemoryType::Device;
        if (reusable) {
            for (auto& slot : slots) {
                const auto& candidate = Resources[slot.Resource];
                if (slot.LastUse >= life.FirstUse || candidate.IsTexture != resource.IsTexture) continue;
                const bool matches = resource.IsTexture ? TexturePoolKey{resource.TextureDesc} == TexturePoolKey{candidate.TextureDesc} : BufferPoolKey{resource.BufferDesc} == BufferPoolKey{candidate.BufferDesc};
                if (!matches) continue;
                resource.Physical = slot.Resource;
                slot.LastUse = life.LastUse;
                ++Report.ReusedResources;
                break;
            }
            if (resource.Physical == r) slots.push_back({r, life.LastUse});
        }
        Report.Resources[r].PhysicalSlot = resource.Physical;
    }
}

void RenderGraph::Impl::OptimizeRaster() {
    vector<uint8_t> consumed(CompiledGraph.Versions.size(), 0);
    for (const auto p : CompiledGraph.ExecutionOrder)
        for (const auto value : CompiledGraph.Passes[p].Reads) consumed[value] = 1;
    uint32_t previous = InvalidIndex;
    for (const auto p : CompiledGraph.ExecutionOrder) {
        auto& pass = Passes[p];
        auto& report = Report.Passes[p];
        if (report.Type != RgPassType::Raster) {
            previous = InvalidIndex;
            continue;
        }
        report.RasterGroup = p;
        pass.MergeTail = p;
        const auto discard = [&](uint32_t view, render::StoreAction& store) {
            const auto resource = Views[view].Resource;
            if (store != render::StoreAction::Store || Resources[resource].External()) return;
            for (const auto value : CompiledGraph.Passes[p].Writes)
                if (CompiledGraph.Versions[value].Resource == resource && consumed[value]) return;
            store = render::StoreAction::Discard;
            for (auto& cell : pass.Cells)
                if (cell.Resource == resource && cell.Write) cell.ValidAfter = false;
            report.Decisions.push_back(fmt::format("Discard store for {}: no live consumer", Resources[resource].Name));
            ++Report.DiscardedStores;
        };
        if (Options.OptimizeAttachmentStores) {
            for (auto& color : pass.Colors) discard(color->View, color->Desc.Store);
            if (pass.DepthAttachment && !pass.DepthAttachment->Desc.ReadOnly) discard(pass.DepthAttachment->View, pass.DepthAttachment->Desc.Store);
        }
        const auto attachmentOnly = [&](const Pass& entry) {
            if (entry.AllowUavWrites) return false;
            for (const auto& access : entry.Accesses) {
                if (!Resources[access.Resource].IsTexture || (access.State != uint32_t(render::TextureState::RenderTarget) && access.State != uint32_t(render::TextureState::DepthRead) && access.State != uint32_t(render::TextureState::DepthWrite))) return false;
            }
            return true;
        };
        const auto sameView = [&](uint32_t a, uint32_t b) { return Views[a].Resource == Views[b].Resource && Views[a].Key == Views[b].Key; };
        bool merge = Options.MergeRasterPasses && previous != InvalidIndex && attachmentOnly(pass) && attachmentOnly(Passes[previous]);
        if (merge) {
            const auto& prior = Passes[previous];
            merge = prior.Colors.size() == pass.Colors.size() && prior.DepthAttachment.has_value() == pass.DepthAttachment.has_value();
            if (merge)
                for (size_t i = 0; i < pass.Colors.size(); ++i)
                    merge &= sameView(prior.Colors[i]->View, pass.Colors[i]->View) && pass.Colors[i]->Desc.Load == render::LoadAction::Load && prior.Colors[i]->Desc.Store == render::StoreAction::Store;
            if (merge && pass.DepthAttachment) merge &= sameView(prior.DepthAttachment->View, pass.DepthAttachment->View) && prior.DepthAttachment->Desc.ReadOnly == pass.DepthAttachment->Desc.ReadOnly && pass.DepthAttachment->Desc.Load == render::LoadAction::Load && prior.DepthAttachment->Desc.Store == render::StoreAction::Store;
        }
        if (merge) {
            report.RasterGroup = Report.Passes[previous].RasterGroup;
            Passes[report.RasterGroup].MergeTail = p;
            report.Decisions.push_back("Merged: adjacent raster passes preserve identical attachments and need no non-attachment barriers");
            ++Report.MergedRasterPasses;
        } else
            report.Decisions.push_back(Options.MergeRasterPasses ? "Raster boundary: attachment identity, load/store, or non-attachment access requires separation" : "Raster merging disabled");
        previous = p;
    }
}

bool RenderGraph::Impl::Realize() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Realize");
    const uint64_t createdBefore = Pool.GetStats().Created;
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        if (resource.ExternalBuffer) NativeBuffers.emplace(resource.ExternalBuffer->Buffer, r);
        if (Report.Resources[r].FirstUse < 0) continue;
        if (resource.Physical != r) continue;
        if (resource.IsTexture) {
            if (!resource.ExternalTexture) {
                resource.PoolTexture = Pool.AcquireTexture(resource.TextureDesc, resource.Name, resource.ViewId);
                if (!resource.PoolTexture) {
                    Error("TextureAllocation", "Texture allocation failed before recording", InvalidIndex, r);
                    return false;
                }
                Report.Resources[r].PhysicalId = resource.PoolTexture->Id;
            }
            const auto states = resource.ExternalTexture ? std::span<const render::TextureStates>{resource.ExternalTexture->SubresourceStates} : std::span<const render::TextureStates>{resource.PoolTexture->States};
            for (const auto state : states) resource.States.push_back(state.value());
        } else {
            if (resource.Readback) {
                auto buffer = Device.CreateBuffer(resource.BufferDesc);
                if (!buffer) {
                    Error("ReadbackAllocation", "Readback allocation failed", InvalidIndex, r);
                    return false;
                }
                resource.Readback->Buffer = shared_ptr<render::Buffer>(buffer.Release());
            } else if (!resource.ExternalBuffer) {
                resource.PoolBuffer = Pool.AcquireBuffer(resource.BufferDesc, resource.Name, resource.ViewId);
                if (!resource.PoolBuffer) {
                    Error("BufferAllocation", "Buffer allocation failed before recording", InvalidIndex, r);
                    return false;
                }
                Report.Resources[r].PhysicalId = resource.PoolBuffer->Id;
            }
            resource.States.assign(1, (resource.ExternalBuffer ? resource.ExternalBuffer->State : resource.Readback ? InitialBufferState(resource.BufferDesc)
                                                                                                                    : resource.PoolBuffer->State)
                                          .value());
            NativeBuffers.emplace(resource.NativeBuffer(), r);
        }
    }
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        if (resource.Physical == r || Report.Resources[r].FirstUse < 0) continue;
        auto& physical = Resources[resource.Physical];
        resource.PoolTexture = physical.PoolTexture;
        resource.PoolBuffer = physical.PoolBuffer;
        resource.States = physical.States;
        Report.Resources[r].PhysicalId = Report.Resources[resource.Physical].PhysicalId;
    }
    for (const uint32_t p : CompiledGraph.ExecutionOrder) {
        auto& pass = Passes[p];
        for (const auto& access : pass.Accesses)
            if (!Resources[access.Resource].IsTexture && access.Read)
                pass.BufferReadStates[Resources[access.Resource].NativeBuffer()] |= access.State;
        for (const auto v : pass.DeclaredViews) {
            auto& view = Views[v];
            if (view.Native) continue;
            auto& resource = Resources[view.Resource];
            if (resource.PoolTexture)
                view.Native = Pool.GetTextureView(*resource.PoolTexture, view.Key);
            else {
                auto persistent = resource.ExternalTexture->PersistentViews;
                if (persistent) {
                    for (const auto& cached : *persistent)
                        if (cached.Key == view.Key) {
                            view.Native = cached.View.get();
                            break;
                        }
                    if (!view.Native) {
                        auto native = Device.CreateTextureView({resource.NativeTexture(), view.Key.Dimension, view.Key.Format, view.Key.Range, view.Key.Usage});
                        if (native) {
                            view.Native = native.Get();
                            persistent->push_back({view.Key, native.Release()});
                        }
                    }
                }
                const auto borrowed = resource.ExternalTexture->ColorAttachmentView;
                if (!view.Native && borrowed) {
                    const auto& desc = borrowed->GetDesc();
                    const auto range = render::NormalizeSubresourceRange(resource.TextureDesc, desc.Range);
                    if (range && desc.Target == resource.NativeTexture() && TextureViewKey{desc.Dim, desc.Format, *range, desc.Usage} == view.Key) view.Native = borrowed;
                }
                if (!view.Native && !persistent) view.Native = Pool.CreateExternalTextureView({resource.NativeTexture(), view.Key.Dimension, view.Key.Format, view.Key.Range, view.Key.Usage});
            }
            if (!view.Native) {
                Error("ViewAllocation", "Texture view allocation failed before recording", p, view.Resource);
                return false;
            }
        }
        if (Report.Passes[p].Type != RgPassType::Raster) continue;
        const auto group = Report.Passes[p].RasterGroup;
        if (group != p) {
            pass.NativePass = Passes[group].NativePass;
            pass.Framebuffer = Passes[group].Framebuffer;
            pass.PassState = Passes[group].PassState;
            continue;
        }
        const uint32_t tail = pass.MergeTail;
        vector<render::RenderPassColorAttachmentDescriptor> colors;
        vector<render::TextureView*> views;
        vector<render::TextureFormat> formats;
        size_t colorIndex = 0;
        for (const auto& attachment : pass.Colors) {
            const auto& view = Views[attachment->View];
            colors.push_back({view.Key.Format, pass.Samples, attachment->Desc.Load, Passes[tail].Colors[colorIndex++]->Desc.Store});
            formats.push_back(view.Key.Format);
            views.push_back(view.Native.Get());
            pass.Clears.push_back(attachment->Desc.Clear);
        }
        std::optional<render::RenderPassDepthStencilAttachmentDescriptor> depth;
        std::optional<render::TextureFormat> depthFormat;
        Nullable<render::TextureView*> depthView{nullptr};
        if (pass.DepthAttachment) {
            const auto& attachment = *pass.DepthAttachment;
            const auto& view = Views[attachment.View];
            depthFormat = view.Key.Format;
            depthView = view.Native;
            depth = render::RenderPassDepthStencilAttachmentDescriptor{view.Key.Format, pass.Samples, attachment.Desc.Load, Passes[tail].DepthAttachment->Desc.Store, attachment.Desc.Load, Passes[tail].DepthAttachment->Desc.Store, attachment.Desc.ReadOnly};
        }
        pass.NativePass = Registry.GetOrCreateRenderPass({colors, depth});
        if (!pass.NativePass) {
            Error("RenderPassAllocation", "Render pass realization failed before recording", p);
            return false;
        }
        pass.Framebuffer = Registry.GetOrCreateFramebuffer({pass.NativePass.Get(), views, depthView.Get(), pass.Width, pass.Height, pass.Layers});
        if (!pass.Framebuffer) {
            Error("FramebufferAllocation", "Framebuffer realization failed before recording", p);
            return false;
        }
        pass.PassState.emplace(std::move(formats), depthFormat, pass.Samples, pass.NativePass.Get());
        pass.PassState->DepthReadOnly = pass.DepthAttachment && pass.DepthAttachment->Desc.ReadOnly;
    }
    Report.PhysicalAllocations = static_cast<uint32_t>(Pool.GetStats().Created - createdBefore);
    return true;
}

bool RenderGraph::Prepare() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Prepare");
    auto& impl = *_impl;
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PrepareUploads");
        for (uint32_t p : impl.CompiledGraph.ExecutionOrder) {
            auto& pass = impl.Passes[p];
            if (pass.UploadBytes.empty()) continue;
            auto& resource = impl.Resources[pass.Accesses.front().Resource];
            ScopedBufferMap map{resource.NativeBuffer(), {0, pass.UploadBytes.size()}};
            if (!map) {
                impl.Error("UploadMap", "Graph upload buffer mapping failed before recording", p);
                return false;
            }
            std::memcpy(map.Data(), pass.UploadBytes.data(), pass.UploadBytes.size());
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PreparePipelines");
        for (auto& value : impl.GraphicsPrograms) {
        if (!impl.Report.Passes[value.Pass].Live) continue;
        const auto before = value.Program->GetGraphicsPipelineStateCount();
        value.PipelineState = value.Program->GetOrCreateGraphicsPipelineState(value.State, value.Layout, value.Topology,
                                                                              *impl.Passes[value.Pass].PassState);
        ++impl.Report.GraphicsPipelinePreparations;
        impl.Report.GraphicsPipelineCreations += static_cast<uint32_t>(value.Program->GetGraphicsPipelineStateCount() - before);
        if (!value.PipelineState) {
            impl.Error("GraphicsPipelineState", "Graphics pipeline state creation failed before recording", value.Pass);
            return false;
        }
    }
    for (Impl::ComputeProgram& value : impl.ComputePrograms) {
        if (!impl.Report.Passes[value.Pass].Live) continue;
        value.PipelineState = value.Program->GetOrCreateComputePipelineState();
        if (!value.PipelineState) {
            impl.Error("ComputePipelineState", "Compute pipeline state creation failed before recording", value.Pass);
            return false;
        }
    }
    }

    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PrepareParameters");
        const bool hasLiveParameters = std::any_of(
        impl.ParameterSets.begin(), impl.ParameterSets.end(),
        [&](const Impl::ParameterSet& value) { return impl.Report.Passes[value.Pass].Live; });
    if (!hasLiveParameters) return true;
    if (!impl.FrameResources || !impl.FrameResources->_impl->Arena ||
        !impl.FrameResources->_impl->Arena->IsValid()) {
        impl.Error("ParameterStorage", "Graph parameter sets require initialized per-flight frame resources");
        return false;
    }
    RenderGraphFrameResources::Impl& frame = *impl.FrameResources->_impl;

    for (Impl::ParameterSet& parameterSet : impl.ParameterSets) {
        if (!impl.Report.Passes[parameterSet.Pass].Live) continue;
        parameterSet.DynamicOffsets.clear();
        RenderGraphFrameResources::Impl::ParameterSetKey key{
            .Layout = parameterSet.Program->GetPipelineLayout(),
            .Group = parameterSet.Group,
            .Values = {}};
        struct PreparedBinding {
            render::BindingHandle Handle;
            uint32_t ArrayElement{0};
            render::ShaderParameterValue Value;
            string Declaration;
        };
        vector<PreparedBinding> prepared;
        prepared.reserve(parameterSet.Bindings.size());
        key.Values.reserve(parameterSet.Bindings.size());

        for (const Impl::ParameterBinding& binding : parameterSet.Bindings) {
            const render::BindingHandle handle =
                parameterSet.Program->GetPipelineLayout()->FindBinding(binding.Declaration);
            if (!handle.IsValid()) {
                impl.Error("ParameterBinding", "Resolved pipeline binding is unavailable during preparation",
                           parameterSet.Pass, InvalidIndex, binding.Declaration);
                return false;
            }

            render::ShaderParameterValue value;
            if (const auto* bytes = std::get_if<Impl::CBufferBytes>(&binding.Value)) {
                DynamicCBufferArena::Reservation reservation = frame.Arena->Reserve(bytes->Bytes.size());
                if (!reservation.IsValid()) {
                    impl.Error("ParameterUpload", "Constant upload allocation failed before recording",
                               parameterSet.Pass, InvalidIndex, binding.Declaration);
                    return false;
                }
                std::memcpy(reservation.Data(), bytes->Bytes.data(), bytes->Bytes.size());
                const DynamicCBufferArena::Allocation allocation = reservation.Commit(bytes->Bytes.size());
                if (!allocation.IsValid() ||
                    (binding.Info.Dynamic && allocation.Offset > std::numeric_limits<uint32_t>::max())) {
                    impl.Error("ParameterUpload", "Constant upload commit or dynamic offset conversion failed",
                               parameterSet.Pass, InvalidIndex, binding.Declaration);
                    return false;
                }
                value = render::ShaderBufferBinding{
                    allocation.Target,
                    {binding.Info.Dynamic ? 0 : allocation.Offset, allocation.Size},
                    0};
                if (binding.Info.Dynamic) {
                    parameterSet.DynamicOffsets.push_back(
                        {handle, static_cast<uint32_t>(allocation.Offset)});
                }
            } else if (const auto* texture = std::get_if<Impl::TextureParameter>(&binding.Value)) {
                render::TextureView* view = impl.Views[texture->View].Native.Get();
                if (view == nullptr) {
                    impl.Error("ParameterTexture", "Graph texture view was not realized before parameter preparation",
                               parameterSet.Pass, texture->Resource, binding.Declaration);
                    return false;
                }
                value = view;
            } else if (const auto* buffer = std::get_if<Impl::BufferParameter>(&binding.Value)) {
                const uint64_t descriptorOffset = binding.Info.Dynamic ? 0 : buffer->Range.Offset;
                if (binding.Info.Dynamic && buffer->Range.Offset > std::numeric_limits<uint32_t>::max()) {
                    impl.Error("ParameterDynamicOffset", "Dynamic buffer offset exceeds the RHI offset width",
                               parameterSet.Pass, buffer->Resource, binding.Declaration);
                    return false;
                }
                render::Buffer* native = impl.Resources[buffer->Resource].NativeBuffer();
                if (shader::IsTexelBufferKind(binding.Info.LogicalKind)) {
                    value = render::ShaderTexelBufferBinding{
                        native, {descriptorOffset, buffer->Range.Size}, buffer->Format};
                } else {
                    value = render::ShaderBufferBinding{
                        native, {descriptorOffset, buffer->Range.Size}, buffer->StructureByteStride};
                }
                if (binding.Info.Dynamic) {
                    parameterSet.DynamicOffsets.push_back(
                        {handle, static_cast<uint32_t>(buffer->Range.Offset)});
                }
            } else {
                const auto& sampler = std::get<Impl::SamplerParameter>(binding.Value);
                const Nullable<render::Sampler*> native = impl.Device.GetOrCreateSampler(sampler.Desc);
                if (!native) {
                    impl.Error("ParameterSampler", "Sampler creation failed before recording",
                               parameterSet.Pass, InvalidIndex, binding.Declaration);
                    return false;
                }
                value = native.Get();
            }

            prepared.push_back({handle, binding.ArrayElement, value, binding.Declaration});
            key.Values.push_back({binding.Declaration, binding.ArrayElement, std::move(value)});
        }
        std::sort(key.Values.begin(), key.Values.end(), [](const auto& a, const auto& b) {
            if (a.Declaration != b.Declaration) return a.Declaration < b.Declaration;
            return a.ArrayElement < b.ArrayElement;
        });

        const auto cached = frame.SetCache.find(key);
        if (cached != frame.SetCache.end()) {
            parameterSet.Native = cached->second;
            continue;
        }
        Nullable<unique_ptr<render::ShaderParameterSet>> created =
            impl.Device.CreateShaderParameterSet({.Layout = key.Layout,
                                                  .GroupIndex = parameterSet.Group});
        if (!created) {
            impl.Error("ParameterSetAllocation", "Parameter set allocation failed before recording",
                       parameterSet.Pass);
            return false;
        }
        unique_ptr<render::ShaderParameterSet> set = created.Release();
        for (const PreparedBinding& binding : prepared) {
            if (!set->Set(binding.Handle, binding.ArrayElement, binding.Value)) {
                impl.Error("ParameterSetWrite", "Parameter set rejected a validated binding value",
                           parameterSet.Pass, InvalidIndex, binding.Declaration);
                return false;
            }
        }
        if (!set->FlushWrites()) {
            impl.Error("ParameterSetFlush", "Parameter set descriptor writes failed before recording",
                       parameterSet.Pass);
            return false;
        }
        parameterSet.Native = set.get();
        frame.Sets.push_back(std::move(set));
        frame.SetCache.emplace(std::move(key), parameterSet.Native.Get());
    }
    }
    return true;
}

void RenderGraph::Impl::BuildExecutionPlan() {
    ExecutionPlan.resize(Passes.size());
    unordered_map<uint64_t, uint32_t> indices;
    for (const uint32_t p : CompiledGraph.ExecutionOrder) {
        auto& plan = ExecutionPlan[p];
        indices.clear();
        plan.Accesses.reserve(Passes[p].Cells.size());
        for (const auto& access : Passes[p].Cells) {
            const auto& resource = Resources[access.Resource];
            const auto physical = resource.Physical, cell = resource.PhysicalCell(access.Cell);
            const auto key = uint64_t{physical} << 32 | cell;
            const auto [it, inserted] = indices.emplace(key, static_cast<uint32_t>(plan.Accesses.size()));
            if (inserted)
                plan.Accesses.push_back({access.Resource, physical, cell, access.State, access.Write, access.Stages});
            else {
                auto& existing = plan.Accesses[it->second];
                existing.Write |= access.Write;
                existing.Stages |= access.Stages;
            }
        }
    }
}

void RenderGraph::Impl::PlanBarriers() {
    vector<vector<uint32_t>> states;
    vector<vector<uint8_t>> writes;
    vector<vector<render::ShaderStages>> stages;
    for (const auto& resource : Resources) {
        states.push_back(resource.States);
        writes.emplace_back(resource.States.size(), 1);
        stages.emplace_back(resource.States.size(), render::ShaderStage::UNKNOWN);
    }
    for (const uint32_t p : CompiledGraph.ExecutionOrder) {
        auto& plan = ExecutionPlan[p];
        vector<uint32_t> uavResources;
        const bool continuation = Report.Passes[p].Type == RgPassType::Raster && Report.Passes[p].RasterGroup != p;
        for (const auto& access : plan.Accesses) {
            auto& resource = Resources[access.Resource];
            const auto physical = access.Physical, cell = access.Cell;
            const bool write = access.Write;
            const auto afterStages = access.Stages;
            auto& state = states[physical][cell];
            auto& beforeStages = stages[physical][cell];
            const uint32_t uav = resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
            const bool sameStateWrite = state != uav && (writes[physical][cell] || write);
            if (!continuation && (state != access.State || sameStateWrite || !Options.EliminateBarriers)) {
                if (resource.IsTexture)
                    plan.Barriers.push_back(render::BarrierTextureDescriptor{.Target = resource.NativeTexture(), .Before = static_cast<render::TextureState>(state), .After = static_cast<render::TextureState>(access.State), .IsSubresourceBarrier = true, .Range = {cell / resource.TextureDesc.MipLevels, 1, cell % resource.TextureDesc.MipLevels, 1}, .BeforeStages = beforeStages, .AfterStages = afterStages});
                else
                    plan.Barriers.push_back(render::BarrierBufferDescriptor{.Target = resource.NativeBuffer(), .Before = static_cast<render::BufferState>(state), .After = static_cast<render::BufferState>(access.State), .BeforeStages = beforeStages, .AfterStages = afterStages});
                Report.Barriers.push_back({p, access.Resource, cell, state, access.State, false,
                                           !resource.IsTexture ? "Whole-buffer state; content dependencies retain byte ranges" : resource.AspectCount() == 2 ? "Coupled depth/stencil native layout; content dependencies retain aspects"
                                                                                                                                                             : "Exact mip/layer"});
                ++Report.TransitionBarriers;
            } else if (!continuation && state == uav && (writes[physical][cell] || write))
                AddUnique(uavResources, access.Resource);
            if (state == access.State && !write && !writes[physical][cell])
                beforeStages |= afterStages;
            else
                beforeStages = afterStages;
            state = access.State;
            writes[physical][cell] = write ? 1 : 0;
        }
        for (const auto r : uavResources) {
            auto& resource = Resources[r];
            render::Resource* native = resource.IsTexture ? static_cast<render::Resource*>(resource.NativeTexture()) : static_cast<render::Resource*>(resource.NativeBuffer());
            plan.Barriers.push_back(render::BarrierUavDescriptor{native});
            const auto state = resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
            Report.Barriers.push_back({p, r, 0, state, state, true, "UAV memory dependency"});
            ++Report.UavBarriers;
        }
    }
}

RenderGraphExecutionResult RenderGraph::Execute(render::CommandBuffer& command) {
    return Execute(command, {});
}

RenderGraphExecutionResult RenderGraph::Execute(render::CommandBuffer& command, std::span<const PresentCommandTarget> presentTargets) {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Execute");
    auto& impl = *_impl;
    if (impl.Executed) {
        impl.Error("AlreadyExecuted", "A graph may execute only once");
        return {};
    }
    impl.Executed = true;
    if (!Compile() || !impl.Realize() || !Prepare()) {
        for (auto& pass : impl.Passes)
            if (pass.Ticket._state) pass.Ticket._state->Cancel();
        impl.Pool.EndGraph();
        impl.Report.Pool = impl.Pool.GetStats();
        PlotGraphCpuStats(impl.Report);
        return {};
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PlanBarriers");
        impl.PlanBarriers();
    }
    RenderGraphExecutionResult result{true, false, {}};
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Record");
    Nullable<unique_ptr<render::GraphicsCommandEncoder>> rasterEncoder{nullptr};
    render::CommandBuffer* rasterCommand = nullptr;
    const auto resolveCommand = [&](const Impl::PassExecutionPlan& plan, uint32_t passIndex) -> render::CommandBuffer* {
        render::CommandBuffer* found = nullptr;
        for (const auto& access : plan.Accesses) {
            // 写 flip 的 blit 与只读 Export（RTV→Present）必须落在同一 present CB。
            auto& resource = impl.Resources[access.Physical];
            if (!resource.IsTexture || (!resource.ExternalTexture && !resource.PoolTexture)) continue;
            render::Texture* texture = resource.NativeTexture();
            for (const PresentCommandTarget& present : presentTargets) {
                if (present.Texture == nullptr || present.Commands == nullptr || present.Texture != texture) continue;
                if (found != nullptr && found != present.Commands) {
                    impl.Error("PresentCommandSplit", "A pass writes more than one presentation surface", passIndex);
                    return &command;
                }
                found = present.Commands;
            }
        }
        return found != nullptr ? found : &command;
    };
    for (size_t order = 0; order < impl.CompiledGraph.ExecutionOrder.size(); ++order) {
        const uint32_t p = impl.CompiledGraph.ExecutionOrder[order];
        auto& report = impl.Report.Passes[p];
        if (!report.Live) continue;
        auto& pass = impl.Passes[p];
        const auto& plan = impl.ExecutionPlan[p];
        render::CommandBuffer* dest = resolveCommand(plan, p);
        render::CommandBuffer* recording = dest;
        if (report.Type == RgPassType::Raster) {
            if (report.RasterGroup == p) {
                rasterCommand = dest;
                recording = dest;
            } else {
                recording = rasterCommand != nullptr ? rasterCommand : dest;
                if (dest != recording) {
                    impl.Error("PresentCommandSplit", "Raster group spans multiple presentation command buffers", p);
                }
            }
        } else {
            rasterCommand = nullptr;
        }
        RADRAY_PROFILE_SCOPE_DYN(report.Name);
        recording->PushDebugGroup(report.Name);
        result.CommandsRecorded = true;
        if (!plan.Barriers.empty()) {
            if (impl.Options.BatchBarriers) {
                recording->ResourceBarrier(plan.Barriers);
                ++impl.Report.BarrierBatches;
            } else
                for (const auto& barrier : plan.Barriers) {
                    recording->ResourceBarrier(std::span{&barrier, 1});
                    ++impl.Report.BarrierBatches;
                }
        }
        for (const auto& access : plan.Accesses) impl.Resources[access.Physical].States[access.Cell] = access.State;
        if (report.Type == RgPassType::Raster) {
            const auto depthClear = pass.DepthAttachment ? std::optional{pass.DepthAttachment->Desc.Clear} : std::nullopt;
            if (report.RasterGroup == p) rasterEncoder = recording->BeginRenderPass({pass.NativePass.Get(), pass.Framebuffer.Get(), pass.Clears,
                                                                                  depthClear, report.Name, pass.AllowUavWrites});
            if (!rasterEncoder) {
                impl.Error("BeginRenderPass", "Encoder creation failed after barriers; actual states are committed for host recovery", p);
                result.Success = false;
            } else {
                RenderGraphRasterContext context(*this, p, *rasterEncoder);
                if (pass.Data) pass.Data->Run(context);
                const bool last = order + 1 == impl.CompiledGraph.ExecutionOrder.size() || impl.Report.Passes[impl.CompiledGraph.ExecutionOrder[order + 1]].RasterGroup != report.RasterGroup;
                if (last || !impl.Report.Diagnostics.empty()) recording->EndRenderPass(rasterEncoder.Release());
            }
        } else if (report.Type == RgPassType::Compute) {
            auto encoder = recording->BeginComputePass();
            if (!encoder) {
                impl.Error("BeginComputePass", "Encoder creation failed after barriers; actual states are committed for host recovery", p);
                result.Success = false;
            } else {
                RenderGraphComputeContext context(*this, p, *encoder);
                if (pass.Data) pass.Data->Run(context);
                recording->EndComputePass(encoder.Release());
            }
        } else if (pass.CopyOp) {
            const auto& copy = *pass.CopyOp;
            auto& src = impl.Resources[copy.Source];
            auto& dst = impl.Resources[copy.Destination];
            if (copy.Type == Impl::CopyType::Buffer)
                recording->CopyBufferToBuffer(dst.NativeBuffer(), copy.DestinationOffset, src.NativeBuffer(), copy.SourceOffset, copy.Size);
            else if (copy.Type == Impl::CopyType::TextureToBuffer)
                recording->CopyTextureToBuffer(dst.NativeBuffer(), copy.DestinationOffset, src.NativeTexture(), copy.SourceRange);
            else if (copy.Type == Impl::CopyType::BufferToTexture) {
                if (!recording->CopyBufferToTextureRegion({src.NativeBuffer(), dst.NativeTexture(), copy.Upload})) {
                    impl.Error("CopyBufferTextureRegion", "Backend rejected the region upload", p);
                    result.Success = false;
                }
            } else if (copy.Type == Impl::CopyType::Resolve)
                recording->ResolveTexture({.Destination = dst.NativeTexture(),
                                        .DestinationMipLevel = copy.DestinationRange.BaseMipLevel,
                                        .DestinationArrayLayer = copy.DestinationRange.BaseArrayLayer,
                                        .Source = src.NativeTexture(),
                                        .SourceMipLevel = copy.SourceRange.BaseMipLevel,
                                        .SourceArrayLayer = copy.SourceRange.BaseArrayLayer,
                                        .ArrayLayerCount = copy.SourceRange.ArrayLayerCount});
            else
                recording->CopyTextureToTexture({.Destination = dst.NativeTexture(), .DestinationMipLevel = copy.DestinationRange.BaseMipLevel, .DestinationArrayLayer = copy.DestinationRange.BaseArrayLayer, .Source = src.NativeTexture(), .SourceMipLevel = copy.SourceRange.BaseMipLevel, .SourceArrayLayer = copy.SourceRange.BaseArrayLayer, .Width = std::max(1u, src.TextureDesc.Width >> copy.SourceRange.BaseMipLevel), .Height = std::max(1u, src.TextureDesc.Height >> copy.SourceRange.BaseMipLevel), .ArrayLayerCount = copy.SourceRange.ArrayLayerCount});
        }
        recording->PopDebugGroup();
        if (!impl.Report.Diagnostics.empty()) result.Success = false;
        if (!result.Success) {
            for (const auto& access : pass.Cells)
                if (access.Write) impl.Resources[access.Resource].Valid[access.Cell] = 0;
            break;
        }
        report.Executed = true;
        if (pass.Ticket._state) pass.Ticket._state->Record();
        for (const auto& access : pass.Cells)
            if (access.Write) {
                auto& resource = impl.Resources[access.Resource];
                resource.Valid[access.Cell] = access.ValidAfter ? 1 : 0;
                resource.Written = access.ValidAfter;
            }
    }
    result.Submission = make_shared<FrameSubmission>(impl.GenerationSerial);
    result.Submission->Record();
    auto submission = impl.DetachSubmissionResources();
    const auto serial = impl.GenerationSerial;
    result.Submission->OnSubmitted = [state = std::move(submission.State), retained = submission.Retained, serial] {
        state->Commit();
        retained->Submit(serial);
    };
    result.Submission->OnCompleted = [retained = std::move(submission.Retained), serial](bool success) { retained->Complete(serial, success); };
    }
    impl.Pool.EndGraph();
    impl.Report.Pool = impl.Pool.GetStats();
    PlotGraphCpuStats(impl.Report);
    return result;
}

std::optional<render::TextureStates> RenderGraph::RecordedTextureState(RgTextureValue handle, uint32_t subresource) const noexcept {
    const auto& impl = *_impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.Resources.size()) return {};
    if (impl.Resources[handle.Index].Port && handle.Version < impl.Resources[handle.Index].ResolvedValues.size()) handle.Index = impl.Resources[handle.Index].ResolvedValues[handle.Version].first;
    if (handle.Index >= impl.Resources.size()) return {};
    const auto& resource = impl.Resources[handle.Index];
    if (!resource.IsTexture || resource.Physical >= impl.Resources.size()) return {};
    const auto& states = impl.Resources[resource.Physical].States;
    if (subresource >= states.size()) return {};
    return static_cast<render::TextureState>(states[subresource]);
}
bool RenderGraph::WasWritten(RgTextureValue handle) const noexcept {
    return handle.Generation == _impl->Generation && handle.Index < _impl->Resources.size() && _impl->Resources[handle.Index].IsTexture && _impl->Resources[handle.Index].Written;
}
bool RenderGraph::WasWritten(const RenderExternalTexture& texture) const noexcept {
    for (const auto& resource : _impl->Resources)
        if (resource.ExternalTexture && resource.ExternalTexture->Texture == texture.Texture) return resource.Written;
    return false;
}
std::optional<render::TextureDescriptor> RenderGraph::GetTextureDescriptor(RgTextureValue handle) const noexcept {
    if (handle.Generation != _impl->Generation || handle.Index >= _impl->Resources.size() || !_impl->Resources[handle.Index].IsTexture) return {};
    return _impl->Resources[handle.Index].TextureDesc;
}
std::optional<RgTextureParameterBinding> RenderGraph::GetTextureViewBinding(RgTextureViewHandle handle) const noexcept {
    if (handle.Generation != _impl->Generation || handle.Index >= _impl->Views.size()) return {};
    const auto& view = _impl->Views[handle.Index];
    return RgTextureParameterBinding{{view.Resource, _impl->Generation, view.Version}, {view.Key.Dimension, view.Key.Format, view.Key.Range}};
}
void RenderGraph::AddDiagnostic(std::string_view code, std::string_view message) {
    _impl->Error(code, message);
}
void RenderGraphRasterContext::Fail(std::string_view message) {
    _graph._impl->Error("RasterExecution", message, _pass);
}
render::TextureView* RenderGraph::ResolveView(uint32_t pass, RgTextureViewHandle handle) const {
    const auto& impl = *_impl;
    const auto& declared = impl.Passes[pass].DeclaredViews;
    if (handle.Generation != impl.Generation || std::find(declared.begin(), declared.end(), handle.Index) == declared.end() || !impl.Views[handle.Index].Native) RADRAY_ABORT("RenderGraph texture view was not declared by this pass");
    return impl.Views[handle.Index].Native.Get();
}
render::Buffer* RenderGraph::ResolveBuffer(uint32_t pass, RgBufferValue handle) const {
    const auto& impl = *_impl;
    const auto& declared = impl.Passes[pass].DeclaredBuffers;
    if (handle.Generation == impl.Generation && handle.Index < impl.Resources.size() && impl.Resources[handle.Index].Port && handle.Version < impl.Resources[handle.Index].ResolvedValues.size()) {
        const auto value = impl.Resources[handle.Index].ResolvedValues[handle.Version];
        handle.Index = value.first;
        handle.Version = value.second;
    }
    if (handle.Generation != impl.Generation || std::find(declared.begin(), declared.end(), handle.Index) == declared.end()) RADRAY_ABORT("RenderGraph buffer was not declared by this pass");
    return impl.Resources[handle.Index].NativeBuffer();
}
void RenderGraph::ExecuteIndirect(
    uint32_t pass, RgIndirectArgumentsHandle handle, RgIndirectCommand expected,
    render::GraphicsCommandEncoder* graphics, render::ComputeCommandEncoder* compute) noexcept {
    const auto& impl = *_impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.IndirectArgumentsRecords.size()) {
        RADRAY_ABORT("RenderGraph indirect arguments handle belongs to another graph or is invalid");
    }
    const Impl::IndirectArguments& arguments = impl.IndirectArgumentsRecords[handle.Index];
    if (arguments.Pass != pass || arguments.Command != expected) {
        RADRAY_ABORT("RenderGraph indirect arguments handle was not declared by this pass or has the wrong command kind");
    }
    render::Buffer* buffer = impl.Resources[arguments.Resource].NativeBuffer();
    switch (expected) {
        case RgIndirectCommand::Draw:
            if (graphics == nullptr || compute != nullptr) RADRAY_ABORT("DrawIndirect requires a raster pass encoder");
            graphics->DrawIndirect(buffer, arguments.Offset, arguments.Count);
            return;
        case RgIndirectCommand::DrawIndexed:
            if (graphics == nullptr || compute != nullptr) RADRAY_ABORT("DrawIndexedIndirect requires a raster pass encoder");
            graphics->DrawIndexedIndirect(buffer, arguments.Offset, arguments.Count);
            return;
        case RgIndirectCommand::Dispatch:
            if (compute == nullptr || graphics != nullptr) RADRAY_ABORT("DispatchIndirect requires a compute pass encoder");
            if (arguments.Count != 0) compute->DispatchIndirect(buffer, arguments.Offset);
            return;
    }
    RADRAY_ABORT("RenderGraph indirect command kind is invalid");
}

void RenderGraph::BindParameterSet(
    uint32_t pass, RgParameterSetHandle handle,
    render::GraphicsCommandEncoder* graphics, render::ComputeCommandEncoder* compute) noexcept {
    const auto& impl = *_impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.ParameterSets.size()) {
        RADRAY_ABORT("RenderGraph parameter-set handle belongs to another graph or is invalid");
    }
    const Impl::ParameterSet& parameterSet = impl.ParameterSets[handle.Index];
    if (parameterSet.Pass != pass || !parameterSet.Native) {
        RADRAY_ABORT("RenderGraph parameter set was not declared and prepared for this pass");
    }
    if (graphics != nullptr && compute == nullptr) {
        graphics->BindShaderParameterSet(parameterSet.Group, parameterSet.Native.Get(),
                                         parameterSet.DynamicOffsets);
    } else if (compute != nullptr && graphics == nullptr) {
        compute->BindShaderParameterSet(parameterSet.Group, parameterSet.Native.Get(),
                                        parameterSet.DynamicOffsets);
    } else {
        RADRAY_ABORT("RenderGraph parameter set requires exactly one pass encoder");
    }
}

void RenderGraph::BindComputeProgram(
    uint32_t pass, RgComputeProgramHandle handle,
    render::ComputeCommandEncoder& encoder) noexcept {
    const auto& impl = *_impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.ComputePrograms.size()) {
        RADRAY_ABORT("RenderGraph compute-program handle belongs to another graph or is invalid");
    }
    const Impl::ComputeProgram& program = impl.ComputePrograms[handle.Index];
    if (program.Pass != pass || !program.PipelineState) {
        RADRAY_ABORT("RenderGraph compute program was not declared and prepared for this pass");
    }
    encoder.BindComputePipelineState(program.PipelineState.Get());
}

void RenderGraph::BindGraphicsProgram(uint32_t pass, RgGraphicsProgramHandle handle, render::GraphicsCommandEncoder& encoder) noexcept {
    const auto& impl = *_impl;
    if (handle.Generation != impl.Generation || handle.Index >= impl.GraphicsPrograms.size())
        RADRAY_ABORT("RenderGraph graphics program belongs to another graph or is invalid");
    const auto& program = impl.GraphicsPrograms[handle.Index];
    if (program.Pass != pass || !program.PipelineState)
        RADRAY_ABORT("RenderGraph graphics program was not declared and prepared for this pass");
    encoder.BindGraphicsPipelineState(program.PipelineState.Get());
}

bool RenderGraph::ValidateNativeBuffer(uint32_t pass, render::Buffer* buffer, RgBufferAccess access) noexcept {
    auto& impl = *_impl;
    if (!buffer) {
        impl.Error("GeometryBuffer", "Geometry binding requires a non-null buffer", pass);
        return false;
    }
    const auto tracked = impl.NativeBuffers.find(buffer);
    if (tracked == impl.NativeBuffers.end()) return true;
    const auto& reads = impl.Passes[pass].BufferReadStates;
    const auto declared = reads.find(buffer);
    const uint32_t required = static_cast<uint32_t>(BufferAccessInfo(access).first);
    if (declared == reads.end() || (declared->second & required) != required) {
        impl.Error("UndeclaredGeometryRead", "Graph geometry requires a matching Vertex or Index read declaration in this pass",
                   pass, tracked->second);
        return false;
    }
    return true;
}

void RenderGraphGraphicsCommands::BindVertexBuffers(std::span<const render::VertexBufferBinding> bindings) noexcept {
    for (const auto& binding : bindings) {
        render::Buffer* target = binding.View.Target;
        if (target != nullptr && std::find(_validatedVertex.begin(), _validatedVertex.end(), target) != _validatedVertex.end()) continue;
        if (!_graph.ValidateNativeBuffer(_pass, target, RgBufferAccess::Vertex)) {
            _valid = false;
            continue;
        }
        std::rotate(_validatedVertex.rbegin(), _validatedVertex.rbegin() + 1, _validatedVertex.rend());
        _validatedVertex.front() = target;
    }
    if (_valid) _encoder.BindVertexBuffers(bindings);
}
void RenderGraphGraphicsCommands::BindIndexBuffer(render::IndexBufferView view) noexcept {
    if (view.Target == nullptr || view.Target != _validatedIndex) {
        if (!_graph.ValidateNativeBuffer(_pass, view.Target, RgBufferAccess::Index)) _valid = false;
        else _validatedIndex = view.Target;
    }
    if (_valid) _encoder.BindIndexBuffer(view);
}

void RenderGraphGraphicsCommands::DrawIndirect(RgIndirectArgumentsHandle arguments) noexcept {
    if (_valid) _graph.ExecuteIndirect(_pass, arguments, RgIndirectCommand::Draw, &_encoder, nullptr);
}
void RenderGraphGraphicsCommands::DrawIndexedIndirect(RgIndirectArgumentsHandle arguments) noexcept {
    if (_valid) _graph.ExecuteIndirect(_pass, arguments, RgIndirectCommand::DrawIndexed, &_encoder, nullptr);
}
void RenderGraphComputeCommands::DispatchIndirect(RgIndirectArgumentsHandle arguments) noexcept {
    _graph.ExecuteIndirect(_pass, arguments, RgIndirectCommand::Dispatch, nullptr, &_encoder);
}
render::TextureView* RenderGraphRasterContext::GetTextureView(RgTextureViewHandle handle) const { return _graph.ResolveView(_pass, handle); }
render::Buffer* RenderGraphRasterContext::GetBuffer(RgBufferValue handle) const { return _graph.ResolveBuffer(_pass, handle); }
void RenderGraphRasterContext::BindParameterSet(RgParameterSetHandle handle) noexcept {
    _graph.BindParameterSet(_pass, handle, &_encoder._encoder, nullptr);
}
void RenderGraphRasterContext::BindGraphicsProgram(RgGraphicsProgramHandle handle) noexcept {
    _graph.BindGraphicsProgram(_pass, handle, _encoder._encoder);
}
const GraphicsPassState& RenderGraphRasterContext::PassState() const noexcept { return *_graph._impl->Passes[_pass].PassState; }
render::TextureView* RenderGraphComputeContext::GetTextureView(RgTextureViewHandle handle) const { return _graph.ResolveView(_pass, handle); }
render::Buffer* RenderGraphComputeContext::GetBuffer(RgBufferValue handle) const { return _graph.ResolveBuffer(_pass, handle); }
void RenderGraphComputeContext::BindParameterSet(RgParameterSetHandle handle) noexcept {
    _graph.BindParameterSet(_pass, handle, nullptr, &_encoder._encoder);
}
void RenderGraphComputeContext::BindComputeProgram(RgComputeProgramHandle handle) noexcept {
    _graph.BindComputeProgram(_pass, handle, _encoder._encoder);
}

}  // namespace radray
