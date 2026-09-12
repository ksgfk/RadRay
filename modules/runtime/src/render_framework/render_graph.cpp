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
#include <radray/scope_guard.h>
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

// Rebuilt from live declarations before preparation; lookup also enforces pass access rights in
// Off mode. Buckets belong to the flight workspace and retain their high-water storage.
struct ReadyNativeAccess {
    struct Entry {
        uint64_t Key{UINT64_MAX};
        Nullable<void*> Native{nullptr};
    };
    vector<Entry> Entries;
    void Reset(size_t count) {
        size_t capacity = 2;
        while (capacity < count * 2) capacity *= 2;
        if (Entries.size() < capacity) Entries.resize(capacity);
        std::fill(Entries.begin(), Entries.end(), Entry{});
    }
    static uint64_t Key(uint32_t index, bool texture) { return (uint64_t{index} << 1) | uint64_t{texture}; }
    size_t Bucket(uint64_t key) const { return static_cast<size_t>((key * 11400714819323198485ull) >> 32) & (Entries.size() - 1); }
    void Insert(uint32_t index, bool texture, void* native) {
        const uint64_t key = Key(index, texture);
        size_t bucket = Bucket(key);
        while (Entries[bucket].Key != UINT64_MAX && Entries[bucket].Key != key) bucket = (bucket + 1) & (Entries.size() - 1);
        Entries[bucket] = {key, native};
    }
    Nullable<void*> Find(uint32_t index, bool texture) const {
        if (Entries.empty()) return nullptr;
        const uint64_t key = Key(index, texture);
        size_t bucket = Bucket(key);
        while (Entries[bucket].Key != UINT64_MAX) {
            if (Entries[bucket].Key == key) return Entries[bucket].Native;
            bucket = (bucket + 1) & (Entries.size() - 1);
        }
        return nullptr;
    }
};

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

struct RenderGraphPlanCache::Impl {
    struct Entry {
        vector<uint64_t> Key;
        size_t Hash{0};
        uint64_t Used{0};
        shared_ptr<const void> Plan;
    };
    size_t Capacity;
    uint64_t UseSerial{0};
    vector<Entry> Entries;
};

RenderGraphPlanCache::RenderGraphPlanCache(size_t capacity) : _impl(make_unique<Impl>()) {
    _impl->Capacity = std::max(size_t{2}, capacity);
    _impl->Entries.reserve(_impl->Capacity);
}
RenderGraphPlanCache::~RenderGraphPlanCache() noexcept = default;
size_t RenderGraphPlanCache::Size() const noexcept { return _impl->Entries.size(); }
void RenderGraphPlanCache::Clear() noexcept {
    _impl->Entries.clear();
    _compositionCache.reset();
}

struct RenderGraphFrameResources::Impl {
    // Keyed by the layout's binding record, not by its declaration name: a per-frame key must not
    // copy strings. BindingHandle exposes only equality, so the hash below covers the value alone
    // and lets equality separate two records that share it.
    struct ParameterValue {
        render::BindingHandle Handle;
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

    Impl(render::Device& device, render::RenderPassRegistry& registry, shared_ptr<RenderGraphPlanCache> plans)
        : Device(device), Pool(device, registry), Plans(std::move(plans)) {}

    render::Device& Device;
    RenderResourcePool Pool;
    Nullable<HostWriteBatch*> HostWrites{nullptr};
    unique_ptr<DynamicCBufferArena> Arena;
    // Sets are released before the upload pages and pooled resources they reference.
    vector<unique_ptr<render::ShaderParameterSet>> Sets;
    unordered_map<ParameterSetKey, render::ShaderParameterSet*, ParameterSetKeyHash> SetCache;
    // Scratch for the key under construction, reused by every set built in a frame.
    ParameterSetKey KeyScratch;
    vector<ReadyNativeAccess> NativeAccessTables;
    GraphCompileWorkspace CompileWorkspace;
    shared_ptr<RenderGraphPlanCache> Plans;
};

RenderGraphFrameResources::RenderGraphFrameResources(
    render::Device& device, render::RenderPassRegistry& registry)
    : RenderGraphFrameResources(device, registry, make_shared<RenderGraphPlanCache>()) {}

RenderGraphFrameResources::RenderGraphFrameResources(
    render::Device& device, render::RenderPassRegistry& registry, shared_ptr<RenderGraphPlanCache> plans)
    : _impl(make_unique<Impl>(device, registry, std::move(plans))) {}

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
shared_ptr<FrameGraphTemplateCache>& RenderGraphFrameResources::DefaultCompositionCacheStorage() noexcept {
    return _impl->Plans->_compositionCache;
}
const RenderResourcePoolStats& RenderGraphFrameResources::GetPoolStats() const noexcept { return _impl->Pool.GetStats(); }
size_t RenderGraphFrameResources::GetParameterSetCount() const noexcept { return _impl->Sets.size(); }

void RenderGraphFrameResources::Clear() {
    _impl->SetCache.clear();
    _impl->Sets.clear();
    if (_impl->Arena) _impl->Arena->Clear();
    _impl->Pool.Clear();
    _impl->CompileWorkspace = {};
    _impl->NativeAccessTables.clear();
    _impl->Plans->Clear();
}

struct RenderGraph::Impl {
    struct CompiledFramePlan;
    template <class T>
    struct DeclarationStorage {
        std::optional<T> Owned;
        Nullable<const T*> Borrowed{nullptr};
        DeclarationStorage() : Owned(T{}) {}
        explicit DeclarationStorage(const T& declaration) : Borrowed(&declaration) {}
        const T& Get() const noexcept { return Borrowed ? *Borrowed : *Owned; }
        T& Edit() noexcept {
            RADRAY_ASSERT(Owned.has_value() && !Borrowed);
            return *Owned;
        }
        void Materialize() {
            if (!Borrowed) return;
            Owned = *Borrowed;
            Borrowed = nullptr;
        }
    };
    struct ResourceDeclaration {
        string Name;
        std::source_location Location;
        bool IsTexture{false};
        vector<uint32_t> VersionParents{InvalidIndex, 0};
        vector<uint64_t> BufferBoundaries;
        bool Port{false};
        bool ExternalSlot{false};
        uint32_t Connection{InvalidIndex}, ConnectionVersion{0};
        vector<std::pair<uint32_t, uint32_t>> ResolvedValues;
        render::TextureDescriptor TextureDesc;
        render::BufferDescriptor BufferDesc;
        RenderGraphExternalAccess ExternalAccess{RenderGraphExternalAccess::ReadWrite};
        uint64_t ViewId{0};
    };
    struct Resource {
        DeclarationStorage<ResourceDeclaration> Declaration;
        Resource() = default;
        explicit Resource(const ResourceDeclaration& declaration) : Declaration(declaration) {}
        const ResourceDeclaration& Def() const noexcept { return Declaration.Get(); }
        ResourceDeclaration& Edit() noexcept { return Declaration.Edit(); }
        uint32_t TemplateInstance{InvalidIndex};
        vector<uint32_t> VersionTail;
        std::optional<std::pair<uint32_t, uint32_t>> ConnectionPatch;
        std::optional<RenderGraphExternalAccess> ExternalAccessOverride;
        bool ExternalBound{false};
        RenderGraphExternalAccess AccessMode() const noexcept { return ExternalAccessOverride.value_or(Def().ExternalAccess); }
        uint32_t VersionCount() const noexcept { return static_cast<uint32_t>(Def().VersionParents.size() + VersionTail.size()); }
        std::string_view Name() const noexcept { return Declaration.Owned ? Declaration.Owned->Name : Def().Name; }
        std::source_location Location() const noexcept { return Declaration.Owned ? Declaration.Owned->Location : Def().Location; }
        uint64_t ViewId() const noexcept { return Declaration.Owned ? Declaration.Owned->ViewId : Def().ViewId; }
        uint32_t Physical{InvalidIndex};
        uint32_t PlannedCellCount{0};
        Nullable<RenderExternalTexture*> ExternalTexture{nullptr};
        Nullable<RenderExternalBuffer*> ExternalBuffer{nullptr};
        vector<RenderExternalTexture*> TextureImports;
        vector<RenderExternalBuffer*> BufferImports;
        Nullable<PooledTexture*> PoolTexture{nullptr};
        Nullable<PooledBuffer*> PoolBuffer{nullptr};
        shared_ptr<RgReadbackTicket::Storage> Readback;
        vector<uint32_t> States;
        vector<uint8_t> Valid;
        bool Written{false};
        int32_t FirstUse{-1};
        int32_t LastUse{-1};
        uint32_t SubresourceCount() const { return Def().TextureDesc.MipLevels * (Def().TextureDesc.Dim == render::TextureDimension::Dim3D ? 1 : Def().TextureDesc.DepthOrArraySize); }
        uint32_t AspectCount() const { return render::GetTextureFormatAspects(Def().TextureDesc.Format).HasFlag(render::TextureAspect::Stencil) ? 2u : 1u; }
        uint32_t PhysicalCell(uint32_t cell) const { return Def().IsTexture ? cell % SubresourceCount() : 0; }
        uint32_t CellCount() const { return PlannedCellCount ? PlannedCellCount : Def().IsTexture ? SubresourceCount() * AspectCount()
                                                                                                  : static_cast<uint32_t>(Def().BufferBoundaries.size() > 1 ? Def().BufferBoundaries.size() - 1 : 1); }
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
        uint32_t TemplateInstance{InvalidIndex};
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
    enum class BarrierKind : uint8_t { InitialState,
                                       Transition,
                                       Uav };
    struct BarrierTemplate {
        BarrierKind Kind;
        uint32_t Resource, Physical, Cell, Before, After;
        render::ShaderStages BeforeStages, AfterStages;
    };
    struct PassExecutionPlan {
        vector<PhysicalAccess> Accesses;
        Nullable<const vector<PhysicalAccess>*> CachedAccesses{nullptr};
        vector<BarrierTemplate> BarrierTemplates;
        vector<uint32_t> RouteResources;
        vector<render::ResourceBarrierDescriptor> Barriers;
        Nullable<render::CommandBuffer*> Commands{nullptr};
        std::span<const PhysicalAccess> GetAccesses() const { return CachedAccesses ? std::span<const PhysicalAccess>{*CachedAccesses} : std::span<const PhysicalAccess>{Accesses}; }
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
        shared_ptr<const CompiledFramePlan> Plan;
        vector<shared_ptr<void>> Owners;
        vector<shared_ptr<RgReadbackTicket::Storage>> Readbacks;
        // Generic payloads may own GPU resources, so their lifetime still extends to completion.
        vector<unique_ptr<Payload>> Payloads;
        vector<unique_ptr<WorkPayload>> WorkPayloads;
        vector<shared_ptr<FrameSubmission>> Tickets;

        void Submit(uint64_t serial) {
            for (const auto& ticket : Tickets) {
                if (ticket->Status() == FrameOperationStatus::Recorded)
                    ticket->Submit(serial);
                else
                    ticket->Cancel();
            }
        }
        void Complete(uint64_t serial, bool success) {
            for (const auto& ticket : Tickets) {
                if (ticket->Status() == FrameOperationStatus::Submitted)
                    ticket->Complete(serial, success);
                else
                    ticket->Cancel();
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
        uint32_t TemplateInstance{InvalidIndex};
    };
    struct CompiledPassData {
        vector<Access> Accesses;
        vector<CellAccess> Cells;
        vector<PhysicalAccess> PhysicalAccesses;
        vector<BarrierTemplate> BarrierTemplates;
        vector<uint32_t> RouteResources;
        vector<render::StoreAction> ColorStores;
        std::optional<render::StoreAction> DepthStore;
        uint32_t RasterGroup{InvalidIndex}, MergeTail{InvalidIndex};
        uint32_t Width{0}, Height{0}, Layers{0}, Samples{0};
        render::ShaderStages UavWriteStages{render::ShaderStage::UNKNOWN};
        bool AllowUavWrites{false};
    };
    struct PassDeclaration {
        std::source_location Location;
        vector<Access> Accesses;
        vector<uint32_t> DeclaredViews, DeclaredBuffers;
        vector<std::optional<Color>> Colors;
        std::optional<Depth> DepthAttachment;
        std::optional<Copy> CopyOp;
        bool SideEffect{false};
        vector<std::pair<uint32_t, uint64_t>> WorkRequirements;
        string Name;
        RgPassType Type{RgPassType::Raster};
        shared_ptr<const TemplateFactory> Factory;
        uint32_t Slot{InvalidIndex};
        uint32_t UploadSlot{InvalidIndex};
        bool Tracked{false};
        render::ShaderStages UavWriteStages{render::ShaderStage::UNKNOWN};
        bool AllowUavWrites{false};
    };
    struct Pass {
        struct ReadyCheck {
            shared_ptr<const void> Payload;
            bool (*Validate)(const void*, RenderGraphPrepareContext&);
        };
        struct GeometryCheck {
            render::Buffer* Buffer;
            RgBufferAccess Access;
        };
        DeclarationStorage<PassDeclaration> Declaration;
        Pass() = default;
        explicit Pass(const PassDeclaration& declaration) : Declaration(declaration) {}
        const PassDeclaration& Def() const noexcept { return Declaration.Get(); }
        PassDeclaration& Edit() noexcept { return Declaration.Edit(); }
        uint32_t TemplateInstance{InvalidIndex};
        std::string_view Name() const noexcept { return Declaration.Owned ? Declaration.Owned->Name : Def().Name; }
        std::source_location Location() const noexcept { return Declaration.Owned ? Declaration.Owned->Location : Def().Location; }
        array<std::optional<render::ColorClearValue>, 8> ColorClearOverrides;
        std::optional<render::DepthStencilClearValue> DepthClearOverride;
        render::ColorClearValue ColorClear(size_t slot) const noexcept {
            if (ColorClearOverrides[slot]) return *ColorClearOverrides[slot];
            return Declaration.Owned ? Declaration.Owned->Colors[slot]->Desc.Clear : Def().Colors[slot]->Desc.Clear;
        }
        std::optional<render::DepthStencilClearValue> DepthClear() const noexcept {
            if (DepthClearOverride) return DepthClearOverride;
            const auto& depth = Declaration.Owned ? Declaration.Owned->DepthAttachment : Def().DepthAttachment;
            return depth ? std::optional{depth->Desc.Clear} : std::nullopt;
        }
        unique_ptr<Payload> Data;
        vector<ReadyCheck> ReadyChecks;
        vector<GeometryCheck> GeometryChecks;
        Nullable<const ReadyNativeAccess*> NativeAccess{nullptr};
        vector<CellAccess> Cells;
        unordered_map<render::Buffer*, uint32_t> GeometryReadStates;
        bool GeometryReadStatesBuilt{false};
        RenderGraphCommandCalls CommandCalls{};
        Nullable<const CompiledPassData*> CompiledData{nullptr};
        std::span<const Access> GetAccesses() const { return CompiledData ? std::span<const Access>{CompiledData->Accesses} : std::span<const Access>{Def().Accesses}; }
        std::span<const CellAccess> GetCells() const { return CompiledData ? std::span<const CellAccess>{CompiledData->Cells} : std::span<const CellAccess>{Cells}; }
        render::StoreAction ColorStore(size_t slot) const { return CompiledData ? CompiledData->ColorStores[slot] : Def().Colors[slot]->Desc.Store; }
        render::StoreAction DepthStore() const { return CompiledData ? *CompiledData->DepthStore : Def().DepthAttachment->Desc.Store; }
        uint32_t MergeTail{InvalidIndex};
        Nullable<render::RenderPass*> NativePass{nullptr};
        Nullable<render::Framebuffer*> Framebuffer{nullptr};
        std::optional<GraphicsPassState> PassState;
        vector<render::ColorClearValue> Clears;
        uint32_t Width{0}, Height{0}, Layers{0}, Samples{0};
        render::ShaderStages UavWriteStages{render::ShaderStage::UNKNOWN};
        bool AllowUavWrites{false};
        vector<byte> UploadBytes;
        Nullable<const RgUploadData*> UploadSource{nullptr};
        RgOperationTicket Ticket;
        bool Live{false};
        bool Executed{false};
        uint32_t RasterGroup{InvalidIndex};
    };
    struct CompiledFramePlan {
        struct ResourceLayout {
            uint32_t Physical, CellCount;
            int32_t FirstUse, LastUse;
        };
        uint64_t Id{0};
        CompiledRenderGraph Graph;
        vector<CompiledPassData> Passes;
        vector<ResourceLayout> Resources;
        vector<std::pair<uint32_t, uint64_t>> LiveWork;
        vector<ResourceDeclaration> ResourceDeclarations;
        vector<PassDeclaration> PassDeclarations;
        vector<View> Views;
        vector<IndirectArguments> IndirectArgumentsRecords;
        uint32_t ReusedResources{0}, MergedRasterPasses{0}, DiscardedStores{0};
    };
    render::Device& Device;
    RenderResourcePool& Pool;
    render::RenderPassRegistry& Registry;
    Nullable<RenderGraphFrameResources*> FrameResources{nullptr};
    uint64_t Generation;
    uint64_t GenerationSerial{0};
    uint64_t ResourceView{0};
    bool Frozen{false}, Compiled{false}, Executed{false}, Failed{false}, PreparingWork{false}, ValidatingReady{false};
    RenderGraphRuntimeOptions Runtime{kDiagnosticRenderGraphRuntimeOptions};
    string FirstErrorCode;
    vector<Resource> Resources;
    vector<View> Views;
    vector<Pass> Passes;
    struct WorkDeclaration {
        string Name;
        shared_ptr<const TemplateWorkFactory> Factory;
        uint32_t Slot{InvalidIndex};
    };
    struct TemplatePlacement {
        uint32_t ResourceBase{0}, ViewBase{0}, PassBase{0}, IndirectBase{0}, WorkBase{0};
        vector<ResourceDeclaration> Resources;
        vector<PassDeclaration> Passes;
        vector<View> Views;
        vector<IndirectArguments> Indirect;
        vector<WorkDeclaration> Works;
    };
    struct TemplateInstanceData {
        shared_ptr<const RenderGraphTemplate> Source;
        shared_ptr<const TemplatePlacement> Placement;
        vector<shared_ptr<void>> Slots;
    };
    vector<const void*> TemplateSlotTypes;
    vector<TemplateInstanceData> Templates;
    struct Work {
        DeclarationStorage<WorkDeclaration> Declaration;
        Work() = default;
        explicit Work(const WorkDeclaration& declaration) : Declaration(declaration) {}
        uint32_t TemplateInstance{InvalidIndex};
        unique_ptr<WorkPayload> Data;
    };
    vector<Work> Works;
    vector<std::pair<uint32_t, uint64_t>> LiveWork;
    vector<IndirectArguments> IndirectArgumentsRecords;
    unordered_map<render::Buffer*, uint32_t> NativeBuffers;
    vector<ReadyNativeAccess> NativeAccessTables;
    struct ParameterUse {
        string Declaration;
        uint32_t Resource{InvalidIndex}, Version{0}, View{InvalidIndex}, State{0};
        render::BufferRange Bytes{};
        render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
        render::TextureViewUsage ViewUsage{render::TextureViewUsage::Resource};
        bool Read{false};
    };
    struct ParameterRequest {
        Nullable<ShaderProgram*> Program{nullptr};
        uint32_t Pass{InvalidIndex};
        RenderGraphFrameResources::Impl::ParameterSetKey Key;
        vector<ParameterUse> Uses;
    };
    struct PendingParameterSet {
        unique_ptr<render::ShaderParameterSet> Set;
        uint32_t Pass;
    };
    vector<ParameterRequest> ParameterRequests;
    unordered_map<RenderGraphFrameResources::Impl::ParameterSetKey, PendingParameterSet,
                  RenderGraphFrameResources::Impl::ParameterSetKeyHash>
        PendingParameterSets;
    vector<shared_ptr<void>> Owners;
    RenderGraphCompileOptions Options;
    CompiledRenderGraph CompiledGraph;
    shared_ptr<const CompiledFramePlan> FramePlan;
    vector<PassExecutionPlan> ExecutionPlan;
    const CompiledRenderGraph& GetCompiled() const { return FramePlan ? FramePlan->Graph : CompiledGraph; }
    RenderGraphExecutionReport OwnedReport;
    RenderGraphExecutionReport& Report;

    Impl(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry,
         Nullable<RenderGraphFrameResources*> frameResources, std::string_view name, Nullable<RenderGraphExecutionReport*> report = nullptr,
         RenderGraphRuntimeOptions runtime = kDiagnosticRenderGraphRuntimeOptions)
        : Device(device), Pool(pool), Registry(registry), FrameResources(frameResources), Generation(NextGraphGeneration.fetch_add(1, std::memory_order_relaxed)),
          Runtime(runtime), Report(report ? *report : OwnedReport) {
        if (Generation == 0 || Generation == UINT64_MAX) RADRAY_ABORT("RenderGraph generation exhausted");
        GenerationSerial = Pool.GetFrameSerial();
        Report.Name = name;
    }
    bool ValidationFull() const noexcept { return IsRenderValidationFull(Runtime.Validation); }
    bool ReportFull() const noexcept { return IsRenderGraphReportFull(Runtime.Report); }
    void Error(std::string_view code, std::string_view message, uint32_t pass = InvalidIndex,
               uint32_t resource = InvalidIndex, std::string_view binding = {}) {
        Failed = true;
        if (FirstErrorCode.empty()) FirstErrorCode = string{code};
        Report.FirstErrorCode = FirstErrorCode;
        if (!ReportFull()) return;
        auto location = pass < Passes.size() ? Passes[pass].Location() : resource < Resources.size() ? Resources[resource].Location()
                                                                                                     : std::source_location::current();
        string detail{message};
        if (resource < Resources.size()) {
            const auto& entry = Resources[resource];
            if (entry.Def().IsTexture) {
                const auto& desc = entry.Def().TextureDesc;
                detail += fmt::format(" [dimension={}, {}x{}x{}, mips={}, samples={}, format={}, usage={}]", EnumName(desc.Dim), desc.Width, desc.Height,
                                      desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, EnumName(desc.Format), desc.Usage.value());
            } else
                detail += fmt::format(" [buffer size={}, memory={}, usage={}]", entry.Def().BufferDesc.Size, EnumName(entry.Def().BufferDesc.Memory), entry.Def().BufferDesc.Usage.value());
        }
        Report.Diagnostics.push_back({string{code}, Report.Name, pass < Passes.size() ? string{Passes[pass].Name()} : string{}, string{binding},
                                      resource < Resources.size() ? string{Resources[resource].Name()} : string{}, std::move(detail), string{location.file_name()}, location.line()});
    }
    bool Mutable() {
        if (!Frozen) return true;
        Error("GraphFrozen", "Setup cannot change a compiled or executed graph");
        return false;
    }
    bool Handle(uint32_t index, uint64_t generation, bool texture, uint32_t pass = InvalidIndex) {
        if (generation != Generation || index >= Resources.size() || Resources[index].Def().IsTexture != texture) {
            Error("InvalidHandle", fmt::format("Handle generation {} does not belong to graph generation {}, or its type/index is invalid", generation, Generation), pass);
            return false;
        }
        return true;
    }
    SubmissionData DetachSubmissionResources();
    bool ValidateResources();
    bool ValidatePlanInput();
    bool ValidateParameterRequests();
    bool PublishParameterSets();
    bool ResolvePorts();
    void MaterializeTemplates();
    bool InstantiateTemplatePayloads();
    RgTextureViewHandle PatchTemplateView(uint32_t pass, RgTextureViewHandle value) const;
    RgBufferValue PatchTemplateBuffer(uint32_t pass, RgBufferValue value) const;
    RgIndirectArgumentsHandle PatchTemplateIndirect(uint32_t pass, RgIndirectArgumentsHandle value) const;
    bool NormalizePasses();
    vector<uint64_t> BuildPlanKey() const;
    bool FindFramePlan(std::span<const uint64_t> key);
    void SaveFramePlan(vector<uint64_t> key);
    void ApplyFramePlan();
    void ApplyCompiledReport();
    void Cull();
    void PlanStorage();
    void BuildExecutionPlan();
    void BuildBarrierTemplates();
    bool Realize();
    void PatchBarriers();
    bool PatchCommandRoutes(render::CommandBuffer& command, std::span<const PresentCommandTarget> presentTargets);
    void OptimizeRaster();
};

struct RenderGraphTemplate::Impl {
    render::Device* Device;
    uint64_t Generation;
    vector<RenderGraph::Impl::ResourceDeclaration> Resources;
    vector<RenderGraph::Impl::PassDeclaration> Passes;
    vector<RenderGraph::Impl::View> Views;
    vector<RenderGraph::Impl::IndirectArguments> Indirect;
    vector<RenderGraph::Impl::WorkDeclaration> Works;
    vector<const void*> SlotTypes;
    struct PlacementEntry {
        shared_ptr<const RenderGraph::Impl::TemplatePlacement> Value;
        uint64_t Used{0};
    };
    mutable vector<PlacementEntry> Placements;
    mutable uint64_t UseSerial{0};
};

RenderGraphTemplate::RenderGraphTemplate(unique_ptr<Impl> impl) : _impl(std::move(impl)) {}
RenderGraphTemplate::~RenderGraphTemplate() = default;
uint64_t RenderGraphTemplate::GetGeneration() const noexcept { return _impl->Generation; }

uint32_t RenderGraph::AddTemplateSlot(const void* type) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return InvalidIndex;
    const auto slot = static_cast<uint32_t>(impl.TemplateSlotTypes.size());
    impl.TemplateSlotTypes.push_back(type);
    return slot;
}

void RenderGraph::SetTemplateFactory(RgPassHandle pass, uint32_t slot, uint64_t generation, const void* type, shared_ptr<const TemplateFactory> factory) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return;
    if (pass.Generation != impl.Generation || pass.Index >= impl.Passes.size() || generation != impl.Generation ||
        slot >= impl.TemplateSlotTypes.size() || impl.TemplateSlotTypes[slot] != type) {
        impl.Error("TemplateSlot", "Template pass requires a slot with the matching type and builder generation");
        return;
    }
    auto& declaration = impl.Passes[pass.Index].Edit();
    declaration.Factory = std::move(factory);
    declaration.Slot = slot;
}

RenderGraph RenderGraph::CreateTemplateBuilder(std::string_view name) const {
    return RenderGraph{_impl->Device, _impl->Pool, _impl->Registry, name, _impl->Runtime};
}
shared_ptr<const RenderGraphTemplate> RenderGraph::FreezeTemplate() {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    if (!impl.Templates.empty() || !impl.Owners.empty()) {
        impl.Error("TemplateCapture", "A template cannot capture instances, frame owners or legacy work payloads");
        return {};
    }
    for (const auto& resource : impl.Resources)
        if (resource.External() || resource.Readback) impl.Error("TemplateCapture", "Native resources and readback tickets belong to frame instances");
    for (const auto& pass : impl.Passes)
        if (pass.Data || !pass.UploadBytes.empty() || pass.UploadSource) impl.Error("TemplateCapture", "Use typed template callbacks instead of captured legacy payloads or immediate upload bytes");
    for (const auto& work : impl.Works)
        if (work.Data || !work.Declaration.Get().Factory) impl.Error("TemplateCapture", "Use typed work slots instead of capturing a frame work payload");
    if (impl.Failed) return {};
    impl.Frozen = true;
    auto result = make_unique<RenderGraphTemplate::Impl>();
    result->Device = &impl.Device;
    result->Generation = impl.Generation;
    result->Resources.reserve(impl.Resources.size());
    for (auto& resource : impl.Resources) result->Resources.push_back(std::move(resource.Edit()));
    result->Passes.reserve(impl.Passes.size());
    for (auto& pass : impl.Passes) {
        pass.Edit().Tracked = bool(pass.Ticket._state);
        pass.Edit().UavWriteStages = pass.UavWriteStages;
        pass.Edit().AllowUavWrites = pass.AllowUavWrites;
        result->Passes.push_back(std::move(pass.Edit()));
    }
    result->Views = std::move(impl.Views);
    result->Indirect = std::move(impl.IndirectArgumentsRecords);
    result->SlotTypes = std::move(impl.TemplateSlotTypes);
    result->Works.reserve(impl.Works.size());
    for (auto& work : impl.Works) result->Works.push_back(std::move(work.Declaration.Edit()));
    return shared_ptr<const RenderGraphTemplate>{new RenderGraphTemplate(std::move(result))};
}

RenderGraphTemplateInstance RenderGraph::Instantiate(shared_ptr<const RenderGraphTemplate> graphTemplate) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !graphTemplate) return {};
    const auto& source = *graphTemplate->_impl;
    if (source.Device != &impl.Device) {
        impl.Error("TemplateDevice", "A graph template belongs to its declaring device");
        return {};
    }
    const uint32_t resourceBase = static_cast<uint32_t>(impl.Resources.size());
    const uint32_t viewBase = static_cast<uint32_t>(impl.Views.size());
    const uint32_t passBase = static_cast<uint32_t>(impl.Passes.size());
    const uint32_t indirectBase = static_cast<uint32_t>(impl.IndirectArgumentsRecords.size());
    const uint32_t workBase = static_cast<uint32_t>(impl.Works.size());
    shared_ptr<const Impl::TemplatePlacement> placement;
    for (auto& entry : source.Placements) {
        const auto& candidate = *entry.Value;
        if (candidate.ResourceBase == resourceBase && candidate.ViewBase == viewBase && candidate.PassBase == passBase && candidate.IndirectBase == indirectBase && candidate.WorkBase == workBase) {
            placement = entry.Value;
            entry.Used = ++source.UseSerial;
            break;
        }
    }
    if (!placement) {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PlaceTemplate");
        ++impl.Report.TemplatePlacementBuilds;
        auto placed = make_shared<Impl::TemplatePlacement>();
        placed->ResourceBase = resourceBase;
        placed->ViewBase = viewBase;
        placed->PassBase = passBase;
        placed->IndirectBase = indirectBase;
        placed->WorkBase = workBase;
        placed->Works = source.Works;
        placed->Resources = source.Resources;
        for (auto& resource : placed->Resources) {
            if (resource.Connection != InvalidIndex) resource.Connection += resourceBase;
            for (auto& value : resource.ResolvedValues)
                if (value.first != InvalidIndex) value.first += resourceBase;
        }
        placed->Passes = source.Passes;
        for (auto& pass : placed->Passes) {
            for (auto& requirement : pass.WorkRequirements) requirement.first += workBase;
            for (auto& access : pass.Accesses) access.Resource += resourceBase;
            for (auto& view : pass.DeclaredViews) view += viewBase;
            for (auto& buffer : pass.DeclaredBuffers) buffer += resourceBase;
            for (auto& color : pass.Colors)
                if (color) color->View += viewBase;
            if (pass.DepthAttachment) pass.DepthAttachment->View += viewBase;
            if (pass.CopyOp) {
                pass.CopyOp->Source += resourceBase;
                pass.CopyOp->Destination += resourceBase;
            }
        }
        placed->Views = source.Views;
        for (auto& view : placed->Views) view.Resource += resourceBase;
        placed->Indirect = source.Indirect;
        for (auto& indirect : placed->Indirect) {
            indirect.Resource += resourceBase;
            indirect.Pass += passBase;
        }
        placement = std::move(placed);
        RenderGraphTemplate::Impl::PlacementEntry entry{placement, ++source.UseSerial};
        if (source.Placements.size() < 4)
            source.Placements.push_back(std::move(entry));
        else
            *std::min_element(source.Placements.begin(), source.Placements.end(), [](const auto& a, const auto& b) { return a.Used < b.Used; }) = std::move(entry);
    }
    const auto instance = static_cast<uint32_t>(impl.Templates.size());
    ++impl.Report.TemplateInstances;
    impl.Templates.push_back({std::move(graphTemplate), placement, vector<shared_ptr<void>>(source.SlotTypes.size())});
    for (const auto& declaration : placement->Resources) {
        auto& resource = impl.Resources.emplace_back(declaration);
        resource.TemplateInstance = instance;
    }
    for (const auto& declaration : placement->Passes) {
        auto& pass = impl.Passes.emplace_back(declaration);
        pass.TemplateInstance = instance;
        pass.RasterGroup = static_cast<uint32_t>(impl.Passes.size() - 1);
        pass.UavWriteStages = declaration.UavWriteStages;
        pass.AllowUavWrites = declaration.AllowUavWrites;
        if (declaration.Tracked) pass.Ticket._state = make_shared<FrameSubmission>(impl.GenerationSerial);
        if (impl.ReportFull()) impl.Report.Passes.push_back({declaration.Name, string{declaration.Location.file_name()}, declaration.Location.line(), declaration.Type});
    }
    for (const auto& declaration : placement->Views) {
        auto& view = impl.Views.emplace_back(declaration);
        view.TemplateInstance = instance;
    }
    for (const auto& declaration : placement->Indirect) {
        auto& indirect = impl.IndirectArgumentsRecords.emplace_back(declaration);
        indirect.TemplateInstance = instance;
    }
    for (const auto& declaration : placement->Works) {
        auto& work = impl.Works.emplace_back(declaration);
        work.TemplateInstance = instance;
    }
    RenderGraphTemplateInstance result;
    result._graph = this;
    result._index = instance;
    result._generation = impl.Generation;
    return result;
}

bool RenderGraph::SetColorClear(RgPassHandle handle, uint32_t slot, render::ColorClearValue clear) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return false;
    if (handle.Generation != impl.Generation || handle.Index >= impl.Passes.size() || slot >= 8 ||
        slot >= impl.Passes[handle.Index].Def().Colors.size() || !impl.Passes[handle.Index].Def().Colors[slot]) {
        impl.Error("TemplateClear", "Color clear patch requires a declared attachment in this graph");
        return false;
    }
    impl.Passes[handle.Index].ColorClearOverrides[slot] = clear;
    return true;
}
bool RenderGraph::SetDepthClear(RgPassHandle handle, render::DepthStencilClearValue clear) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return false;
    if (handle.Generation != impl.Generation || handle.Index >= impl.Passes.size() || !impl.Passes[handle.Index].Def().DepthAttachment) {
        impl.Error("TemplateClear", "Depth clear patch requires a declared attachment in this graph");
        return false;
    }
    impl.Passes[handle.Index].DepthClearOverride = clear;
    return true;
}

bool RenderGraph::BindTemplateSlot(const RenderGraphTemplateInstance& instance, uint32_t slot, uint64_t generation, const void* type, shared_ptr<void> data) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return false;
    if (instance._graph.Get() != this || instance._generation != impl.Generation || instance._index >= impl.Templates.size()) {
        impl.Error("TemplateInstance", "Typed binding requires a current graph instance");
        return false;
    }
    auto& target = impl.Templates[instance._index];
    const auto& source = *target.Source->_impl;
    if (generation != source.Generation || slot >= source.SlotTypes.size() || source.SlotTypes[slot] != type || !data || target.Slots[slot]) {
        impl.Error("TemplateSlot", "A typed slot must be bound once to a non-null value of its declared type");
        return false;
    }
    target.Slots[slot] = std::move(data);
    impl.Owners.push_back(target.Slots[slot]);
    return true;
}

uint32_t RenderGraph::MapTemplateHandle(const RenderGraphTemplateInstance& instance, uint32_t index, uint64_t generation, uint8_t kind, uint32_t version) const {
    auto& impl = *_impl;
    if (instance._graph.Get() != this || instance._generation != impl.Generation || instance._index >= impl.Templates.size()) return InvalidIndex;
    const auto& target = impl.Templates[instance._index];
    const auto& source = *target.Source->_impl;
    if (generation != source.Generation) return InvalidIndex;
    if (kind == 4) return index < source.Passes.size() ? target.Placement->PassBase + index : InvalidIndex;
    if (kind == 5) return index < source.Works.size() ? target.Placement->WorkBase + index : InvalidIndex;
    if (index >= source.Resources.size() || source.Resources[index].IsTexture != (kind == 0 || kind == 2) ||
        (kind >= 2 && !source.Resources[index].Port)) return InvalidIndex;
    if (kind < 2 && version >= source.Resources[index].VersionParents.size()) return InvalidIndex;
    return target.Placement->ResourceBase + index;
}

RgTextureValue RenderGraphTemplateInstance::Value(RgTextureValue value) const {
    return _graph ? RgTextureValue{_graph->MapTemplateHandle(*this, value.Index, value.Generation, 0, value.Version), _generation, value.Version} : RgTextureValue{};
}
RgBufferValue RenderGraphTemplateInstance::Value(RgBufferValue value) const {
    return _graph ? RgBufferValue{_graph->MapTemplateHandle(*this, value.Index, value.Generation, 1, value.Version), _generation, value.Version} : RgBufferValue{};
}
RgTexturePort RenderGraphTemplateInstance::Value(RgTexturePort value) const {
    return _graph ? RgTexturePort{_graph->MapTemplateHandle(*this, value.Index, value.Generation, 2), _generation} : RgTexturePort{};
}
RgBufferPort RenderGraphTemplateInstance::Value(RgBufferPort value) const {
    return _graph ? RgBufferPort{_graph->MapTemplateHandle(*this, value.Index, value.Generation, 3), _generation} : RgBufferPort{};
}
RgPassHandle RenderGraphTemplateInstance::Value(RgPassHandle value) const {
    return _graph ? RgPassHandle{_graph->MapTemplateHandle(*this, value.Index, value.Generation, 4), _generation} : RgPassHandle{};
}
RgWorkHandle RenderGraphTemplateInstance::Value(RgWorkHandle value) const {
    return _graph ? RgWorkHandle{_graph->MapTemplateHandle(*this, value.Index, value.Generation, 5), _generation} : RgWorkHandle{};
}

bool RenderGraphTemplateInstance::Bind(RgTextureValue slot, RenderExternalTexture& texture) const {
    return _graph && _graph->BindExternalTexture(Value(slot), texture);
}
bool RenderGraphTemplateInstance::Bind(RgBufferValue slot, RenderExternalBuffer& buffer) const {
    return _graph && _graph->BindExternalBuffer(Value(slot), buffer);
}

void RenderGraph::Impl::MaterializeTemplates() {
    ++Report.TemplateMaterializations;
    for (auto& resource : Resources) {
        resource.Declaration.Materialize();
        if (!resource.VersionTail.empty()) {
            auto& parents = resource.Edit().VersionParents;
            parents.insert(parents.end(), resource.VersionTail.begin(), resource.VersionTail.end());
        }
        if (resource.ConnectionPatch) {
            if (resource.Def().ExternalSlot) resource.Edit().Port = true;
            resource.Edit().Connection = resource.ConnectionPatch->first;
            resource.Edit().ConnectionVersion = resource.ConnectionPatch->second;
        }
    }
    for (auto& pass : Passes) pass.Declaration.Materialize();
}

bool RenderGraph::Impl::InstantiateTemplatePayloads() {
    for (const auto& [index, mask] : FramePlan->LiveWork) {
        auto& work = Works[index];
        const auto& declaration = work.Declaration.Get();
        if (!declaration.Factory) continue;
        if (work.TemplateInstance >= Templates.size() || declaration.Slot >= Templates[work.TemplateInstance].Slots.size() || !Templates[work.TemplateInstance].Slots[declaration.Slot]) {
            Error("TemplateSlot", "A live template work has an unbound frame slot");
            return false;
        }
        work.Data = declaration.Factory->Instantiate(Templates[work.TemplateInstance].Slots[declaration.Slot].get());
    }
    for (const auto p : GetCompiled().ExecutionOrder) {
        auto& pass = Passes[p];
        if (!pass.Def().Factory && pass.Def().UploadSlot == InvalidIndex) continue;
        if (pass.TemplateInstance >= Templates.size()) {
            Error("TemplateInstance", "Template callbacks execute only through a bound frame instance", p);
            return false;
        }
        const auto& slots = Templates[pass.TemplateInstance].Slots;
        if (pass.Def().UploadSlot != InvalidIndex) {
            if (pass.Def().UploadSlot >= slots.size() || !slots[pass.Def().UploadSlot]) {
                Error("TemplateSlot", "A live template upload has an unbound data slot", p);
                return false;
            }
            pass.UploadSource = static_cast<RgUploadData*>(slots[pass.Def().UploadSlot].get());
        }
        if (!pass.Def().Factory) continue;
        if (pass.Def().Slot >= slots.size() || !slots[pass.Def().Slot]) {
            Error("TemplateSlot", "A live template callback has an unbound frame slot", p);
            return false;
        }
        pass.Data = pass.Def().Factory->Instantiate(slots[pass.Def().Slot].get());
    }
    return true;
}

RgTextureViewHandle RenderGraph::Impl::PatchTemplateView(uint32_t pass, RgTextureViewHandle value) const {
    const auto instance = Passes[pass].TemplateInstance;
    if (instance < Templates.size() && value.Generation == Templates[instance].Source->GetGeneration()) {
        const auto& target = Templates[instance];
        if (value.Index >= target.Placement->Views.size()) return {};
        return {target.Placement->ViewBase + value.Index, Generation};
    }
    return value;
}
RgBufferValue RenderGraph::Impl::PatchTemplateBuffer(uint32_t pass, RgBufferValue value) const {
    const auto instance = Passes[pass].TemplateInstance;
    if (instance < Templates.size() && value.Generation == Templates[instance].Source->GetGeneration()) {
        const auto& target = Templates[instance];
        if (value.Index >= target.Placement->Resources.size() || target.Placement->Resources[value.Index].IsTexture) return {};
        return {target.Placement->ResourceBase + value.Index, Generation, value.Version};
    }
    return value;
}
RgIndirectArgumentsHandle RenderGraph::Impl::PatchTemplateIndirect(uint32_t pass, RgIndirectArgumentsHandle value) const {
    const auto instance = Passes[pass].TemplateInstance;
    if (instance < Templates.size() && value.Generation == Templates[instance].Source->GetGeneration()) {
        const auto& target = Templates[instance];
        if (value.Index >= target.Placement->Indirect.size()) return {};
        return {target.Placement->IndirectBase + value.Index, Generation};
    }
    return value;
}

RenderGraph::Impl::SubmissionData RenderGraph::Impl::DetachSubmissionResources() {
    SubmissionData submission{make_shared<SubmissionState>(), make_shared<SubmissionResources>()};
    auto& retained = submission.Retained;
    retained->Plan = FramePlan;
    retained->Owners = Owners;
    for (auto& work : Works) retained->WorkPayloads.push_back(std::move(work.Data));
    for (auto& pass : Passes) {
        if (pass.Data) retained->Payloads.push_back(std::move(pass.Data));
        if (pass.Ticket._state) retained->Tickets.push_back(pass.Ticket._state);
    }
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        if (resource.Readback) retained->Readbacks.push_back(resource.Readback);
        if (resource.States.empty() || resource.Physical != index) continue;
        if (resource.Def().IsTexture) {
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

RenderGraph::RenderGraph(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry, std::string_view name,
                         RenderGraphRuntimeOptions runtime)
    : _impl(make_unique<Impl>(device, pool, registry, nullptr, name, nullptr, runtime)) {}
RenderGraph::RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                         render::RenderPassRegistry& registry, std::string_view name, RenderGraphRuntimeOptions runtime)
    : _impl(make_unique<Impl>(device, resources.GetPool(), registry, &resources, name, nullptr, runtime)) {}
RenderGraph::RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                         render::RenderPassRegistry& registry, std::string_view name, uint64_t& generation, RenderGraphExecutionReport& report,
                         RenderGraphRuntimeOptions runtime)
    : _impl(make_unique<Impl>(device, resources.GetPool(), registry, &resources, name, &report, runtime)) { generation = _impl->Generation; }
RenderGraph::~RenderGraph() = default;
uint64_t RenderGraph::GetGeneration() const noexcept { return _impl->Generation; }
const RenderGraphRuntimeOptions& RenderGraph::GetRuntimeOptions() const noexcept { return _impl->Runtime; }
bool RenderGraph::IsValidationFull() const noexcept { return _impl->ValidationFull(); }
bool RenderGraph::HasFailed() const noexcept { return _impl->Failed; }
uint32_t RenderGraph::GetPassCount() const noexcept { return static_cast<uint32_t>(_impl->Passes.size()); }
std::string_view RenderGraph::GetFirstErrorCode() const noexcept { return _impl->FirstErrorCode; }
bool RenderGraphPassBuilder::IsValidationFull() const noexcept { return _graph.IsValidationFull(); }
const RenderGraphRuntimeOptions& RenderGraphPassBuilder::GetRuntimeOptions() const noexcept { return _graph.GetRuntimeOptions(); }
uint64_t RenderGraph::SetResourceView(uint64_t viewId) {
    if (!_impl->Mutable()) return _impl->ResourceView;
    return std::exchange(_impl->ResourceView, viewId);
}
bool RenderGraph::WasPassExecuted(RgPassHandle handle) const noexcept {
    return handle.Generation == _impl->Generation && handle.Index < _impl->Passes.size() && _impl->Passes[handle.Index].Executed;
}
bool RenderGraph::PassWroteTexture(RgPassHandle pass, RgTextureValue texture) const noexcept {
    if (!WasPassExecuted(pass) || texture.Generation != _impl->Generation || texture.Index >= _impl->Resources.size()) return false;
    if (_impl->Resources[texture.Index].Def().Port && texture.Version < _impl->Resources[texture.Index].Def().ResolvedValues.size()) {
        const auto mapped = _impl->Resources[texture.Index].Def().ResolvedValues[texture.Version];
        texture.Index = mapped.first;
        texture.Version = mapped.second;
    }
    if (texture.Index >= _impl->Resources.size()) return false;
    const auto& resource = _impl->Resources[texture.Index];
    if (!resource.Def().IsTexture || !resource.Written) return false;
    for (const auto& access : _impl->Passes[pass.Index].GetCells())
        if (access.Resource == texture.Index && access.Version == texture.Version && access.Write && access.ValidAfter && resource.Valid[access.Cell]) return true;
    return false;
}

RgPassHandle RenderGraphPassBuilder::GetPassHandle() const noexcept { return {_pass, _graph.GetGeneration()}; }
void RenderGraphPassBuilder::Reject(std::string_view code, std::string_view message, std::string_view binding) {
    _graph._impl->Error(code, message, _pass, InvalidIndex, binding);
}

RgPassHandle RenderGraphRasterContext::GetPassHandle() const noexcept { return {_pass, _graph.GetGeneration()}; }
const RenderGraphExecutionReport& RenderGraph::GetReport() const noexcept { return _impl->Report; }

RgTextureValue RenderGraph::CreateTexture(const render::TextureDescriptor& desc, std::string_view name, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    ++impl.Report.ResourceDeclarations;
    const auto index = static_cast<uint32_t>(impl.Resources.size());
    Impl::Resource resource{};
    resource.Edit().Name = name;
    resource.Edit().Location = location;
    resource.Edit().IsTexture = true;
    resource.Edit().TextureDesc = desc;
    resource.Edit().ViewId = impl.ResourceView;
    impl.Resources.push_back(std::move(resource));
    return {index, impl.Generation, 1};
}
RgBufferValue RenderGraph::CreateBuffer(const render::BufferDescriptor& desc, std::string_view name, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    ++impl.Report.ResourceDeclarations;
    const auto index = static_cast<uint32_t>(impl.Resources.size());
    Impl::Resource resource{};
    resource.Edit().Name = name;
    resource.Edit().Location = location;
    resource.Edit().BufferDesc = desc;
    resource.Edit().ViewId = impl.ResourceView;
    impl.Resources.push_back(std::move(resource));
    return {index, impl.Generation, 1};
}
RgTextureValue RenderGraph::DeclareExternalTexture(const render::TextureDescriptor& desc, std::string_view name, RenderGraphExternalAccess access) {
    auto value = CreateTexture(desc, name);
    if (value.IsValid()) {
        auto& declaration = _impl->Resources[value.Index].Edit();
        declaration.ExternalSlot = true;
        declaration.ExternalAccess = access;
        declaration.VersionParents.resize(1);
        value.Version = 0;
    }
    return value;
}
RgBufferValue RenderGraph::DeclareExternalBuffer(const render::BufferDescriptor& desc, std::string_view name, RenderGraphExternalAccess access) {
    auto value = CreateBuffer(desc, name);
    if (value.IsValid()) {
        auto& declaration = _impl->Resources[value.Index].Edit();
        declaration.ExternalSlot = true;
        declaration.ExternalAccess = access;
        declaration.VersionParents.resize(1);
        value.Version = 0;
    }
    return value;
}
bool RenderGraph::BindExternalTexture(RgTextureValue slot, RenderExternalTexture& texture) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(slot.Index, slot.Generation, true)) return false;
    auto& resource = impl.Resources[slot.Index];
    if (slot.Version != 0 || !resource.Def().ExternalSlot || resource.ExternalBound || !texture.Texture ||
        !(TexturePoolKey{resource.Def().TextureDesc} == TexturePoolKey{texture.Desc})) {
        impl.Error("TemplateExternal", "Bind an external texture slot exactly once with its declared descriptor", InvalidIndex, slot.Index);
        return false;
    }
    for (uint32_t index = 0; index < impl.Resources.size(); ++index) {
        auto& canonical = impl.Resources[index];
        if (!canonical.ExternalTexture || canonical.ExternalTexture->Texture != texture.Texture) continue;
        const auto& first = *canonical.ExternalTexture;
        if (!(TexturePoolKey{first.Desc} == TexturePoolKey{texture.Desc}) ||
            !std::equal(first.SubresourceStates.begin(), first.SubresourceStates.end(), texture.SubresourceStates.begin(), texture.SubresourceStates.end()) ||
            !std::equal(first.ContentValid.begin(), first.ContentValid.end(), texture.ContentValid.begin(), texture.ContentValid.end())) {
            impl.Error("ConflictingImport", "One native texture identity has conflicting descriptors, states or content validity");
            return false;
        }
        canonical.ExternalAccessOverride = static_cast<RenderGraphExternalAccess>(std::max(uint8_t(canonical.AccessMode()), uint8_t(resource.Def().ExternalAccess)));
        if (std::find(canonical.TextureImports.begin(), canonical.TextureImports.end(), &texture) == canonical.TextureImports.end()) canonical.TextureImports.push_back(&texture);
        resource.ConnectionPatch = std::pair{index, 0u};
        resource.ExternalBound = true;
        Retain(texture.Owner);
        return true;
    }
    resource.ExternalTexture = &texture;
    resource.TextureImports.push_back(&texture);
    resource.ExternalBound = true;
    Retain(texture.Owner);
    return true;
}
bool RenderGraph::BindExternalBuffer(RgBufferValue slot, RenderExternalBuffer& buffer) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(slot.Index, slot.Generation, false)) return false;
    auto& resource = impl.Resources[slot.Index];
    if (slot.Version != 0 || !resource.Def().ExternalSlot || resource.ExternalBound || !buffer.Buffer ||
        !(BufferPoolKey{resource.Def().BufferDesc} == BufferPoolKey{buffer.Desc})) {
        impl.Error("TemplateExternal", "Bind an external buffer slot exactly once with its declared descriptor", InvalidIndex, slot.Index);
        return false;
    }
    for (uint32_t index = 0; index < impl.Resources.size(); ++index) {
        auto& canonical = impl.Resources[index];
        if (!canonical.ExternalBuffer || canonical.ExternalBuffer->Buffer != buffer.Buffer) continue;
        const auto& first = *canonical.ExternalBuffer;
        if (!(BufferPoolKey{first.Desc} == BufferPoolKey{buffer.Desc}) || first.State != buffer.State || first.ContentValid != buffer.ContentValid) {
            impl.Error("ConflictingImport", "One native buffer identity has conflicting descriptors, states or content validity");
            return false;
        }
        canonical.ExternalAccessOverride = static_cast<RenderGraphExternalAccess>(std::max(uint8_t(canonical.AccessMode()), uint8_t(resource.Def().ExternalAccess)));
        if (std::find(canonical.BufferImports.begin(), canonical.BufferImports.end(), &buffer) == canonical.BufferImports.end()) canonical.BufferImports.push_back(&buffer);
        resource.ConnectionPatch = std::pair{index, 0u};
        resource.ExternalBound = true;
        Retain(buffer.Owner);
        return true;
    }
    resource.ExternalBuffer = &buffer;
    resource.BufferImports.push_back(&buffer);
    resource.ExternalBound = true;
    Retain(buffer.Owner);
    return true;
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
        existing.ExternalAccessOverride = static_cast<RenderGraphExternalAccess>(std::max(uint8_t(existing.AccessMode()), uint8_t(access)));
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
        resource.Edit().VersionParents.resize(1);
        result.Version = 0;
        resource.Edit().ExternalAccess = access;
    }
    return result;
}
RgBufferValue RenderGraph::ImportBuffer(RenderExternalBuffer& buffer, std::string_view name, RenderGraphExternalAccess access, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    for (uint32_t i = 0; i < impl.Resources.size(); ++i) {
        auto& existing = impl.Resources[i];
        if (!existing.ExternalBuffer || existing.ExternalBuffer->Buffer != buffer.Buffer) continue;
        const auto& first = *existing.ExternalBuffer;
        if (!(BufferPoolKey{first.Desc} == BufferPoolKey{buffer.Desc}) || first.State != buffer.State || first.ContentValid != buffer.ContentValid) {
            impl.Error("ConflictingImport", "One native buffer identity has conflicting descriptors, states or content validity");
            return {};
        }
        existing.ExternalAccessOverride = static_cast<RenderGraphExternalAccess>(std::max(uint8_t(existing.AccessMode()), uint8_t(access)));
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
        resource.Edit().VersionParents.resize(1);
        result.Version = 0;
        resource.Edit().ExternalAccess = access;
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
    data.Edit().Accesses.push_back({value.Index, {0, 1, 0, 1}, uint32_t(render::BufferState::HostWrite), false, true, true, value.Version, {0, bytes.size()}});
    return value;
}
RgBufferValue RenderGraph::UploadBuffer(std::string_view name, uint64_t size, render::BufferUses usage,
                                        const RgUploadData& source, RgWorkHandle work, uint64_t mask) {
    if (size == 0) {
        AddDiagnostic("UploadSize", "An upload must contain bytes");
        return {};
    }
    const auto value = CreateBuffer({size, render::MemoryType::Upload, usage | render::BufferUse::MapWrite, {}}, name);
    const auto pass = AddPass(name, RgPassType::Upload, std::source_location::current());
    if (!value.IsValid() || !pass.IsValid()) return {};
    auto& data = _impl->Passes[pass.Index];
    data.UploadSource = &source;
    data.Edit().Accesses.push_back({value.Index, {0, 1, 0, 1}, uint32_t(render::BufferState::HostWrite), false, true, true, value.Version, {0, size}});
    RequireWork(pass.Index, work, mask);
    return value;
}
RgWorkHandle RenderGraph::AddTemplateWorkFactory(std::string_view name, uint32_t slot, uint64_t generation, const void* type, shared_ptr<const TemplateWorkFactory> factory) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    if (generation != impl.Generation || slot >= impl.TemplateSlotTypes.size() || impl.TemplateSlotTypes[slot] != type) {
        impl.Error("TemplateSlot", "Template work requires a matching typed slot from this builder");
        return {};
    }
    const auto index = static_cast<uint32_t>(impl.Works.size());
    impl.Works.emplace_back().Declaration.Edit() = {string{name}, std::move(factory), slot};
    return {index, impl.Generation};
}
RgBufferValue RenderGraph::UploadBuffer(std::string_view name, uint64_t size, render::BufferUses usage,
                                        RgTemplateSlot<RgUploadData> slot, RgWorkHandle work, uint64_t mask) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    if (!size || slot.Generation != impl.Generation || slot.Index >= impl.TemplateSlotTypes.size() || impl.TemplateSlotTypes[slot.Index] != &TemplateType<RgUploadData>) {
        impl.Error("TemplateSlot", "A template upload requires a matching typed data slot and nonzero size");
        return {};
    }
    const auto value = CreateBuffer({size, render::MemoryType::Upload, usage | render::BufferUse::MapWrite, {}}, name);
    const auto pass = AddPass(name, RgPassType::Upload, std::source_location::current());
    if (!value.IsValid() || !pass.IsValid()) return {};
    auto& declaration = impl.Passes[pass.Index].Edit();
    declaration.UploadSlot = slot.Index;
    declaration.Accesses.push_back({value.Index, {0, 1, 0, 1}, uint32_t(render::BufferState::HostWrite), false, true, true, value.Version, {0, size}});
    RequireWork(pass.Index, work, mask);
    return value;
}
RgWorkHandle RenderGraph::AddWorkPayload(std::string_view name, unique_ptr<WorkPayload> payload) {
    auto& impl = *_impl;
    if (!impl.Mutable() || impl.Works.size() >= InvalidIndex) return {};
    const auto index = static_cast<uint32_t>(impl.Works.size());
    auto& work = impl.Works.emplace_back();
    work.Declaration.Edit().Name = name;
    work.Data = std::move(payload);
    return {index, impl.Generation};
}
void RenderGraph::RequireWork(uint32_t pass, RgWorkHandle work, uint64_t mask) {
    auto& impl = *_impl;
    if (!impl.Mutable() || work.Generation != impl.Generation || work.Index >= impl.Works.size() || mask == 0) {
        impl.Error("WorkHandle", "Work requirements need a current graph handle and nonzero mask", pass);
        return;
    }
    auto& requirements = impl.Passes[pass].Edit().WorkRequirements;
    for (auto& [index, bits] : requirements)
        if (index == work.Index) {
            bits |= mask;
            return;
        }
    requirements.emplace_back(work.Index, mask);
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
    const auto normalized = render::NormalizeSubresourceRange(impl.Resources[value.Index].Def().TextureDesc, range);
    if (!normalized || !ValidTextureState(impl.Resources[value.Index].Def().TextureDesc, finalState, false)) {
        impl.Error("ExportState", "Export requires a valid range and defined final state");
        return {};
    }
    const auto pass = AddPass("Export." + impl.Resources[value.Index].Def().Name, RgPassType::Export, std::source_location::current());
    auto& data = impl.Passes[pass.Index];
    data.Edit().SideEffect = true;
    data.Edit().Accesses.push_back({value.Index, *normalized, finalState.value(), true, false, true, value.Version});
    return Track(pass);
}
RgOperationTicket RenderGraph::ExportBuffer(RgBufferValue value, RgBufferAccess finalAccess, render::BufferRange range) {
    auto& impl = *_impl;
    const auto pass = AddPass("Export.Buffer", RgPassType::Export, std::source_location::current());
    if (!pass.IsValid()) return {};
    if (!UseBuffer(pass.Index, value, finalAccess, false, false, render::ShaderStage::UNKNOWN, range).IsValid()) return {};
    impl.Passes[pass.Index].Edit().Accesses.back().Read = true;
    impl.Passes[pass.Index].Edit().SideEffect = true;
    return Track(pass);
}
RgReadbackTicket RenderGraph::ReadbackTexture(std::string_view name, RgTextureValue value, render::SubresourceRange range) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, true)) return {};
    const auto& desc = impl.Resources[value.Index].Def().TextureDesc;
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
    const auto size = impl.Resources[value.Index].Def().BufferDesc.Size;
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
    resource.Edit().Port = true;
    resource.Edit().VersionParents.resize(1);
    return {value.Index, value.Generation};
}
RgBufferPort RenderGraph::DeclareBufferPort(const render::BufferDescriptor& desc, std::string_view name) {
    const auto value = CreateBuffer(desc, name);
    if (!value.IsValid()) return {};
    auto& resource = _impl->Resources[value.Index];
    resource.Edit().Port = true;
    resource.Edit().VersionParents.resize(1);
    return {value.Index, value.Generation};
}
RgTextureValue RenderGraph::Value(RgTexturePort port) const noexcept {
    const auto& impl = *_impl;
    if (port.Generation != impl.Generation || port.Index >= impl.Resources.size() || !impl.Resources[port.Index].Def().Port || !impl.Resources[port.Index].Def().IsTexture) return {};
    return {port.Index, port.Generation, 0};
}
RgBufferValue RenderGraph::Value(RgBufferPort port) const noexcept {
    const auto& impl = *_impl;
    if (port.Generation != impl.Generation || port.Index >= impl.Resources.size() || !impl.Resources[port.Index].Def().Port || impl.Resources[port.Index].Def().IsTexture) return {};
    return {port.Index, port.Generation, 0};
}
bool RenderGraph::Connect(RgTexturePort input, RgTextureValue output) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(input.Index, input.Generation, true) || !impl.Handle(output.Index, output.Generation, true)) return false;
    auto& port = impl.Resources[input.Index];
    const auto& source = impl.Resources[output.Index];
    if (!port.Def().Port || port.Def().Connection != InvalidIndex || port.ConnectionPatch || output.Version >= source.VersionCount() || !(TexturePoolKey{port.Def().TextureDesc} == TexturePoolKey{source.Def().TextureDesc})) {
        impl.Error("PortConnection", "Texture port must have one producer with an identical descriptor", InvalidIndex, input.Index);
        return false;
    }
    if (port.Declaration.Borrowed)
        port.ConnectionPatch = std::pair{output.Index, output.Version};
    else {
        port.Edit().Connection = output.Index;
        port.Edit().ConnectionVersion = output.Version;
    }
    return true;
}
bool RenderGraph::Connect(RgBufferPort input, RgBufferValue output) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(input.Index, input.Generation, false) || !impl.Handle(output.Index, output.Generation, false)) return false;
    auto& port = impl.Resources[input.Index];
    const auto& source = impl.Resources[output.Index];
    if (!port.Def().Port || port.Def().Connection != InvalidIndex || port.ConnectionPatch || output.Version >= source.VersionCount() || !(BufferPoolKey{port.Def().BufferDesc} == BufferPoolKey{source.Def().BufferDesc})) {
        impl.Error("PortConnection", "Buffer port must have one producer with an identical descriptor", InvalidIndex, input.Index);
        return false;
    }
    if (port.Declaration.Borrowed)
        port.ConnectionPatch = std::pair{output.Index, output.Version};
    else {
        port.Edit().Connection = output.Index;
        port.Edit().ConnectionVersion = output.Version;
    }
    return true;
}

bool RenderGraph::Impl::ResolvePorts() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::ResolvePorts");
    ++Report.PortResolveBuilds;
    vector<vector<uint8_t>> visiting(Resources.size());
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        resource.Edit().ResolvedValues.assign(resource.Def().VersionParents.size(), {InvalidIndex, InvalidIndex});
        visiting[r].assign(resource.Def().VersionParents.size(), 0);
        if (resource.Def().Port && resource.Def().Connection == InvalidIndex) Error("UnconnectedPort", "Every declared input port requires one connection", InvalidIndex, r);
    }
    if (Failed) return false;
    for (const auto& pass : Passes)
        for (const auto& access : pass.Def().Accesses) {
            if (access.Resource >= Resources.size() || access.Version >= Resources[access.Resource].Def().VersionParents.size())
                Error("InvalidVersion", "Pass access names an unreserved resource version");
            else if (access.Write && pass.TemplateInstance != InvalidIndex && pass.TemplateInstance == Resources[access.Resource].TemplateInstance &&
                     Resources[access.Resource].Def().ExternalSlot && Resources[access.Resource].Def().ExternalAccess == RenderGraphExternalAccess::ReadOnly)
                Error("ReadOnlyExternal", "Cannot write a read-only external slot", InvalidIndex, access.Resource);
        }
    for (const auto& view : Views)
        if (view.Resource >= Resources.size() || view.Version >= Resources[view.Resource].Def().VersionParents.size()) Error("InvalidVersion", "View names an unreserved resource version");
    if (Failed) return false;
    const auto resolve = [&](auto&& self, uint32_t r, uint32_t v) -> std::pair<uint32_t, uint32_t> {
        if (r >= Resources.size() || v >= Resources[r].Def().VersionParents.size()) {
            Error("InvalidVersion", "Port connects an unreserved version");
            return {InvalidIndex, InvalidIndex};
        }
        auto& resource = Resources[r];
        if (!resource.Def().Port) return {r, v};
        auto& result = resource.Edit().ResolvedValues[v];
        if (result.first != InvalidIndex) return result;
        if (visiting[r][v]) {
            Error("PortCycle", "Resource port connections contain a cycle", InvalidIndex, r);
            return {InvalidIndex, InvalidIndex};
        }
        visiting[r][v] = 1;
        const auto parent = v == 0 ? self(self, resource.Def().Connection, resource.Def().ConnectionVersion) : self(self, r, resource.Def().VersionParents[v]);
        if (parent.first != InvalidIndex) {
            if (v == 0)
                result = parent;
            else {
                auto& parents = Resources[parent.first].Edit().VersionParents;
                result = {parent.first, static_cast<uint32_t>(parents.size())};
                parents.push_back(parent.second);
            }
        }
        visiting[r][v] = 0;
        return result;
    };
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        const auto count = static_cast<uint32_t>(Resources[r].Def().ResolvedValues.size());
        for (uint32_t v = 0; v < count; ++v) {
            const auto value = resolve(resolve, r, v);
            Resources[r].Edit().ResolvedValues[v] = value;
        }
    }
    if (Failed) return false;
    const auto mapped = [&](uint32_t r, uint32_t v) { return Resources[r].Def().Port ? Resources[r].Def().ResolvedValues[v] : std::pair{r, v}; };
    for (auto& pass : Passes) {
        for (auto& access : pass.Edit().Accesses) {
            const auto value = mapped(access.Resource, access.Version);
            access.Resource = value.first;
            access.Version = value.second;
        }
        for (auto& r : pass.Edit().DeclaredBuffers) r = mapped(r, 0).first;
        if (pass.Def().CopyOp) {
            pass.Edit().CopyOp->Source = mapped(pass.Def().CopyOp->Source, 0).first;
            pass.Edit().CopyOp->Destination = mapped(pass.Def().CopyOp->Destination, 0).first;
        }
    }
    for (auto& view : Views) {
        const auto value = mapped(view.Resource, view.Version);
        view.Resource = value.first;
        view.Version = value.second;
    }
    for (auto& arguments : IndirectArgumentsRecords) arguments.Resource = mapped(arguments.Resource, 0).first;
    return true;
}

RgTextureValue RenderGraph::NextVersion(RgTextureValue value) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, true)) return {};
    auto& resource = impl.Resources[value.Index];
    const auto count = resource.VersionCount();
    if (value.Version >= count) {
        impl.Error("InvalidVersion", "Texture version is out of range");
        return {};
    }
    if (value.Version + 1 != count) {
        impl.Error("VersionBranch", "In-place versions form one storage chain; copy to another resource to branch contents");
        return {};
    }
    if (resource.Declaration.Borrowed)
        resource.VersionTail.push_back(value.Version);
    else
        resource.Edit().VersionParents.push_back(value.Version);
    value.Version = count;
    return value;
}
RgBufferValue RenderGraph::NextVersion(RgBufferValue value) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(value.Index, value.Generation, false)) return {};
    auto& resource = impl.Resources[value.Index];
    const auto count = resource.VersionCount();
    if (value.Version >= count) {
        impl.Error("InvalidVersion", "Buffer version is out of range");
        return {};
    }
    if (value.Version + 1 != count) {
        impl.Error("VersionBranch", "In-place versions form one storage chain; copy to another resource to branch contents");
        return {};
    }
    if (resource.Declaration.Borrowed)
        resource.VersionTail.push_back(value.Version);
    else
        resource.Edit().VersionParents.push_back(value.Version);
    value.Version = count;
    return value;
}

RgPassHandle RenderGraph::AddPass(std::string_view name, RgPassType type, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    ++impl.Report.PassDeclarations;
    const auto index = static_cast<uint32_t>(impl.Passes.size());
    Impl::Pass pass{};
    pass.Edit().Location = location;
    pass.Edit().Name = string{name};
    pass.Edit().Type = type;
    pass.RasterGroup = index;
    impl.Passes.push_back(std::move(pass));
    if (impl.ReportFull()) impl.Report.Passes.push_back({string{name}, string{location.file_name()}, location.line(), type});
    return {index, impl.Generation};
}
void RenderGraph::SetPayload(RgPassHandle pass, unique_ptr<Payload> payload) { _impl->Passes[pass.Index].Data = std::move(payload); }

RgTextureViewHandle RenderGraph::UseTexture(uint32_t pass, RgTextureValue texture, RgTextureViewDesc view,
                                            render::TextureViewUsage usage, bool read, bool write, bool validAfter,
                                            render::ShaderStages uavWriteStages) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(texture.Index, texture.Generation, true, pass)) return {};
    const auto& desc = impl.Resources[texture.Index].Def().TextureDesc;
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
    impl.Passes[pass].Edit().Accesses.push_back({texture.Index, *range, StateFor(usage), read, write, validAfter, texture.Version});
    impl.Passes[pass].Edit().Accesses.back().Stages = view.Stages ? view.Stages : uavWriteStages ? uavWriteStages
                                                                                                 : render::ShaderStages{impl.Passes[pass].Def().Type == RgPassType::Compute ? render::ShaderStage::Compute : render::ShaderStage::Graphics};
    AddUnique(impl.Passes[pass].Edit().DeclaredViews, index);
    if (write && usage == render::TextureViewUsage::UnorderedAccess &&
        impl.Passes[pass].Def().Type == RgPassType::Raster) {
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
    const auto& desc = impl.Resources[buffer.Index].Def().BufferDesc;
    if (usage == render::BufferUse::UNKNOWN || !desc.Usage.HasFlag(usage) ||
        (write && access != RgBufferAccess::UnorderedAccess && access != RgBufferAccess::CopyDestination) ||
        (read && access == RgBufferAccess::CopyDestination)) {
        impl.Error("InvalidBufferAccess", "Buffer access is incompatible with its usage or read/write mode", pass, buffer.Index);
        return {};
    }
    impl.Passes[pass].Edit().Accesses.push_back({buffer.Index, {0, 1, 0, 1}, static_cast<uint32_t>(state), read, write, true, buffer.Version, range});
    impl.Passes[pass].Edit().Accesses.back().Stages = uavWriteStages ? uavWriteStages : render::ShaderStages{impl.Passes[pass].Def().Type == RgPassType::Compute ? render::ShaderStage::Compute : render::ShaderStage::Graphics};
    AddUnique(impl.Passes[pass].Edit().DeclaredBuffers, buffer.Index);
    if (write && access == RgBufferAccess::UnorderedAccess &&
        impl.Passes[pass].Def().Type == RgPassType::Raster) {
        auto& rasterPass = impl.Passes[pass];
        rasterPass.AllowUavWrites = true;
        rasterPass.UavWriteStages |= uavWriteStages ? uavWriteStages : render::ShaderStages{render::ShaderStage::Graphics};
    }
    return buffer;
}
RgTextureViewHandle RenderGraphPassBuilder::ReadTexture(RgTextureValue texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::Resource, true, false, true); }
RgBufferValue RenderGraphPassBuilder::ReadBuffer(RgBufferValue buffer, RgBufferAccess access, render::BufferRange range) { return _graph.UseBuffer(_pass, buffer, access, true, false, {}, range); }
RgBufferValue RenderGraphPassBuilder::WriteBuffer(RgBufferValue buffer, RgBufferAccess access, render::BufferRange range) { return _graph.UseBuffer(_pass, buffer, access, false, true, {}, range); }
RgBufferValue RenderGraphPassBuilder::ReadWriteBuffer(RgBufferValue buffer, RgBufferAccess access, render::BufferRange range) { return _graph.UseBuffer(_pass, buffer, access, true, true, {}, range); }
RgIndirectArgumentsHandle RenderGraphPassBuilder::ReadIndirectArguments(
    RgBufferValue buffer, RgIndirectCommand command, uint64_t offset, uint32_t count) {
    return _graph.AddIndirectArguments(_pass, buffer, command, offset, count);
}
void RenderGraphPassBuilder::SetSideEffect() { _graph._impl->Passes[_pass].Edit().SideEffect = true; }
void RenderGraphPassBuilder::RequireWork(RgWorkHandle work, uint64_t mask) { _graph.RequireWork(_pass, work, mask); }
RgTextureViewHandle RenderGraphComputeBuilder::WriteTexture(RgTextureValue texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess, false, true, true); }
RgTextureViewHandle RenderGraphComputeBuilder::ReadWriteTexture(RgTextureValue texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess, true, true, true); }
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
    const uint64_t size = impl.Resources[buffer.Index].Def().BufferDesc.Size;
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

Nullable<render::ComputePipelineState*> RenderGraph::ResolveComputePipeline(uint32_t pass, ShaderProgram& program) {
    auto& impl = *_impl;
    const render::ShaderStages stages = ProgramStages(program);
    if (program.GetDevice() != &impl.Device || !stages.HasFlag(render::ShaderStage::Compute) ||
        stages.HasFlag(render::ShaderStage::Graphics)) {
        impl.Error("ComputeProgram", "Compute passes require a compute-only ShaderProgram from this graph's device", pass);
        return nullptr;
    }
    const Nullable<render::ComputePipelineState*> pipeline = program.GetOrCreateComputePipelineState();
    if (!pipeline) impl.Error("ComputePipelineState", "Compute pipeline state creation failed before recording", pass);
    return pipeline;
}

Nullable<render::GraphicsPipelineState*> RenderGraph::ResolveGraphicsPipeline(
    uint32_t pass, ShaderProgram& program, const MaterialPipelineState& state,
    const PrimitiveVertexLayout& layout, PrimitiveTopology topology) {
    auto& impl = *_impl;
    const render::ShaderStages stages = ProgramStages(program);
    if (program.GetDevice() != &impl.Device || !stages.HasFlag(render::ShaderStage::Vertex) ||
        stages.HasFlag(render::ShaderStage::Compute)) {
        impl.Error("GraphicsProgram", "Raster passes require a graphics ShaderProgram from this graph's device", pass);
        return nullptr;
    }
    // A merged raster pass shares the group head's realized formats, so any member of the group
    // resolves against the same GraphicsPassState.
    const auto& passState = impl.Passes[pass].PassState;
    if (impl.Passes[pass].Def().Type != RgPassType::Raster || !passState) {
        impl.Error("GraphicsProgram", "Graphics pipeline states require a realized raster pass", pass);
        return nullptr;
    }
    const size_t before = program.GetGraphicsPipelineStateCount();
    const Nullable<render::GraphicsPipelineState*> pipeline =
        program.GetOrCreateGraphicsPipelineState(state, layout, topology, *passState);
    ++impl.Report.GraphicsPipelinePreparations;
    impl.Report.GraphicsPipelineCreations += static_cast<uint32_t>(program.GetGraphicsPipelineStateCount() - before);
    if (!pipeline) impl.Error("GraphicsPipelineState", "Graphics pipeline state creation failed before recording", pass);
    return pipeline;
}

PreparedShaderGroup RenderGraph::CreateParameterSet(uint32_t pass, ShaderProgram& program, uint32_t group,
                                                    std::span<const RgParameterBinding> bindings) {
    auto& impl = *_impl;
    const auto fail = [&](std::string_view code, std::string_view message,
                          std::string_view declaration = {}, uint32_t resource = InvalidIndex) {
        impl.Error(code, message, pass, resource, declaration);
        return PreparedShaderGroup{};
    };
    if (impl.ValidatingReady)
        return fail("ParameterPreparation", "Ready validation cannot create new recording data");
    if (pass >= impl.Passes.size() || program.GetDevice() != &impl.Device)
        return fail("ParameterProgram", "Parameter sets require a ShaderProgram from this graph's device");
    const RgPassType passType = impl.Passes[pass].Def().Type;
    const render::ShaderStages programStages = ProgramStages(program);
    const bool stageCompatible =
        (passType == RgPassType::Raster && programStages.HasFlag(render::ShaderStage::Graphics) &&
         !programStages.HasFlag(render::ShaderStage::Compute)) ||
        (passType == RgPassType::Compute && programStages.HasFlag(render::ShaderStage::Compute) &&
         !programStages.HasFlag(render::ShaderStage::Graphics));
    if (!stageCompatible)
        return fail("ParameterProgram", "The parameter program's shader stages do not match the pass type");
    render::PipelineLayout* layout = program.GetPipelineLayout();
    if (layout == nullptr)
        return fail("ParameterProgram", "The parameter program has no pipeline layout");
    if (!impl.FrameResources || !impl.FrameResources->_impl->Arena || !impl.FrameResources->_impl->Arena->IsValid())
        return fail("ParameterStorage", "Graph parameter sets require initialized per-flight frame resources");
    RenderGraphFrameResources::Impl& frame = *impl.FrameResources->_impl;
    const bool full = impl.ValidationFull();
    Impl::ParameterRequest request;
    if (full) {
        request.Program = &program;
        request.Pass = pass;
        request.Uses.reserve(bindings.size());
    }

    PreparedShaderGroup result{};
    result.Group = group;
    // The key is workspace-owned scratch: a steady-state frame reuses its storage and only a cache
    // miss copies it into the map.
    RenderGraphFrameResources::Impl::ParameterSetKey& key = frame.KeyScratch;
    key.Layout = layout;
    key.Group = group;
    key.Values.clear();
    key.Values.reserve(bindings.size());

    const auto findCBuffer = [&](std::string_view declaration) -> const ShaderParameterBufferLayout* {
        for (const ShaderParameterBufferLayout& buffer : program.GetParameterLayout().Buffers())
            if (buffer.Name == declaration) return &buffer;
        return nullptr;
    };
    bool groupKnown = false;
    for (const RgParameterBinding& source : bindings) {
        const std::string_view declaration = source.Declaration;
        Impl::ParameterUse use;
        if (full) use.Declaration = declaration;
        const std::optional<render::ShaderBindingInfo> info = program.GetArtifact().FindBindingInfo(declaration);
        if (declaration.empty() || !info.has_value())
            return fail("ParameterDeclaration", "Binding is not a canonical descriptor declaration in this program", declaration);
        if (info->Group != group)
            return fail("ParameterGroup", "Binding belongs to a different parameter group", declaration);
        groupKnown = true;
        if (info->Immutable)
            return fail("ImmutableBinding", "Static or immutable samplers must not be supplied by the caller", declaration);
        if (source.ArrayElement >= info->Count)
            return fail("ParameterArrayElement", "Binding array element is outside the declaration count", declaration);
        const render::BindingHandle handle = layout->FindBinding(declaration);
        if (!handle.IsValid())
            return fail("ParameterBinding", "Resolved pipeline binding is unavailable during preparation", declaration);
        const shader::ShaderBindingKind kind = info->LogicalKind;
        render::ShaderParameterValue value;
        if (const auto* bytes = std::get_if<RgCBufferParameterBinding>(&source.Value)) {
            const ShaderParameterBufferLayout* cbuffer = findCBuffer(declaration);
            if (kind != shader::ShaderBindingKind::CBuffer || source.ArrayElement != 0 || cbuffer == nullptr ||
                cbuffer->Group != group || bytes->Bytes.size() != cbuffer->Size)
                return fail("ParameterType", "Copied cbuffer bytes must exactly match a scalar cbuffer declaration", declaration);
            DynamicCBufferArena::Reservation reservation = frame.Arena->Reserve(bytes->Bytes.size());
            if (!reservation.IsValid())
                return fail("ParameterUpload", "Constant upload allocation failed before recording", declaration);
            std::memcpy(reservation.Data(), bytes->Bytes.data(), bytes->Bytes.size());
            const DynamicCBufferArena::Allocation allocation = reservation.Commit(bytes->Bytes.size());
            if (!allocation.IsValid() || (info->Dynamic && allocation.Offset > std::numeric_limits<uint32_t>::max()))
                return fail("ParameterUpload", "Constant upload commit or dynamic offset conversion failed", declaration);
            value = render::ShaderBufferBinding{allocation.Target, {info->Dynamic ? 0 : allocation.Offset, allocation.Size}, 0};
            if (info->Dynamic) result.DynamicOffsets.push_back({handle, static_cast<uint32_t>(allocation.Offset)});
        } else if (const auto* texture = std::get_if<RgTextureParameterBinding>(&source.Value)) {
            if (!shader::IsImageKind(kind))
                return fail("ParameterType", "Texture value does not match the shader declaration", declaration);
            const RgTextureViewHandle view = impl.PatchTemplateView(pass, texture->View);
            if (view.Generation != impl.Generation || view.Index >= impl.Views.size())
                return fail("ParameterUndeclared", "Texture binding uses a view handle from another graph", declaration);
            const Impl::View& declared = impl.Views[view.Index];
            if (!declared.Native)
                return fail("ParameterTexture", "Graph texture view was not realized before parameter preparation",
                            declaration, declared.Resource);
            const bool writable = shader::IsWritableKind(kind);
            if (full) {
                use.Resource = declared.Resource;
                use.Version = declared.Version;
                use.View = view.Index;
                use.ViewUsage = writable ? render::TextureViewUsage::UnorderedAccess : render::TextureViewUsage::Resource;
                use.Read = !writable;
                use.Stages = info->Stages;
            }
            value = declared.Native.Get();
        } else if (const auto* buffer = std::get_if<RgBufferParameterBinding>(&source.Value)) {
            const bool bufferKind = kind == shader::ShaderBindingKind::CBuffer ||
                                    kind == shader::ShaderBindingKind::TypedBuffer ||
                                    kind == shader::ShaderBindingKind::RWTypedBuffer ||
                                    kind == shader::ShaderBindingKind::StructuredBuffer ||
                                    kind == shader::ShaderBindingKind::RWStructuredBuffer ||
                                    kind == shader::ShaderBindingKind::RawBuffer ||
                                    kind == shader::ShaderBindingKind::RWRawBuffer;
            if (!bufferKind)
                return fail("ParameterType", "Buffer value does not match the shader declaration", declaration, buffer->Buffer.Index);
            const bool writable = shader::IsWritableKind(kind);
            RgBufferValue resolved = impl.PatchTemplateBuffer(pass, buffer->Buffer);
            if (!impl.Handle(resolved.Index, resolved.Generation, false, pass)) return {};
            if (impl.Resources[resolved.Index].Def().Port) {
                const auto& values = impl.Resources[resolved.Index].Def().ResolvedValues;
                if (resolved.Version >= values.size() || values[resolved.Version].first == InvalidIndex)
                    return fail("ParameterUndeclared", "Buffer port was not connected", declaration, resolved.Index);
                const auto mapped = values[resolved.Version];
                resolved.Index = mapped.first;
                resolved.Version = mapped.second;
            }
            const uint64_t bufferSize = impl.Resources[resolved.Index].Def().BufferDesc.Size;
            if (buffer->Range.Offset > bufferSize)
                return fail("ParameterBufferRange", "Buffer parameter offset is outside the resource", declaration, resolved.Index);
            const uint64_t available = bufferSize - buffer->Range.Offset;
            const uint64_t size = buffer->Range.Size == render::BufferRange::All() ? available : buffer->Range.Size;
            if (size == 0 || size > available)
                return fail("ParameterBufferRange", "Buffer parameter range is empty or outside the resource", declaration, resolved.Index);
            const render::BufferRange range{buffer->Range.Offset, size};
            bool representationValid = buffer->Format == render::TextureFormat::UNKNOWN;
            if (kind == shader::ShaderBindingKind::CBuffer) {
                const ShaderParameterBufferLayout* cbuffer = findCBuffer(declaration);
                const uint64_t alignment = std::max<uint64_t>(1, impl.Device.GetCapabilities().Limits.CBufferOffsetAlignment);
                representationValid = representationValid && buffer->StructureByteStride == 0 &&
                                      cbuffer != nullptr && cbuffer->Group == group && range.Size == cbuffer->Size &&
                                      range.Offset % alignment == 0;
            } else if (kind == shader::ShaderBindingKind::StructuredBuffer ||
                       kind == shader::ShaderBindingKind::RWStructuredBuffer) {
                const uint64_t storageAlignment = std::max<uint64_t>(
                    1, impl.Device.GetCapabilities().Limits.StorageBufferOffsetAlignment);
                representationValid = representationValid && buffer->StructureByteStride != 0 &&
                                      buffer->StructureByteStride % 4 == 0 && buffer->StructureByteStride <= 2048 &&
                                      range.Offset % buffer->StructureByteStride == 0 &&
                                      range.Size % buffer->StructureByteStride == 0 &&
                                      range.Offset % storageAlignment == 0;
            } else if (kind == shader::ShaderBindingKind::RawBuffer ||
                       kind == shader::ShaderBindingKind::RWRawBuffer) {
                const uint64_t storageAlignment = std::max<uint64_t>(
                    1, impl.Device.GetCapabilities().Limits.StorageBufferOffsetAlignment);
                representationValid = representationValid && buffer->StructureByteStride == 0 &&
                                      range.Offset % 4 == 0 && range.Size % 4 == 0 &&
                                      range.Offset % storageAlignment == 0;
            } else {
                const uint32_t elementSize = render::GetTextureFormatBytesPerPixel(buffer->Format);
                representationValid = buffer->StructureByteStride == 0 && elementSize != 0 &&
                                      range.Offset % elementSize == 0 && range.Size % elementSize == 0;
            }
            if (!representationValid)
                return fail("ParameterBufferLayout", "Buffer range, stride or format is incompatible with the declaration",
                            declaration, resolved.Index);
            if (info->Dynamic && range.Offset > std::numeric_limits<uint32_t>::max())
                return fail("ParameterDynamicOffset", "Dynamic buffer offset exceeds the RHI offset width", declaration, resolved.Index);
            if (full) {
                const RgBufferAccess graphAccess = kind == shader::ShaderBindingKind::CBuffer
                                                       ? RgBufferAccess::Constant
                                                   : writable
                                                       ? RgBufferAccess::UnorderedAccess
                                                       : RgBufferAccess::ShaderRead;
                use.Resource = resolved.Index;
                use.Version = resolved.Version;
                use.State = static_cast<uint32_t>(BufferAccessInfo(graphAccess).first);
                use.Read = !writable;
                use.Stages = info->Stages;
                use.Bytes = range;
            }
            const auto& resource = impl.Resources[resolved.Index];
            if (!resource.ExternalBuffer && !resource.Readback && !resource.PoolBuffer)
                return fail("ParameterBuffer", "Graph buffer was not realized before parameter preparation", declaration, resolved.Index);
            render::Buffer* native = impl.Resources[resolved.Index].NativeBuffer();
            const uint64_t descriptorOffset = info->Dynamic ? 0 : range.Offset;
            if (shader::IsTexelBufferKind(kind))
                value = render::ShaderTexelBufferBinding{native, {descriptorOffset, range.Size}, buffer->Format};
            else
                value = render::ShaderBufferBinding{native, {descriptorOffset, range.Size}, buffer->StructureByteStride};
            if (info->Dynamic) result.DynamicOffsets.push_back({handle, static_cast<uint32_t>(range.Offset)});
        } else {
            const auto& sampler = std::get<RgSamplerParameterBinding>(source.Value);
            if (kind != shader::ShaderBindingKind::Sampler)
                return fail("ParameterType", "Sampler value does not match the shader declaration", declaration);
            const Nullable<render::Sampler*> native = impl.Device.GetOrCreateSampler(sampler.Sampler);
            if (!native)
                return fail("ParameterSampler", "Sampler creation failed before recording", declaration);
            value = native.Get();
        }
        key.Values.push_back({handle, source.ArrayElement, std::move(value)});
        if (full) request.Uses.push_back(std::move(use));
    }

    if (!groupKnown) {
        const shader::ShaderArtifactView& artifact = program.GetArtifact().Generic();
        for (const shader::WireBindingRecord& binding : artifact.Bindings()) {
            const std::optional<std::string_view> name = artifact.GetName(binding.Name);
            if (!name.has_value()) continue;
            const std::optional<render::ShaderBindingInfo> info = program.GetArtifact().FindBindingInfo(name.value());
            if (info.has_value() && info->Group == group) {
                groupKnown = true;
                break;
            }
        }
        if (!groupKnown)
            return fail("ParameterGroup", "The program has no descriptor declarations in this parameter group");
    }
    if (full) {
        request.Key = key;
        impl.ParameterRequests.push_back(std::move(request));
    }

    const auto cached = frame.SetCache.find(key);
    if (cached != frame.SetCache.end()) {
        result.Set = cached->second;
        return result;
    }
    if (full) {
        const auto pending = impl.PendingParameterSets.find(key);
        if (pending != impl.PendingParameterSets.end()) {
            result.Set = pending->second.Set.get();
            return result;
        }
    }
    Nullable<unique_ptr<render::ShaderParameterSet>> created =
        impl.Device.CreateShaderParameterSet({.Layout = layout, .GroupIndex = group});
    if (!created)
        return fail("ParameterSetAllocation", "Parameter set allocation failed before recording");
    unique_ptr<render::ShaderParameterSet> set = created.Release();
    if (full) {
        result.Set = set.get();
        impl.PendingParameterSets.emplace(key, Impl::PendingParameterSet{std::move(set), pass});
        return result;
    }
    for (const RenderGraphFrameResources::Impl::ParameterValue& binding : key.Values)
        if (!set->Set(binding.Handle, binding.ArrayElement, binding.Value))
            return fail("ParameterSetWrite", "Parameter set rejected a validated binding value");
    if (!set->FlushWrites())
        return fail("ParameterSetFlush", "Parameter set descriptor writes failed before recording");
    result.Set = set.get();
    frame.Sets.push_back(std::move(set));
    frame.SetCache.emplace(key, result.Set.Get());
    return result;
}

bool RenderGraph::Impl::ValidateParameterRequests() {
    for (const auto& request : ParameterRequests) {
        const auto& key = request.Key;
        const auto& pass = Passes[request.Pass];
        const auto fail = [&](std::string_view code, std::string_view message, std::string_view declaration, uint32_t resource = InvalidIndex) {
            Error(code, message, request.Pass, resource, declaration);
            return false;
        };
        for (size_t i = 0; i < key.Values.size(); ++i) {
            const auto& value = key.Values[i];
            const auto& use = request.Uses[i];
            for (size_t earlier = 0; earlier < i; ++earlier)
                if (key.Values[earlier].Handle == value.Handle && key.Values[earlier].ArrayElement == value.ArrayElement)
                    return fail("DuplicateParameterBinding", "The same binding array element was supplied more than once", use.Declaration);
            if (use.Resource == InvalidIndex) continue;
            if (use.View != InvalidIndex &&
                (Views[use.View].Key.Usage != use.ViewUsage || !pass.NativeAccess->Find(use.View, true)))
                return fail("ParameterUndeclared", "Texture binding needs a matching view declared by this pass", use.Declaration, use.Resource);
            // An RW descriptor may be read-only. Only sampled/read-only kinds require a read;
            // the declared state, byte range and stages must cover both kinds.
            const bool covered = std::any_of(pass.GetAccesses().begin(), pass.GetAccesses().end(), [&](const Access& access) {
                if (access.Resource != use.Resource || access.Version != use.Version || (use.Read && !access.Read) ||
                    (access.State & use.State) != use.State || (use.Stages.value() & ~access.Stages.value()) != 0) return false;
                if (use.View != InvalidIndex) {
                    const auto& range = Views[use.View].Key.Range;
                    return access.Range.BaseArrayLayer <= range.BaseArrayLayer &&
                           access.Range.BaseArrayLayer + access.Range.ArrayLayerCount >= range.BaseArrayLayer + range.ArrayLayerCount &&
                           access.Range.BaseMipLevel <= range.BaseMipLevel &&
                           access.Range.BaseMipLevel + access.Range.MipLevelCount >= range.BaseMipLevel + range.MipLevelCount &&
                           (range.Aspects.value() & ~access.Range.Aspects.value()) == 0;
                }
                const uint64_t end = access.Bytes.Size == render::BufferRange::All() ? Resources[use.Resource].Def().BufferDesc.Size : access.Bytes.Offset + access.Bytes.Size;
                return access.Bytes.Offset <= use.Bytes.Offset && end >= use.Bytes.Offset + use.Bytes.Size;
            });
            if (!covered)
                return fail("ParameterUndeclared", "Resource binding has no declaration covering its access state, range and stages", use.Declaration, use.Resource);
        }
        const auto& program = *request.Program;
        const auto& artifact = program.GetArtifact().Generic();
        for (const shader::WireBindingRecord& record : artifact.Bindings()) {
            const auto name = artifact.GetName(record.Name);
            if (!name) continue;
            const auto info = program.GetArtifact().FindBindingInfo(*name);
            if (!info || info->Group != key.Group || info->Immutable) continue;
            const auto handle = key.Layout->FindBinding(*name);
            for (uint32_t element = 0; element < info->Count; ++element) {
                const bool found = std::any_of(key.Values.begin(), key.Values.end(), [&](const auto& value) {
                    return value.Handle == handle && value.ArrayElement == element;
                });
                if (!found)
                    return fail("MissingParameterBinding", fmt::format("Required binding array element {} is missing", element), *name);
            }
        }
    }
    return true;
}

bool RenderGraph::Impl::PublishParameterSets() {
    if (PendingParameterSets.empty()) return true;
    RADRAY_PROFILE_SCOPE_N("RenderGraph::PublishParameterSets");
    // Full drafts remain graph-owned through all diagnostic checks and native writes. No failed
    // or partially flushed draft can enter the per-flight cache, even when another draft succeeded.
    for (const auto& [key, pending] : PendingParameterSets) {
        for (const auto& binding : key.Values) {
            if (!pending.Set->Set(binding.Handle, binding.ArrayElement, binding.Value)) {
                Error("ParameterSetWrite", "Parameter set rejected a validated binding value", pending.Pass);
                return false;
            }
        }
        if (!pending.Set->FlushWrites()) {
            Error("ParameterSetFlush", "Parameter set descriptor writes failed before recording", pending.Pass);
            return false;
        }
    }
    auto& frame = *FrameResources->_impl;
    for (auto& [key, pending] : PendingParameterSets) {
        auto* native = pending.Set.get();
        frame.Sets.push_back(std::move(pending.Set));
        frame.SetCache.emplace(key, native);
    }
    return true;
}
RgTextureViewHandle RenderGraphRasterBuilder::SetColorAttachment(uint32_t slot, RgTextureValue texture, const RgColorAttachmentDesc& desc) {
    auto& impl = *_graph._impl;
    if (slot >= 8 || (slot < impl.Passes[_pass].Def().Colors.size() && impl.Passes[_pass].Def().Colors[slot])) {
        impl.Error("InvalidAttachmentSlot", "Color slots must be unique and less than 8", _pass);
        return {};
    }
    auto result = _graph.UseTexture(_pass, texture, desc.View, render::TextureViewUsage::RenderTarget, desc.Load == render::LoadAction::Load, true, desc.Store == render::StoreAction::Store);
    if (result.IsValid()) {
        auto& colors = impl.Passes[_pass].Edit().Colors;
        if (slot >= colors.size()) colors.resize(slot + 1);
        colors[slot] = RenderGraph::Impl::Color{result.Index, desc};
    }
    return result;
}
RgTextureViewHandle RenderGraphRasterBuilder::SetDepthAttachment(RgTextureValue texture, const RgDepthAttachmentDesc& desc) {
    auto& impl = *_graph._impl;
    if (impl.Passes[_pass].Def().DepthAttachment || (desc.ReadOnly && (desc.Load != render::LoadAction::Load || desc.Store != render::StoreAction::Store))) {
        impl.Error("InvalidDepthAttachment", "Depth is already bound, or read-only depth would clear/discard", _pass);
        return {};
    }
    auto result = _graph.UseTexture(_pass, texture, desc.View, desc.ReadOnly ? render::TextureViewUsage::DepthRead : render::TextureViewUsage::DepthWrite,
                                    desc.Load == render::LoadAction::Load, !desc.ReadOnly, desc.Store == render::StoreAction::Store);
    if (result.IsValid()) impl.Passes[_pass].Edit().DepthAttachment = RenderGraph::Impl::Depth{result.Index, desc};
    return result;
}

RgPassHandle RenderGraph::AddCopyBufferPass(std::string_view name, RgBufferValue source, RgBufferValue destination,
                                            uint64_t size, uint64_t sourceOffset, uint64_t destinationOffset, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    if (!pass.IsValid()) return pass;
    if (!UseBuffer(pass.Index, source, RgBufferAccess::CopySource, true, false).IsValid() ||
        !UseBuffer(pass.Index, destination, RgBufferAccess::CopyDestination, false, true).IsValid()) return pass;
    const auto sourceSize = _impl->Resources[source.Index].Def().BufferDesc.Size;
    const auto destinationSize = _impl->Resources[destination.Index].Def().BufferDesc.Size;
    if (size == 0 || sourceOffset > sourceSize || size > sourceSize - sourceOffset || destinationOffset > destinationSize || size > destinationSize - destinationOffset) {
        _impl->Error("CopyRange", "Buffer copy range exceeds its source or destination", pass.Index);
        return pass;
    }
    _impl->Passes[pass.Index].Edit().Accesses[0].Bytes = {sourceOffset, size};
    _impl->Passes[pass.Index].Edit().Accesses[1].Bytes = {destinationOffset, size};
    _impl->Passes[pass.Index].Edit().CopyOp = Impl::Copy{Impl::CopyType::Buffer, source.Index, destination.Index, size, sourceOffset, destinationOffset};
    return pass;
}
RgPassHandle RenderGraph::AddCopyTexturePass(std::string_view name, RgTextureValue source, RgTextureValue destination,
                                             render::SubresourceRange sourceRange, render::SubresourceRange destinationRange, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) || !impl.Handle(destination.Index, destination.Generation, true, pass.Index)) return pass;
    const auto& src = impl.Resources[source.Index].Def().TextureDesc;
    const auto& dst = impl.Resources[destination.Index].Def().TextureDesc;
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
    impl.Passes[pass.Index].Edit().Accesses.push_back({source.Index, *sr, static_cast<uint32_t>(render::TextureState::CopySource), true, false, true, source.Version});
    impl.Passes[pass.Index].Edit().Accesses.push_back({destination.Index, *dr, static_cast<uint32_t>(render::TextureState::CopyDestination), false, true, true, destination.Version});
    impl.Passes[pass.Index].Edit().CopyOp = Impl::Copy{Impl::CopyType::Texture, source.Index, destination.Index, 0, 0, 0, *sr, *dr};
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
    const auto& src = impl.Resources[source.Index].Def().TextureDesc;
    const auto& dst = impl.Resources[destination.Index].Def().TextureDesc;
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
    impl.Passes[pass.Index].Edit().Accesses.push_back({source.Index, *sr, static_cast<uint32_t>(render::TextureState::ResolveSource), true, false, true, source.Version});
    impl.Passes[pass.Index].Edit().Accesses.push_back({destination.Index, *dr, static_cast<uint32_t>(render::TextureState::ResolveDestination), false, true, true, destination.Version});
    impl.Passes[pass.Index].Edit().CopyOp = Impl::Copy{Impl::CopyType::Resolve, source.Index, destination.Index, 0, 0, 0, *sr, *dr};
    return pass;
}
RgPassHandle RenderGraph::AddCopyTextureToBufferPass(std::string_view name, RgTextureValue source, RgBufferValue destination,
                                                     render::SubresourceRange range, uint64_t destinationOffset, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) ||
        !UseBuffer(pass.Index, destination, RgBufferAccess::CopyDestination, false, true).IsValid()) return pass;
    const auto& src = impl.Resources[source.Index].Def().TextureDesc;
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
    const auto destinationSize = impl.Resources[destination.Index].Def().BufferDesc.Size;
    if (destinationOffset > destinationSize || size > destinationSize - destinationOffset) {
        impl.Error("CopyRange", "Readback buffer is too small for the aligned texture footprint", pass.Index);
        return pass;
    }
    impl.Passes[pass.Index].Edit().Accesses.back().Bytes = {destinationOffset, size};
    impl.Passes[pass.Index].Edit().Accesses.push_back({source.Index, *normalized, static_cast<uint32_t>(render::TextureState::CopySource), true, false, true, source.Version});
    impl.Passes[pass.Index].Edit().CopyOp = Impl::Copy{Impl::CopyType::TextureToBuffer, source.Index, destination.Index, size, 0, destinationOffset, *normalized};
    return pass;
}

RgPassHandle RenderGraph::AddCopyBufferToTexturePass(std::string_view name, RgBufferValue source, RgTextureValue destination,
                                                     const render::BufferTextureCopyRegion& region, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(destination.Index, destination.Generation, true, pass.Index) ||
        !UseBuffer(pass.Index, source, RgBufferAccess::CopySource, true, false).IsValid()) return pass;
    const auto& dst = impl.Resources[destination.Index].Def().TextureDesc;
    const auto validation = render::ValidateBufferTextureCopyRegion(impl.Resources[source.Index].Def().BufferDesc, dst, region, impl.Device.GetDetail());
    if (!validation.Supported) {
        impl.Error("CopyBufferTextureRegion", validation.Reason, pass.Index);
        return pass;
    }
    const bool partial = region.X != 0 || region.Y != 0 || region.Width != std::max(1u, dst.Width >> region.MipLevel) ||
                         region.Height != std::max(1u, dst.Height >> region.MipLevel);
    const render::SubresourceRange range{region.ArrayLayer, 1, region.MipLevel, 1};
    impl.Passes[pass.Index].Edit().Accesses.back().Bytes = {region.SourceOffset, uint64_t{region.RowPitch} * (region.Height - 1) + uint64_t{region.Width} * render::GetTextureFormatBytesPerPixel(dst.Format)};
    impl.Passes[pass.Index].Edit().Accesses.push_back({destination.Index, range, static_cast<uint32_t>(render::TextureState::CopyDestination), partial, true, true, destination.Version});
    Impl::Copy copy{Impl::CopyType::BufferToTexture, source.Index, destination.Index};
    copy.Upload = region;
    impl.Passes[pass.Index].Edit().CopyOp = copy;
    return pass;
}

bool RenderGraph::Impl::ValidateResources() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::ValidateResources");
    const bool reportFull = ReportFull();
    if (reportFull) Report.Resources.reserve(Resources.size());
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        string descriptor;
        if (resource.Def().IsTexture) {
            ++Report.Textures;
            auto desc = resource.Def().TextureDesc;
            if (resource.External() || resource.Def().Port) desc.Hints = desc.Hints & render::ResourceHint::Dedicated;
            const auto validation = render::ValidateTextureDescriptor(desc, Device);
            if (!validation.Supported) {
                Error("UnsupportedTexture", validation.Reason, InvalidIndex, index);
                continue;
            }
            if (reportFull) descriptor = fmt::format("{} {}x{}x{} mips={} samples={} usage={}", desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Usage);
            resource.Valid.assign(resource.CellCount(), 0);
            if (resource.ExternalTexture) {
                const auto& external = *resource.ExternalTexture;
                if (external.SubresourceStates.size() != resource.SubresourceCount() || (external.ContentValid.size() != resource.CellCount() && external.ContentValid.size() != resource.SubresourceCount())) {
                    Error("ExternalStorage", "External descriptor or state/validity storage does not match native texture", InvalidIndex, index);
                    continue;
                }
                for (uint32_t cell = 0; cell < resource.CellCount(); ++cell) resource.Valid[cell] = external.ContentValid[cell % external.ContentValid.size()];
            }
        } else {
            ++Report.Buffers;
            const auto& desc = resource.Def().BufferDesc;
            constexpr uint32_t knownUses = 2047;
            if (desc.Size == 0 || desc.Size > Device.GetCapabilities().Limits.MaxBufferSize || !EnumContains(desc.Memory) || !desc.Usage ||
                (desc.Usage.value() & ~knownUses) != 0 || (desc.Hints.value() & ~uint32_t{7}) != 0 ||
                (!resource.External() && !resource.Def().Port && desc.Hints.HasFlag(render::ResourceHint::External)) ||
                (desc.Memory == render::MemoryType::Upload && !desc.Usage.HasFlag(render::BufferUse::MapWrite)) ||
                (desc.Memory == render::MemoryType::ReadBack && !desc.Usage.HasFlag(render::BufferUse::MapRead))) {
                Error("UnsupportedBuffer", "Invalid buffer size, usage, memory or hints", InvalidIndex, index);
                continue;
            }
            if (reportFull) descriptor = fmt::format("size={} memory={} usage={}", desc.Size, EnumName(desc.Memory), desc.Usage);
            resource.Valid.assign(resource.CellCount(), resource.ExternalBuffer && resource.ExternalBuffer->ContentValid ? 1 : 0);
            if (resource.ExternalBuffer && resource.Valid[0] &&
                (!(BufferPoolKey{resource.ExternalBuffer->Buffer->GetDesc()} == BufferPoolKey{desc}) || !resource.ExternalBuffer->State ||
                 resource.ExternalBuffer->State.HasFlag(render::BufferState::Undefined))) {
                Error("ExternalStorage", "External buffer descriptor or initial state is invalid", InvalidIndex, index);
            }
        }
        if (reportFull) {
            Report.Resources.push_back({string{resource.Name()}, std::move(descriptor), resource.Def().IsTexture, resource.External()});
            Report.Resources.back().ViewId = resource.ViewId();
            Report.Resources.back().Port = resource.Def().Port;
            Report.Resources.back().RetainedOwner = resource.ExternalTexture ? bool(resource.ExternalTexture->Owner) : resource.ExternalBuffer ? bool(resource.ExternalBuffer->Owner)
                                                                                                                                               : false;
            Report.Resources.back().EstimatedBytes = resource.Def().IsTexture ? EstimateTextureBytes(resource.Def().TextureDesc) : resource.Def().BufferDesc.Size;
        }
    }
    return !Failed;
}

bool RenderGraph::Impl::NormalizePasses() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::NormalizePasses");
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        auto& pass = Passes[p];
        unordered_map<uint64_t, uint32_t> cellMap;
        vector<uint32_t> cells;
        for (const auto& access : pass.Def().Accesses) {
            auto& resource = Resources[access.Resource];
            if (access.Write && resource.External() && resource.AccessMode() == RenderGraphExternalAccess::ReadOnly) Error("ReadOnlyExternal", "Cannot write a read-only external resource", p, access.Resource);
            cells.clear();
            if (resource.Def().IsTexture) {
                for (uint32_t aspect = 0; aspect < resource.AspectCount(); ++aspect) {
                    const auto bit = aspect == 1 ? render::TextureAspect::Stencil : render::IsDepthStencilFormat(resource.Def().TextureDesc.Format) ? render::TextureAspect::Depth
                                                                                                                                                    : render::TextureAspect::Color;
                    if (access.Range.Aspects && !access.Range.Aspects.HasFlag(bit)) continue;
                    for (uint32_t layer = access.Range.BaseArrayLayer; layer < access.Range.BaseArrayLayer + access.Range.ArrayLayerCount; ++layer)
                        for (uint32_t mip = access.Range.BaseMipLevel; mip < access.Range.BaseMipLevel + access.Range.MipLevelCount; ++mip)
                            cells.push_back(aspect * resource.SubresourceCount() + layer * resource.Def().TextureDesc.MipLevels + mip);
                }
            } else {
                for (uint32_t c = 0; c < resource.CellCount(); ++c)
                    if (resource.Def().BufferBoundaries[c] >= access.Bytes.Offset && resource.Def().BufferBoundaries[c + 1] <= access.Bytes.Offset + access.Bytes.Size)
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
                        const auto uav = resource.Def().IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
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
            if (Resources[cell.Resource].Def().IsTexture && !ValidTextureState(Resources[cell.Resource].Def().TextureDesc, static_cast<render::TextureState>(cell.State), false))
                Error("IncompatibleTextureStates", "Texture access requires an invalid usage or incompatible layouts", p, cell.Resource);
        // The current Direct-queue RHI tracks a whole buffer state. Content validity
        // remains byte-granular, but simultaneous disjoint accesses must share one
        // legal native state. Never widen a write layout into an incompatible read.
        unordered_map<uint32_t, uint32_t> bufferStates;
        for (const auto& cell : pass.Cells) {
            if (Resources[cell.Resource].Def().IsTexture) continue;
            auto [it, inserted] = bufferStates.emplace(cell.Resource, cell.State);
            constexpr uint32_t exclusive = uint32_t(render::BufferState::UnorderedAccess) | uint32_t(render::BufferState::CopyDestination) | uint32_t(render::BufferState::HostRead);
            if (!inserted && it->second != cell.State && ((it->second | cell.State) & exclusive))
                Error("IncompatibleBufferStates", "Disjoint buffer ranges require incompatible whole-buffer states in one pass", p, cell.Resource);
            it->second |= cell.State;
        }
        for (auto& cell : pass.Cells)
            if (!Resources[cell.Resource].Def().IsTexture) cell.State = bufferStates[cell.Resource];
        unordered_map<uint64_t, uint32_t> textureStates;
        for (const auto& cell : pass.Cells) {
            const auto& resource = Resources[cell.Resource];
            if (!resource.Def().IsTexture || resource.AspectCount() == 1) continue;
            const uint64_t key = uint64_t(cell.Resource) << 32 | resource.PhysicalCell(cell.Cell);
            auto [it, inserted] = textureStates.emplace(key, cell.State);
            if (!inserted && it->second != cell.State) Error("IncompatibleAspectStates", "Depth and stencil share a native layout; incompatible simultaneous aspect accesses are unsupported", p, cell.Resource);
        }
        if (pass.Def().Type != RgPassType::Raster) continue;
        const auto checkAttachment = [&](uint32_t viewIndex, render::LoadAction load, render::StoreAction store) {
            const auto& view = Views[viewIndex];
            const auto& desc = Resources[view.Resource].Def().TextureDesc;
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
        for (const auto& color : pass.Def().Colors) {
            if (!color)
                Error("AttachmentHole", "Color attachment slots must be contiguous", p);
            else
                checkAttachment(color->View, color->Desc.Load, color->Desc.Store);
        }
        if (pass.Def().DepthAttachment) checkAttachment(pass.Def().DepthAttachment->View, pass.Def().DepthAttachment->Desc.Load, pass.Def().DepthAttachment->Desc.Store);
        if (pass.Width == 0) Error("MissingAttachment", "Raster passes require at least one attachment", p);
    }
    return !Failed;
}

bool RenderGraph::Impl::ValidatePlanInput() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::ValidatePlanInput");
    ++Report.ValidatePlanInputCalls;
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        if (resource.Def().IsTexture) {
            if (resource.ExternalTexture) {
                const auto& external = *resource.ExternalTexture;
                if (!(TexturePoolKey{external.Texture->GetDesc()} == TexturePoolKey{external.Desc}))
                    Error("ExternalStorage", "External descriptor or state/validity storage does not match native texture", InvalidIndex, index);
                for (size_t cell = 0; cell < external.SubresourceStates.size(); ++cell) {
                    const auto state = external.SubresourceStates[cell];
                    const bool allowUndefined = cell < resource.Valid.size() && !resource.Valid[cell];
                    if (!ValidTextureState(external.Desc, state, allowUndefined))
                        Error("ExternalState", "Valid external contents require a defined state", InvalidIndex, index);
                }
            }
        } else if (resource.ExternalBuffer) {
            const bool validContent = !resource.Valid.empty() && resource.Valid[0] != 0;
            if (!validContent &&
                (!(BufferPoolKey{resource.ExternalBuffer->Buffer->GetDesc()} == BufferPoolKey{resource.Def().BufferDesc}) ||
                 !resource.ExternalBuffer->State))
                Error("ExternalStorage", "External buffer descriptor or initial state is invalid", InvalidIndex, index);
        }
        if (!EnumContains(resource.AccessMode())) Error("ExternalAccess", "Invalid external access mode", InvalidIndex, index);
    }
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        for (const auto& access : Passes[p].GetAccesses()) {
            if ((access.Stages.value() & ~uint32_t{7}) != 0)
                Error("ShaderStages", "Access contains unsupported shader-stage bits", p, access.Resource);
        }
    }
    return !Failed;
}

void RenderGraph::Impl::Cull() {
    GraphCompileWorkspace localWorkspace;
    auto& workspace = FrameResources ? FrameResources->_impl->CompileWorkspace : localWorkspace;
    auto& versions = workspace.Versions;
    auto& nodes = workspace.Nodes;
    auto& values = workspace.Values;
    auto& producers = workspace.Producers;
    auto& initialized = workspace.Initialized;
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::BuildIR");
        ++Report.IrBuilds;
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
            const auto count = Resources[r].Def().VersionParents.size();
            values[r].resize(count);
            producers[r].resize(count);
            initialized[r].resize(count);
            for (auto& cells : producers[r]) cells.assign(Resources[r].CellCount(), InvalidIndex);
            for (auto& cells : initialized[r]) cells.assign(Resources[r].CellCount(), 0);
        }
        for (uint32_t p = 0; p < Passes.size(); ++p) {
            nodes[p].SideEffect = Passes[p].Def().SideEffect;
            for (const auto& access : Passes[p].GetCells()) {
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
        if (Failed) return;
        for (uint32_t r = 0; r < Resources.size(); ++r) {
            const auto& resource = Resources[r];
            for (uint32_t v = 0; v < values[r].size(); ++v) {
                auto& cells = values[r][v];
                cells.resize(resource.CellCount());
                for (uint32_t c = 0; c < resource.CellCount(); ++c) {
                    const uint32_t predecessor = v == 0 ? InvalidIndex : values[r][resource.Def().VersionParents[v]][c];
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
            for (const auto& access : Passes[p].GetCells()) {
                const auto& resource = Resources[access.Resource];
                const auto version = access.Write ? resource.Def().VersionParents[access.Version] : access.Version;
                if (access.Read) AddUnique(nodes[p].Reads, values[access.Resource][version][access.Cell]);
                if (access.Write) AddUnique(nodes[p].Writes, values[access.Resource][access.Version][access.Cell]);
            }
        }
        auto& roots = workspace.Roots;
        roots.clear();
        for (uint32_t r = 0; r < Resources.size(); ++r)
            if (Resources[r].AccessMode() == RenderGraphExternalAccess::ObservableOutput)
                for (const auto v : values[r].back())
                    if (versions[v].Producer != InvalidIndex) roots.push_back(v);
    }
    auto& roots = workspace.Roots;
    RADRAY_PROFILE_SCOPE_N("RenderGraph::CompilePlanMiss");
    ++Report.TopologyBuilds;
    CompiledGraph = CompileRenderGraph(static_cast<uint32_t>(Resources.size()), versions, nodes, roots, Options, workspace.Compiler);
    ApplyCompiledReport();
}

void RenderGraph::Impl::ApplyCompiledReport() {
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::ApplyCompiledPlan");
        for (const auto& diagnostic : GetCompiled().Diagnostics)
            Error(diagnostic.Code, diagnostic.Message, diagnostic.Pass, diagnostic.Resource);
        const bool reportFull = ReportFull();
        for (uint32_t p = 0; p < Passes.size(); ++p) {
            const auto& compiled = GetCompiled().Passes[p];
            auto& pass = Passes[p];
            pass.Live = compiled.Live;
            if (reportFull) {
                auto& report = Report.Passes[p];
                report.Live = compiled.Live;
                report.DataDependencies = compiled.DataDependencies;
                report.HazardDependencies = compiled.HazardDependencies;
                report.LivenessReason = compiled.LivenessReason;
                report.Reads = compiled.Reads;
                report.Writes = compiled.Writes;
                report.Accesses.clear();
                for (const auto& access : pass.GetAccesses())
                    report.Accesses.push_back({access.Resource, access.Version, access.State, access.Range, access.Bytes, access.Stages, access.Read, access.Write});
                if (FramePlan) report.RasterGroup = pass.RasterGroup;
            }
            if (compiled.Live) ++Report.LivePasses;
        }
        for (uint32_t r = 0; r < Resources.size(); ++r) {
            Resources[r].FirstUse = GetCompiled().Lifetimes[r].FirstUse;
            Resources[r].LastUse = GetCompiled().Lifetimes[r].LastUse;
            if (reportFull) {
                Report.Resources[r].FirstUse = GetCompiled().Lifetimes[r].FirstUse;
                Report.Resources[r].LastUse = GetCompiled().Lifetimes[r].LastUse;
                if (FramePlan) Report.Resources[r].PhysicalSlot = Resources[r].Physical;
            }
        }
        if (ReportFull()) {
            Report.Versions = GetCompiled().Versions;
            Report.ExecutionOrder = GetCompiled().ExecutionOrder;
        }
        Report.CulledPasses = Report.DeclaredPasses - Report.LivePasses;
    }
}

void RenderGraph::SetCompileOptions(RenderGraphCompileOptions options) {
    if (_impl->Mutable()) _impl->Options = options;
}
const CompiledRenderGraph& RenderGraph::GetCompiledGraph() const noexcept { return _impl->GetCompiled(); }

vector<uint64_t> RenderGraph::Impl::BuildPlanKey() const {
    vector<uint64_t> key;
    key.reserve(Resources.size() * 24 + Passes.size() * 24);
    const auto add = [&](auto value) { key.push_back(static_cast<uint64_t>(value)); };
    const auto range = [&](const render::SubresourceRange& value) {
        add(value.BaseArrayLayer);
        add(value.ArrayLayerCount);
        add(value.BaseMipLevel);
        add(value.MipLevelCount);
        add(value.Aspects.value());
    };
    add(reinterpret_cast<uintptr_t>(&Device));
    add(Options.CullPasses);
    add(Options.ReuseResources);
    add(Options.MergeRasterPasses);
    add(Options.OptimizeAttachmentStores);
    add(Options.EliminateBarriers);
    add(Options.BatchBarriers);
    add(Templates.size());
    for (const auto& instance : Templates) {
        add(instance.Source->GetGeneration());
        add(instance.Placement->ResourceBase);
        add(instance.Placement->ViewBase);
        add(instance.Placement->PassBase);
        add(instance.Placement->IndirectBase);
        add(instance.Placement->WorkBase);
    }
    add(Resources.size());
    for (const auto& resource : Resources) {
        add(resource.TemplateInstance);
        add(resource.AccessMode());
        if (resource.TemplateInstance != InvalidIndex) {
            add(resource.External());
            if (resource.ExternalTexture) {
                add(resource.ExternalTexture->ContentValid.size());
                for (const auto valid : resource.ExternalTexture->ContentValid) add(valid);
            }
            if (resource.ExternalBuffer) add(resource.ExternalBuffer->ContentValid);
            add(resource.VersionTail.size());
            for (const auto parent : resource.VersionTail) add(parent);
            add(resource.ConnectionPatch.has_value());
            if (resource.ConnectionPatch) {
                add(resource.ConnectionPatch->first);
                add(resource.ConnectionPatch->second);
            }
            continue;
        }
        add(resource.Def().IsTexture);
        add(resource.Def().Port);
        add(resource.Def().Connection);
        add(resource.Def().ConnectionVersion);
        add(resource.Def().ExternalAccess);
        add(resource.External());
        add(bool(resource.Readback));
        add(resource.Def().VersionParents.size());
        for (const auto value : resource.Def().VersionParents) add(value);
        add(resource.Def().ResolvedValues.size());
        for (const auto& value : resource.Def().ResolvedValues) {
            add(value.first);
            add(value.second);
        }
        if (resource.Def().IsTexture) {
            const auto& desc = resource.Def().TextureDesc;
            add(desc.Dim);
            add(desc.Width);
            add(desc.Height);
            add(desc.DepthOrArraySize);
            add(desc.MipLevels);
            add(desc.SampleCount);
            add(desc.Format);
            add(desc.Memory);
            add(desc.Usage.value());
            add(desc.Hints.value());
            if (resource.ExternalTexture) {
                add(resource.ExternalTexture->ContentValid.size());
                for (const auto valid : resource.ExternalTexture->ContentValid) add(valid);
            }
        } else {
            const auto& desc = resource.Def().BufferDesc;
            add(desc.Size);
            add(desc.Memory);
            add(desc.Usage.value());
            add(desc.Hints.value());
            if (resource.ExternalBuffer) add(resource.ExternalBuffer->ContentValid);
        }
    }
    add(Works.size());
    add(Passes.size());
    for (const auto& pass : Passes) {
        add(pass.TemplateInstance);
        if (pass.TemplateInstance != InvalidIndex) continue;
        add(pass.Def().Type);
        add(pass.Def().SideEffect);
        add(pass.UploadBytes.size());
        add(bool(pass.UploadSource));
        add(bool(pass.Ticket._state));
        add(pass.Def().WorkRequirements.size());
        for (const auto& [work, mask] : pass.Def().WorkRequirements) {
            add(work);
            add(mask);
        }
        add(pass.Def().Accesses.size());
        for (const auto& access : pass.Def().Accesses) {
            add(access.Resource);
            add(access.Version);
            add(access.State);
            add(access.Read);
            add(access.Write);
            add(access.ValidAfter);
            add(access.Stages.value());
            range(access.Range);
            add(access.Bytes.Offset);
            add(access.Bytes.Size);
        }
        add(pass.Def().DeclaredViews.size());
        for (const auto value : pass.Def().DeclaredViews) add(value);
        add(pass.Def().DeclaredBuffers.size());
        for (const auto value : pass.Def().DeclaredBuffers) add(value);
        add(pass.Def().Colors.size());
        for (const auto& color : pass.Def().Colors) {
            add(color.has_value());
            if (color) {
                add(color->View);
                add(color->Desc.Load);
                add(color->Desc.Store);
            }
        }
        add(pass.Def().DepthAttachment.has_value());
        if (pass.Def().DepthAttachment) {
            const auto& depth = *pass.Def().DepthAttachment;
            add(depth.View);
            add(depth.Desc.Load);
            add(depth.Desc.Store);
            add(depth.Desc.ReadOnly);
        }
        add(pass.Def().CopyOp.has_value());
        if (pass.Def().CopyOp) {
            const auto& copy = *pass.Def().CopyOp;
            add(copy.Type);
            add(copy.Source);
            add(copy.Destination);
            add(copy.Size);
            add(copy.SourceOffset);
            add(copy.DestinationOffset);
            range(copy.SourceRange);
            range(copy.DestinationRange);
            add(copy.Upload.SourceOffset);
            add(copy.Upload.RowPitch);
            add(copy.Upload.MipLevel);
            add(copy.Upload.ArrayLayer);
            add(copy.Upload.X);
            add(copy.Upload.Y);
            add(copy.Upload.Width);
            add(copy.Upload.Height);
        }
    }
    add(Views.size());
    for (const auto& view : Views) {
        add(view.TemplateInstance);
        if (view.TemplateInstance != InvalidIndex) continue;
        add(view.Resource);
        add(view.Version);
        add(view.Key.Dimension);
        add(view.Key.Format);
        range(view.Key.Range);
        add(view.Key.Usage);
    }
    add(IndirectArgumentsRecords.size());
    for (const auto& indirect : IndirectArgumentsRecords) {
        add(indirect.TemplateInstance);
        if (indirect.TemplateInstance != InvalidIndex) continue;
        add(indirect.Pass);
        add(indirect.Resource);
        add(indirect.Command);
        add(indirect.Offset);
        add(indirect.Count);
    }
    return key;
}

bool RenderGraph::Impl::FindFramePlan(std::span<const uint64_t> key) {
    if (!FrameResources || !Options.ReuseCompiledPlan) return false;
    auto& cache = *FrameResources->_impl->Plans->_impl;
    HashCode hash;
    for (const auto value : key) hash.Add(value);
    for (auto& entry : cache.Entries) {
        if (entry.Hash != hash.ToHashCode() || entry.Key.size() != key.size() || !std::equal(entry.Key.begin(), entry.Key.end(), key.begin())) continue;
        RADRAY_PROFILE_SCOPE_N("RenderGraph::CompilePlanHit");
        entry.Used = ++cache.UseSerial;
        FramePlan = std::static_pointer_cast<const CompiledFramePlan>(entry.Plan);
        Report.CompilePlanReused = true;
        ApplyFramePlan();
        return true;
    }
    return false;
}

void RenderGraph::Impl::ApplyFramePlan() {
    const auto& plan = *FramePlan;
    Report.ExecutionPlanId = plan.Id;
    Report.ReusedResources = plan.ReusedResources;
    Report.MergedRasterPasses = plan.MergedRasterPasses;
    Report.DiscardedStores = plan.DiscardedStores;
    Report.LiveWorks = static_cast<uint32_t>(plan.LiveWork.size());
    ExecutionPlan.resize(Passes.size());
    for (uint32_t index = 0; index < Passes.size(); ++index) {
        auto& pass = Passes[index];
        pass.Declaration.Borrowed = &plan.PassDeclarations[index];
        const auto& compiled = plan.Passes[index];
        pass.CompiledData = &compiled;
        pass.RasterGroup = compiled.RasterGroup;
        pass.MergeTail = compiled.MergeTail;
        pass.Width = compiled.Width;
        pass.Height = compiled.Height;
        pass.Layers = compiled.Layers;
        pass.Samples = compiled.Samples;
        pass.AllowUavWrites = compiled.AllowUavWrites;
        pass.UavWriteStages = compiled.UavWriteStages;
        ExecutionPlan[index].CachedAccesses = &compiled.PhysicalAccesses;
    }
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        resource.Declaration.Borrowed = &plan.ResourceDeclarations[index];
        const auto& compiled = plan.Resources[index];
        resource.Physical = compiled.Physical;
        resource.PlannedCellCount = compiled.CellCount;
        resource.FirstUse = compiled.FirstUse;
        resource.LastUse = compiled.LastUse;
    }
    for (uint32_t index = 0; index < Views.size(); ++index) {
        Views[index].Resource = plan.Views[index].Resource;
        Views[index].Version = plan.Views[index].Version;
    }
    for (uint32_t index = 0; index < IndirectArgumentsRecords.size(); ++index)
        IndirectArgumentsRecords[index].Resource = plan.IndirectArgumentsRecords[index].Resource;
}

void RenderGraph::Impl::SaveFramePlan(vector<uint64_t> key) {
    auto plan = make_shared<CompiledFramePlan>();
    static std::atomic<uint64_t> nextPlan{1};
    plan->Id = nextPlan.fetch_add(1, std::memory_order_relaxed);
    if (plan->Id == 0) RADRAY_ABORT("Render graph execution plan identity space exhausted");
    plan->Graph = std::move(CompiledGraph);
    plan->LiveWork = std::move(LiveWork);
    plan->ReusedResources = Report.ReusedResources;
    plan->MergedRasterPasses = Report.MergedRasterPasses;
    plan->DiscardedStores = Report.DiscardedStores;
    plan->Passes.resize(Passes.size());
    for (uint32_t index = 0; index < Passes.size(); ++index) {
        auto& pass = Passes[index];
        auto& compiled = plan->Passes[index];
        compiled.Accesses = std::move(pass.Edit().Accesses);
        compiled.Cells = std::move(pass.Cells);
        compiled.PhysicalAccesses = std::move(ExecutionPlan[index].Accesses);
        compiled.BarrierTemplates = std::move(ExecutionPlan[index].BarrierTemplates);
        compiled.RouteResources = std::move(ExecutionPlan[index].RouteResources);
        compiled.RasterGroup = pass.RasterGroup;
        compiled.MergeTail = pass.MergeTail;
        compiled.Width = pass.Width;
        compiled.Height = pass.Height;
        compiled.Layers = pass.Layers;
        compiled.Samples = pass.Samples;
        compiled.AllowUavWrites = pass.AllowUavWrites;
        compiled.UavWriteStages = pass.UavWriteStages;
        for (const auto& color : pass.Def().Colors) compiled.ColorStores.push_back(color ? color->Desc.Store : render::StoreAction::Store);
        if (pass.Def().DepthAttachment) compiled.DepthStore = pass.Def().DepthAttachment->Desc.Store;
    }
    plan->Resources.reserve(Resources.size());
    for (const auto& resource : Resources) plan->Resources.push_back({resource.Physical, resource.CellCount(), resource.FirstUse, resource.LastUse});
    plan->ResourceDeclarations.reserve(Resources.size());
    for (auto& resource : Resources) {
        plan->ResourceDeclarations.push_back(std::move(resource.Edit()));
        resource.Declaration.Owned.reset();
    }
    plan->PassDeclarations.reserve(Passes.size());
    for (auto& pass : Passes) {
        plan->PassDeclarations.push_back(std::move(pass.Edit()));
        pass.Declaration.Owned.reset();
    }
    plan->Views = Views;
    plan->IndirectArgumentsRecords = IndirectArgumentsRecords;
    FramePlan = std::move(plan);
    ApplyFramePlan();
    if (!FrameResources || !Options.ReuseCompiledPlan) return;
    auto& cache = *FrameResources->_impl->Plans->_impl;
    HashCode hash;
    for (const auto value : key) hash.Add(value);
    RenderGraphPlanCache::Impl::Entry entry{std::move(key), hash.ToHashCode(), ++cache.UseSerial, FramePlan};
    if (cache.Entries.size() < cache.Capacity)
        cache.Entries.push_back(std::move(entry));
    else
        *std::min_element(cache.Entries.begin(), cache.Entries.end(), [](const auto& a, const auto& b) { return a.Used < b.Used; }) = std::move(entry);
}

bool RenderGraph::Compile() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Compile");
    auto& impl = *_impl;
    if (impl.Frozen) return impl.Compiled && !impl.Failed;
    if (!impl.TemplateSlotTypes.empty()) {
        impl.Error("TemplateBuilder", "Freeze a typed template builder and bind a frame instance before compiling");
        return false;
    }
    impl.Frozen = true;
    for (uint32_t index = 0; index < impl.Resources.size(); ++index)
        if (impl.Resources[index].Def().ExternalSlot && !impl.Resources[index].ExternalBound)
            impl.Error("TemplateExternal", "Every external resource slot requires a frame binding", InvalidIndex, index);
    if (impl.Failed) return false;
    if (impl.Templates.empty() && !impl.ResolvePorts()) return false;
    impl.Report.DeclaredPasses = static_cast<uint32_t>(impl.Passes.size());
    impl.Report.DeclaredWorks = static_cast<uint32_t>(impl.Works.size());
    auto planKey = impl.BuildPlanKey();
    if (impl.FindFramePlan(planKey)) {
        if (!impl.ValidateResources() || (impl.ValidationFull() && !impl.ValidatePlanInput())) return false;
        impl.ApplyCompiledReport();
        impl.Compiled = !impl.Failed;
        return impl.Compiled;
    }
    if (!impl.Templates.empty()) {
        impl.MaterializeTemplates();
        if (!impl.ResolvePorts()) return false;
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Validate");
        for (auto& resource : impl.Resources)
            if (!resource.Def().IsTexture) resource.Edit().BufferBoundaries = {0, resource.Def().BufferDesc.Size};
        for (auto& pass : impl.Passes)
            for (auto& access : pass.Edit().Accesses) {
                auto& resource = impl.Resources[access.Resource];
                if (resource.Def().IsTexture) continue;
                const uint64_t size = resource.Def().BufferDesc.Size;
                if (access.Bytes.Offset > size) {
                    impl.Error("BufferRange", "Buffer range starts outside resource", InvalidIndex, access.Resource);
                    continue;
                }
                if (access.Bytes.Size == render::BufferRange::All()) access.Bytes.Size = size - access.Bytes.Offset;
                if (access.Bytes.Size == 0 || access.Bytes.Size > size - access.Bytes.Offset) {
                    impl.Error("BufferRange", "Buffer range is empty or outside resource", InvalidIndex, access.Resource);
                    continue;
                }
                resource.Edit().BufferBoundaries.push_back(access.Bytes.Offset);
                resource.Edit().BufferBoundaries.push_back(access.Bytes.Offset + access.Bytes.Size);
            }
        for (auto& resource : impl.Resources) {
            auto& boundaries = resource.Edit().BufferBoundaries;
            std::sort(boundaries.begin(), boundaries.end());
            boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        }
        ++impl.Report.NormalizeBuilds;
        if (impl.Failed || !impl.ValidateResources() || !impl.NormalizePasses()) return false;
        if (impl.ValidationFull() && !impl.ValidatePlanInput()) return false;
        if (impl.ReportFull()) {
            for (uint32_t p = 0; p < impl.Passes.size(); ++p)
                for (const auto& access : impl.Passes[p].Def().Accesses)
                    impl.Report.Passes[p].Accesses.push_back({access.Resource, access.Version, access.State, access.Range, access.Bytes, access.Stages, access.Read, access.Write});
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Cull");
        impl.Cull();
    }
    if (!impl.Failed) {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Optimize");
        impl.PlanStorage();
        impl.OptimizeRaster();
        impl.BuildExecutionPlan();
        impl.BuildBarrierTemplates();
        if (!impl.Failed) impl.SaveFramePlan(std::move(planKey));
    }
    impl.Compiled = !impl.Failed;
    return impl.Compiled;
}

void RenderGraph::Impl::PlanStorage() {
    ++Report.StoragePlanBuilds;
    vector<uint32_t> order;
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        Resources[r].Physical = r;
        if (Resources[r].FirstUse >= 0) order.push_back(r);
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return Resources[a].FirstUse < Resources[b].FirstUse; });
    struct Slot {
        uint32_t Resource;
        int32_t LastUse;
    };
    vector<Slot> slots;
    for (const uint32_t r : order) {
        auto& resource = Resources[r];
        const auto& life = Resources[r];
        const bool reusable = Options.ReuseResources && !resource.External() &&
                              (resource.Def().IsTexture ? resource.Def().TextureDesc.Memory : resource.Def().BufferDesc.Memory) == render::MemoryType::Device;
        if (reusable) {
            for (auto& slot : slots) {
                const auto& candidate = Resources[slot.Resource];
                if (slot.LastUse >= life.FirstUse || candidate.Def().IsTexture != resource.Def().IsTexture) continue;
                const bool matches = resource.Def().IsTexture ? TexturePoolKey{resource.Def().TextureDesc} == TexturePoolKey{candidate.Def().TextureDesc} : BufferPoolKey{resource.Def().BufferDesc} == BufferPoolKey{candidate.Def().BufferDesc};
                if (!matches) continue;
                resource.Physical = slot.Resource;
                slot.LastUse = life.LastUse;
                ++Report.ReusedResources;
                break;
            }
            if (resource.Physical == r) slots.push_back({r, life.LastUse});
        }
        if (ReportFull()) Report.Resources[r].PhysicalSlot = resource.Physical;
    }
}

void RenderGraph::Impl::OptimizeRaster() {
    ++Report.RasterPlanBuilds;
    vector<uint8_t> consumed(GetCompiled().Versions.size(), 0);
    for (const auto p : GetCompiled().ExecutionOrder)
        for (const auto value : GetCompiled().Passes[p].Reads) consumed[value] = 1;
    uint32_t previous = InvalidIndex;
    const bool reportFull = ReportFull();
    for (const auto p : GetCompiled().ExecutionOrder) {
        auto& pass = Passes[p];
        if (pass.Def().Type != RgPassType::Raster) {
            previous = InvalidIndex;
            continue;
        }
        pass.RasterGroup = p;
        pass.MergeTail = p;
        RenderGraphPassReport* report = reportFull ? &Report.Passes[p] : nullptr;
        if (report) report->RasterGroup = p;
        const auto discard = [&](uint32_t view, render::StoreAction& store) {
            const auto resource = Views[view].Resource;
            if (store != render::StoreAction::Store || Resources[resource].External()) return;
            for (const auto value : GetCompiled().Passes[p].Writes)
                if (GetCompiled().Versions[value].Resource == resource && consumed[value]) return;
            store = render::StoreAction::Discard;
            for (auto& cell : pass.Cells)
                if (cell.Resource == resource && cell.Write) cell.ValidAfter = false;
            if (report) report->Decisions.push_back(fmt::format("Discard store for {}: no live consumer", Resources[resource].Def().Name));
            ++Report.DiscardedStores;
        };
        if (Options.OptimizeAttachmentStores) {
            for (auto& color : pass.Edit().Colors) discard(color->View, color->Desc.Store);
            if (pass.Def().DepthAttachment && !pass.Def().DepthAttachment->Desc.ReadOnly) discard(pass.Def().DepthAttachment->View, pass.Edit().DepthAttachment->Desc.Store);
        }
        const auto attachmentOnly = [&](const Pass& entry) {
            if (entry.AllowUavWrites) return false;
            for (const auto& access : entry.Def().Accesses) {
                if (!Resources[access.Resource].Def().IsTexture || (access.State != uint32_t(render::TextureState::RenderTarget) && access.State != uint32_t(render::TextureState::DepthRead) && access.State != uint32_t(render::TextureState::DepthWrite))) return false;
            }
            return true;
        };
        const auto sameView = [&](uint32_t a, uint32_t b) { return Views[a].Resource == Views[b].Resource && Views[a].Key == Views[b].Key; };
        bool merge = Options.MergeRasterPasses && previous != InvalidIndex && attachmentOnly(pass) && attachmentOnly(Passes[previous]);
        if (merge) {
            const auto& prior = Passes[previous];
            merge = prior.Def().Colors.size() == pass.Def().Colors.size() && prior.Def().DepthAttachment.has_value() == pass.Def().DepthAttachment.has_value();
            if (merge)
                for (size_t i = 0; i < pass.Def().Colors.size(); ++i)
                    merge &= sameView(prior.Def().Colors[i]->View, pass.Def().Colors[i]->View) && pass.Def().Colors[i]->Desc.Load == render::LoadAction::Load && prior.Def().Colors[i]->Desc.Store == render::StoreAction::Store;
            if (merge && pass.Def().DepthAttachment) merge &= sameView(prior.Def().DepthAttachment->View, pass.Def().DepthAttachment->View) && prior.Def().DepthAttachment->Desc.ReadOnly == pass.Def().DepthAttachment->Desc.ReadOnly && pass.Def().DepthAttachment->Desc.Load == render::LoadAction::Load && prior.Def().DepthAttachment->Desc.Store == render::StoreAction::Store;
        }
        if (merge) {
            pass.RasterGroup = Passes[previous].RasterGroup;
            Passes[pass.RasterGroup].MergeTail = p;
            if (report) {
                report->RasterGroup = pass.RasterGroup;
                report->Decisions.push_back("Merged: adjacent raster passes preserve identical attachments and need no non-attachment barriers");
            }
            ++Report.MergedRasterPasses;
        } else if (report)
            report->Decisions.push_back(Options.MergeRasterPasses ? "Raster boundary: attachment identity, load/store, or non-attachment access requires separation" : "Raster merging disabled");
        previous = p;
    }
}

bool RenderGraph::Impl::Realize() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Realize");
    const uint64_t createdBefore = Pool.GetStats().Created;
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        if (resource.ExternalBuffer && ValidationFull()) NativeBuffers.emplace(resource.ExternalBuffer->Buffer, r);
        if (Resources[r].FirstUse < 0) continue;
        if (resource.Physical != r) continue;
        if (resource.Def().IsTexture) {
            if (!resource.ExternalTexture) {
                resource.PoolTexture = Pool.AcquireTexture(resource.Def().TextureDesc, resource.Name(), resource.ViewId());
                if (!resource.PoolTexture) {
                    Error("TextureAllocation", "Texture allocation failed before recording", InvalidIndex, r);
                    return false;
                }
                if (ReportFull()) Report.Resources[r].PhysicalId = resource.PoolTexture->Id;
            }
            const auto states = resource.ExternalTexture ? std::span<const render::TextureStates>{resource.ExternalTexture->SubresourceStates} : std::span<const render::TextureStates>{resource.PoolTexture->States};
            for (const auto state : states) resource.States.push_back(state.value());
        } else {
            if (resource.Readback) {
                auto buffer = Device.CreateBuffer(resource.Def().BufferDesc);
                if (!buffer) {
                    Error("ReadbackAllocation", "Readback allocation failed", InvalidIndex, r);
                    return false;
                }
                resource.Readback->Buffer = shared_ptr<render::Buffer>(buffer.Release());
            } else if (!resource.ExternalBuffer) {
                resource.PoolBuffer = Pool.AcquireBuffer(resource.Def().BufferDesc, resource.Name(), resource.ViewId());
                if (!resource.PoolBuffer) {
                    Error("BufferAllocation", "Buffer allocation failed before recording", InvalidIndex, r);
                    return false;
                }
                if (ReportFull()) Report.Resources[r].PhysicalId = resource.PoolBuffer->Id;
            }
            resource.States.assign(1, (resource.ExternalBuffer ? resource.ExternalBuffer->State : resource.Readback ? InitialBufferState(resource.Def().BufferDesc)
                                                                                                                    : resource.PoolBuffer->State)
                                          .value());
            if (ValidationFull()) NativeBuffers.emplace(resource.NativeBuffer(), r);
        }
    }
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        if (resource.Physical == r || Resources[r].FirstUse < 0) continue;
        auto& physical = Resources[resource.Physical];
        resource.PoolTexture = physical.PoolTexture;
        resource.PoolBuffer = physical.PoolBuffer;
        resource.States = physical.States;
        if (ReportFull()) Report.Resources[r].PhysicalId = Report.Resources[resource.Physical].PhysicalId;
    }
    for (const uint32_t p : GetCompiled().ExecutionOrder) {
        auto& pass = Passes[p];
        for (const auto v : pass.Def().DeclaredViews) {
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
                    const auto range = render::NormalizeSubresourceRange(resource.Def().TextureDesc, desc.Range);
                    if (range && desc.Target == resource.NativeTexture() && TextureViewKey{desc.Dim, desc.Format, *range, desc.Usage} == view.Key) view.Native = borrowed;
                }
                if (!view.Native && !persistent) view.Native = Pool.CreateExternalTextureView({resource.NativeTexture(), view.Key.Dimension, view.Key.Format, view.Key.Range, view.Key.Usage});
            }
            if (!view.Native) {
                Error("ViewAllocation", "Texture view allocation failed before recording", p, view.Resource);
                return false;
            }
        }
        if (pass.Def().Type != RgPassType::Raster) continue;
        const auto group = pass.RasterGroup;
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
        for (const auto& attachment : pass.Def().Colors) {
            const auto& view = Views[attachment->View];
            colors.push_back({view.Key.Format, pass.Samples, attachment->Desc.Load, Passes[tail].ColorStore(colorIndex++)});
            formats.push_back(view.Key.Format);
            views.push_back(view.Native.Get());
            pass.Clears.push_back(pass.ColorClear(colorIndex - 1));
        }
        std::optional<render::RenderPassDepthStencilAttachmentDescriptor> depth;
        std::optional<render::TextureFormat> depthFormat;
        Nullable<render::TextureView*> depthView{nullptr};
        if (pass.Def().DepthAttachment) {
            const auto& attachment = *pass.Def().DepthAttachment;
            const auto& view = Views[attachment.View];
            depthFormat = view.Key.Format;
            depthView = view.Native;
            depth = render::RenderPassDepthStencilAttachmentDescriptor{view.Key.Format, pass.Samples, attachment.Desc.Load, Passes[tail].DepthStore(), attachment.Desc.Load, Passes[tail].DepthStore(), attachment.Desc.ReadOnly};
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
        pass.PassState->DepthReadOnly = pass.Def().DepthAttachment && pass.Def().DepthAttachment->Desc.ReadOnly;
    }
    auto& accessTables = FrameResources ? FrameResources->_impl->NativeAccessTables : NativeAccessTables;
    if (accessTables.size() < Passes.size()) accessTables.resize(Passes.size());
    for (const uint32_t p : GetCompiled().ExecutionOrder) {
        auto& pass = Passes[p];
        auto& table = accessTables[p];
        table.Reset(pass.Def().DeclaredViews.size() + pass.Def().DeclaredBuffers.size());
        for (const uint32_t view : pass.Def().DeclaredViews) {
            if (!Views[view].Native) {
                Error("TextureViewAllocation", "Declared texture view is unavailable before preparation", p);
                return false;
            }
            table.Insert(view, true, Views[view].Native.Get());
        }
        for (const uint32_t buffer : pass.Def().DeclaredBuffers) {
            const auto& resource = Resources[buffer];
            if (resource.Def().IsTexture || (!resource.ExternalBuffer && !resource.Readback && !resource.PoolBuffer)) {
                Error("BufferAllocation", "Declared buffer is unavailable before preparation", p, buffer);
                return false;
            }
            table.Insert(buffer, false, resource.NativeBuffer());
        }
        pass.NativeAccess = &table;
    }
    Report.PhysicalAllocations = static_cast<uint32_t>(Pool.GetStats().Created - createdBefore);
    return true;
}

bool RenderGraph::IsPreparingWork() const noexcept { return _impl->PreparingWork; }
bool RenderGraph::Prepare() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::Prepare");
    auto& impl = *_impl;
    auto clearDrafts = MakeScopeGuard([&impl]() noexcept {
        impl.PendingParameterSets.clear();
        impl.ParameterRequests.clear();
    });
    if (!impl.InstantiateTemplatePayloads()) return false;
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PrepareWork");
        struct WorkScope {
            bool& Active;
            explicit WorkScope(bool& active) : Active(active) { Active = true; }
            ~WorkScope() { Active = false; }
        } scope{impl.PreparingWork};
        for (const auto& [index, mask] : impl.FramePlan->LiveWork) {
            ++impl.Report.WorkRuns;
            if (!impl.Works[index].Data->Prepare(mask)) {
                impl.Error("WorkPreparation", fmt::format("Live work '{}' failed before recording", impl.Works[index].Declaration.Get().Name));
                return false;
            }
            if (impl.Failed) return false;
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PrepareUploads");
        for (uint32_t p : impl.GetCompiled().ExecutionOrder) {
            auto& pass = impl.Passes[p];
            if (pass.UploadBytes.empty() && !pass.UploadSource) continue;
            const auto upload = [&] {
                auto& resource = impl.Resources[pass.GetAccesses().front().Resource];
                const auto bytes = pass.UploadSource ? pass.UploadSource->Bytes : std::span<const byte>{pass.UploadBytes};
                if (bytes.size() != resource.Def().BufferDesc.Size) {
                    impl.Error("UploadData", "Live upload bytes must fill their declared fixed-size buffer", p);
                    return false;
                }
                ScopedBufferMap map{resource.NativeBuffer(), {0, bytes.size()}};
                if (!map) {
                    impl.Error("UploadMap", "Graph upload buffer mapping failed before recording", p);
                    return false;
                }
                std::memcpy(map.Data(), bytes.data(), bytes.size());
                return true;
            };
            if (pass.UploadSource) {
                RADRAY_PROFILE_SCOPE_N("RenderGraph::UploadWorkData");
                if (!upload()) return false;
                ++impl.Report.WorkUploads;
                impl.Report.WorkUploadBytes += pass.UploadSource->Bytes.size();
            } else if (!upload())
                return false;
        }
    }
    {
        // Live passes only: a culled pass creates no descriptor and resolves no pipeline state.
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PreparePasses");
        for (const uint32_t p : impl.GetCompiled().ExecutionOrder) {
            Impl::Pass& pass = impl.Passes[p];
            if (!pass.Live || !pass.Data) continue;
            RADRAY_PROFILE_SCOPE_DYN(pass.Name());
            RenderGraphPrepareContext context{*this, p};
            const bool prepared = pass.Data->Prepare(context);
            if (impl.Failed) return false;
            if (!prepared) {
                impl.Error("PassPreparation", "Pass preparation failed before recording", p);
                return false;
            }
        }
    }
    if (impl.ValidationFull() && !ValidateReadyFrame()) return false;
    return impl.PublishParameterSets();
}

bool RenderGraph::ValidateReadyFrame() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::ValidateReadyFrame");
    auto& impl = *_impl;
    ++impl.Report.ValidateReadyFrameCalls;
    impl.ValidatingReady = true;
    auto readyScope = MakeScopeGuard([&impl]() noexcept { impl.ValidatingReady = false; });
    if (!impl.ValidateParameterRequests()) return false;
    for (const uint32_t p : impl.GetCompiled().ExecutionOrder) {
        RenderGraphPrepareContext context{*this, p};
        for (const auto& check : impl.Passes[p].GeometryChecks)
            if (!ValidateGeometryBuffer(p, check.Buffer, check.Access)) return false;
        for (const auto& check : impl.Passes[p].ReadyChecks) {
            ++impl.Report.ReadyValidationCallbacks;
            if (!check.Validate(check.Payload.get(), context)) {
                if (!impl.Failed) impl.Error("ReadyValidation", "Prepared recording data failed validation before recording", p);
                return false;
            }
            if (impl.Failed) return false;
        }
    }
    return true;
}

void RenderGraph::Impl::BuildExecutionPlan() {
    ++Report.ExecutionPlanBuilds;
    ++Report.RoutePlanBuilds;
    ExecutionPlan.resize(Passes.size());
    unordered_map<uint64_t, uint32_t> indices;
    vector<uint32_t> routed(Resources.size(), InvalidIndex);
    vector<uint64_t> workMasks(Works.size(), 0);
    for (const uint32_t p : GetCompiled().ExecutionOrder) {
        for (const auto& [work, mask] : Passes[p].Def().WorkRequirements) workMasks[work] |= mask;
        auto& plan = ExecutionPlan[p];
        indices.clear();
        plan.Accesses.reserve(Passes[p].Cells.size());
        for (const auto& access : Passes[p].GetCells()) {
            const auto& resource = Resources[access.Resource];
            const auto physical = resource.Physical, cell = resource.PhysicalCell(access.Cell);
            if (resource.Def().IsTexture && routed[physical] != p) {
                routed[physical] = p;
                plan.RouteResources.push_back(physical);
            }
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
    for (uint32_t work = 0; work < workMasks.size(); ++work)
        if (workMasks[work]) LiveWork.emplace_back(work, workMasks[work]);
}

void RenderGraph::Impl::BuildBarrierTemplates() {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::BuildBarrierTemplates");
    ++Report.BarrierTemplateBuilds;
    struct PreviousAccess {
        uint32_t State{0};
        render::ShaderStages Stages{render::ShaderStage::UNKNOWN};
        bool Seen{false}, Write{true};
    };
    vector<vector<PreviousAccess>> previous(Resources.size());
    for (uint32_t r = 0; r < Resources.size(); ++r)
        if (Resources[r].Physical == r && Resources[r].FirstUse >= 0)
            previous[r].resize(Resources[r].Def().IsTexture ? Resources[r].SubresourceCount() : 1);
    vector<uint32_t> uavSeen(Resources.size(), InvalidIndex), uavResources;
    for (const uint32_t p : GetCompiled().ExecutionOrder) {
        auto& plan = ExecutionPlan[p];
        uavResources.clear();
        const bool continuation = Passes[p].Def().Type == RgPassType::Raster && Passes[p].RasterGroup != p;
        for (const auto& access : plan.GetAccesses()) {
            const auto& resource = Resources[access.Resource];
            const auto physical = access.Physical, cell = access.Cell;
            auto& before = previous[physical][cell];
            const uint32_t uav = resource.Def().IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
            const bool sameStateWrite = before.State != uav && (before.Write || access.Write);
            if (!continuation) {
                if (!before.Seen)
                    plan.BarrierTemplates.push_back({BarrierKind::InitialState, access.Resource, physical, cell, 0, access.State, render::ShaderStage::UNKNOWN, access.Stages});
                else if (before.State != access.State || sameStateWrite || !Options.EliminateBarriers)
                    plan.BarrierTemplates.push_back({BarrierKind::Transition, access.Resource, physical, cell, before.State, access.State, before.Stages, access.Stages});
                else if (before.State == uav && (before.Write || access.Write) && uavSeen[access.Resource] != p) {
                    uavSeen[access.Resource] = p;
                    uavResources.push_back(access.Resource);
                }
            }
            if (before.Seen && before.State == access.State && !access.Write && !before.Write)
                before.Stages |= access.Stages;
            else
                before.Stages = access.Stages;
            before.State = access.State;
            before.Write = access.Write;
            before.Seen = true;
        }
        for (const auto r : uavResources)
            plan.BarrierTemplates.push_back({BarrierKind::Uav, r, Resources[r].Physical, 0, 0, 0, render::ShaderStage::UNKNOWN, render::ShaderStage::UNKNOWN});
    }
}

void RenderGraph::Impl::PatchBarriers() {
    vector<uint32_t> uavSeen(Resources.size(), InvalidIndex), uavResources;
    for (const uint32_t p : GetCompiled().ExecutionOrder) {
        auto& plan = ExecutionPlan[p];
        const auto& templates = FramePlan->Passes[p].BarrierTemplates;
        plan.Barriers.reserve(templates.size());
        uavResources.clear();
        for (const auto& barrier : templates) {
            auto& resource = Resources[barrier.Resource];
            const auto uav = resource.Def().IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
            uint32_t before = barrier.Before;
            bool memory = barrier.Kind == BarrierKind::Uav;
            if (barrier.Kind == BarrierKind::InitialState) {
                ++Report.InitialStatePatches;
                before = Resources[barrier.Physical].States[barrier.Cell];
                // The incoming queue dependency is conservatively a write. Only its native state
                // varies per instance; all following transitions were resolved by the cold plan.
                memory = before == uav && before == barrier.After && Options.EliminateBarriers;
            }
            if (memory) {
                if (uavSeen[barrier.Resource] != p) {
                    uavSeen[barrier.Resource] = p;
                    uavResources.push_back(barrier.Resource);
                }
                continue;
            }
            if (resource.Def().IsTexture)
                plan.Barriers.push_back(render::BarrierTextureDescriptor{.Target = resource.NativeTexture(), .Before = static_cast<render::TextureState>(before), .After = static_cast<render::TextureState>(barrier.After), .IsSubresourceBarrier = true, .Range = {barrier.Cell / resource.Def().TextureDesc.MipLevels, 1, barrier.Cell % resource.Def().TextureDesc.MipLevels, 1}, .BeforeStages = barrier.BeforeStages, .AfterStages = barrier.AfterStages});
            else
                plan.Barriers.push_back(render::BarrierBufferDescriptor{.Target = resource.NativeBuffer(), .Before = static_cast<render::BufferState>(before), .After = static_cast<render::BufferState>(barrier.After), .BeforeStages = barrier.BeforeStages, .AfterStages = barrier.AfterStages});
            if (ReportFull())
                Report.Barriers.push_back({p, barrier.Resource, barrier.Cell, before, barrier.After, false,
                                           !resource.Def().IsTexture ? "Whole-buffer state; content dependencies retain byte ranges" : resource.AspectCount() == 2 ? "Coupled depth/stencil native layout; content dependencies retain aspects"
                                                                                                                                                                   : "Exact mip/layer"});
            ++Report.TransitionBarriers;
        }
        for (const auto r : uavResources) {
            auto& resource = Resources[r];
            render::Resource* native = resource.Def().IsTexture ? static_cast<render::Resource*>(resource.NativeTexture()) : static_cast<render::Resource*>(resource.NativeBuffer());
            plan.Barriers.push_back(render::BarrierUavDescriptor{native});
            if (ReportFull()) {
                const auto state = resource.Def().IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
                Report.Barriers.push_back({p, r, 0, state, state, true, "UAV memory dependency"});
            }
            ++Report.UavBarriers;
        }
    }
}

bool RenderGraph::Impl::PatchCommandRoutes(render::CommandBuffer& command, std::span<const PresentCommandTarget> presentTargets) {
    RADRAY_PROFILE_SCOPE_N("RenderGraph::PatchCommandRoutes");
    if (presentTargets.empty()) {
        for (const uint32_t p : GetCompiled().ExecutionOrder) ExecutionPlan[p].Commands = &command;
        Report.CommandRoutePatches += GetCompiled().ExecutionOrder.size();
        return true;
    }
    unordered_map<render::Texture*, Nullable<render::CommandBuffer*>> targets;
    for (const auto& target : presentTargets) {
        if (target.Texture == nullptr || target.Commands == nullptr) continue;
        auto [it, inserted] = targets.emplace(target.Texture, target.Commands);
        if (!inserted && it->second.Get() != target.Commands) it->second = nullptr;
    }
    vector<Nullable<render::CommandBuffer*>> routes(Resources.size(), nullptr);
    vector<uint8_t> ambiguous(Resources.size(), 0);
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        const auto& resource = Resources[r];
        if (resource.Physical != r || !resource.Def().IsTexture || resource.FirstUse < 0) continue;
        const auto found = targets.find(resource.NativeTexture());
        if (found != targets.end()) {
            routes[r] = found->second;
            ambiguous[r] = !found->second;
        }
    }
    for (const uint32_t p : GetCompiled().ExecutionOrder) {
        Nullable<render::CommandBuffer*> selected{nullptr};
        for (const uint32_t r : FramePlan->Passes[p].RouteResources) {
            if (ambiguous[r] || (routes[r] && selected && selected != routes[r])) {
                Error("PresentCommandSplit", "A pass accesses more than one presentation command buffer", p);
                return false;
            }
            if (routes[r]) selected = routes[r];
        }
        auto& plan = ExecutionPlan[p];
        plan.Commands = selected ? selected.Get() : &command;
        const auto& pass = Passes[p];
        if (pass.Def().Type == RgPassType::Raster && pass.RasterGroup != p && ExecutionPlan[pass.RasterGroup].Commands != plan.Commands) {
            Error("PresentCommandSplit", "Raster group spans multiple presentation command buffers", p);
            return false;
        }
        ++Report.CommandRoutePatches;
    }
    return true;
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
    if (!Compile() || !impl.Realize() || !Prepare() || !impl.PatchCommandRoutes(command, presentTargets)) {
        for (auto& pass : impl.Passes)
            if (pass.Ticket._state) pass.Ticket._state->Cancel();
        impl.Pool.EndGraph();
        impl.Report.Pool = impl.Pool.GetStats();
        PlotGraphCpuStats(impl.Report);
        return {};
    }
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::PatchBarriers");
        impl.PatchBarriers();
    }
    RenderGraphExecutionResult result{true, false, {}};
    {
        RADRAY_PROFILE_SCOPE_N("RenderGraph::Record");
        Nullable<unique_ptr<render::GraphicsCommandEncoder>> rasterEncoder{nullptr};
        for (size_t order = 0; order < impl.GetCompiled().ExecutionOrder.size(); ++order) {
            const uint32_t p = impl.GetCompiled().ExecutionOrder[order];
            auto& pass = impl.Passes[p];
            if (!pass.Live) continue;
            const auto& plan = impl.ExecutionPlan[p];
            render::CommandBuffer* recording = plan.Commands.Get();
            RADRAY_PROFILE_SCOPE_DYN(pass.Name());
            if (impl.Runtime.GpuMarkers) recording->PushDebugGroup(pass.Name());
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
            for (const auto& access : plan.GetAccesses()) impl.Resources[access.Physical].States[access.Cell] = access.State;
            if (pass.Def().Type == RgPassType::Raster) {
                const auto depthClear = pass.DepthClear();
                if (pass.RasterGroup == p) rasterEncoder = recording->BeginRenderPass({pass.NativePass.Get(), pass.Framebuffer.Get(), pass.Clears,
                                                                                       depthClear, pass.Name(), pass.AllowUavWrites});
                if (!rasterEncoder) {
                    impl.Error("BeginRenderPass", "Encoder creation failed after barriers; actual states are committed for host recovery", p);
                    result.Success = false;
                } else {
                    RenderGraphRasterContext context(*this, p, *rasterEncoder);
                    if (pass.Data) pass.Data->Run(context);
                    const uint32_t nextPass = order + 1 == impl.GetCompiled().ExecutionOrder.size() ? InvalidIndex : impl.GetCompiled().ExecutionOrder[order + 1];
                    const bool last = nextPass == InvalidIndex || impl.Passes[nextPass].RasterGroup != pass.RasterGroup;
                    if (last || impl.Failed) recording->EndRenderPass(rasterEncoder.Release());
                }
            } else if (pass.Def().Type == RgPassType::Compute) {
                auto encoder = recording->BeginComputePass();
                if (!encoder) {
                    impl.Error("BeginComputePass", "Encoder creation failed after barriers; actual states are committed for host recovery", p);
                    result.Success = false;
                } else {
                    RenderGraphComputeContext context(*this, p, *encoder);
                    if (pass.Data) pass.Data->Run(context);
                    recording->EndComputePass(encoder.Release());
                }
            } else if (pass.Def().CopyOp) {
                const auto& copy = *pass.Def().CopyOp;
                if (copy.Type == Impl::CopyType::Resolve)
                    ++pass.CommandCalls.Resolve;
                else
                    ++pass.CommandCalls.Copy;
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
                    recording->CopyTextureToTexture({.Destination = dst.NativeTexture(), .DestinationMipLevel = copy.DestinationRange.BaseMipLevel, .DestinationArrayLayer = copy.DestinationRange.BaseArrayLayer, .Source = src.NativeTexture(), .SourceMipLevel = copy.SourceRange.BaseMipLevel, .SourceArrayLayer = copy.SourceRange.BaseArrayLayer, .Width = std::max(1u, src.Def().TextureDesc.Width >> copy.SourceRange.BaseMipLevel), .Height = std::max(1u, src.Def().TextureDesc.Height >> copy.SourceRange.BaseMipLevel), .ArrayLayerCount = copy.SourceRange.ArrayLayerCount});
            }
            if (impl.Runtime.GpuMarkers) recording->PopDebugGroup();
            impl.Report.CommandCalls.Add(pass.CommandCalls);
            if (impl.ReportFull()) impl.Report.Passes[p].CommandCalls = pass.CommandCalls;
            if (impl.Failed) result.Success = false;
            if (!result.Success) {
                for (const auto& access : pass.GetCells())
                    if (access.Write) impl.Resources[access.Resource].Valid[access.Cell] = 0;
                break;
            }
            pass.Executed = true;
            if (impl.ReportFull()) impl.Report.Passes[p].Executed = true;
            if (pass.Ticket._state) pass.Ticket._state->Record();
            for (const auto& access : pass.GetCells())
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
    if (impl.Resources[handle.Index].Def().Port && handle.Version < impl.Resources[handle.Index].Def().ResolvedValues.size()) handle.Index = impl.Resources[handle.Index].Def().ResolvedValues[handle.Version].first;
    if (handle.Index >= impl.Resources.size()) return {};
    const auto& resource = impl.Resources[handle.Index];
    if (!resource.Def().IsTexture || resource.Physical >= impl.Resources.size()) return {};
    const auto& states = impl.Resources[resource.Physical].States;
    if (subresource >= states.size()) return {};
    return static_cast<render::TextureState>(states[subresource]);
}
bool RenderGraph::WasWritten(RgTextureValue handle) const noexcept {
    return handle.Generation == _impl->Generation && handle.Index < _impl->Resources.size() && _impl->Resources[handle.Index].Def().IsTexture && _impl->Resources[handle.Index].Written;
}
bool RenderGraph::WasWritten(const RenderExternalTexture& texture) const noexcept {
    for (const auto& resource : _impl->Resources)
        if (resource.ExternalTexture && resource.ExternalTexture->Texture == texture.Texture) return resource.Written;
    return false;
}
std::optional<render::TextureDescriptor> RenderGraph::GetTextureDescriptor(RgTextureValue handle) const noexcept {
    if (handle.Generation != _impl->Generation || handle.Index >= _impl->Resources.size() || !_impl->Resources[handle.Index].Def().IsTexture) return {};
    return _impl->Resources[handle.Index].Def().TextureDesc;
}
void RenderGraph::AddDiagnostic(std::string_view code, std::string_view message) {
    _impl->Error(code, message);
}
void RenderGraphRasterContext::Fail(std::string_view message) {
    _graph._impl->Error("RasterExecution", message, _pass);
}
render::TextureView* RenderGraph::ResolveView(uint32_t pass, RgTextureViewHandle handle) const {
    const auto& impl = *_impl;
    handle = impl.PatchTemplateView(pass, handle);
    if (handle.Generation != impl.Generation || pass >= impl.Passes.size() || !impl.Passes[pass].NativeAccess)
        RADRAY_ABORT("RenderGraph texture view was not declared by this pass");
    const auto native = impl.Passes[pass].NativeAccess->Find(handle.Index, true);
    if (!native) RADRAY_ABORT("RenderGraph texture view was not declared by this pass");
    return static_cast<render::TextureView*>(native.Get());
}
render::Buffer* RenderGraph::ResolveBuffer(uint32_t pass, RgBufferValue handle) const {
    const auto& impl = *_impl;
    handle = impl.PatchTemplateBuffer(pass, handle);
    if (handle.Generation == impl.Generation && handle.Index < impl.Resources.size() && impl.Resources[handle.Index].Def().Port && handle.Version < impl.Resources[handle.Index].Def().ResolvedValues.size()) {
        const auto value = impl.Resources[handle.Index].Def().ResolvedValues[handle.Version];
        handle.Index = value.first;
        handle.Version = value.second;
    }
    if (handle.Generation != impl.Generation || pass >= impl.Passes.size() || !impl.Passes[pass].NativeAccess)
        RADRAY_ABORT("RenderGraph buffer was not declared by this pass");
    const auto native = impl.Passes[pass].NativeAccess->Find(handle.Index, false);
    if (!native) RADRAY_ABORT("RenderGraph buffer was not declared by this pass");
    return static_cast<render::Buffer*>(native.Get());
}
void RenderGraph::ExecuteIndirect(
    uint32_t pass, RgIndirectArgumentsHandle handle, RgIndirectCommand expected,
    render::GraphicsCommandEncoder* graphics, render::ComputeCommandEncoder* compute) noexcept {
    auto& impl = *_impl;
    handle = impl.PatchTemplateIndirect(pass, handle);
    if (handle.Generation != impl.Generation || handle.Index >= impl.IndirectArgumentsRecords.size()) {
        RADRAY_ABORT("RenderGraph indirect arguments handle belongs to another graph or is invalid");
    }
    const Impl::IndirectArguments& arguments = impl.IndirectArgumentsRecords[handle.Index];
    if (arguments.Pass != pass || arguments.Command != expected) {
        RADRAY_ABORT("RenderGraph indirect arguments handle was not declared by this pass or has the wrong command kind");
    }
    render::Buffer* buffer = impl.Resources[arguments.Resource].NativeBuffer();
    auto& calls = impl.Passes[pass].CommandCalls;
    switch (expected) {
        case RgIndirectCommand::Draw:
            if (graphics == nullptr || compute != nullptr) RADRAY_ABORT("DrawIndirect requires a raster pass encoder");
            ++calls.DrawIndirect;
            calls.IndirectDrawArguments += arguments.Count;
            graphics->DrawIndirect(buffer, arguments.Offset, arguments.Count);
            return;
        case RgIndirectCommand::DrawIndexed:
            if (graphics == nullptr || compute != nullptr) RADRAY_ABORT("DrawIndexedIndirect requires a raster pass encoder");
            ++calls.DrawIndexedIndirect;
            calls.IndirectDrawArguments += arguments.Count;
            graphics->DrawIndexedIndirect(buffer, arguments.Offset, arguments.Count);
            return;
        case RgIndirectCommand::Dispatch:
            if (compute == nullptr || graphics != nullptr) RADRAY_ABORT("DispatchIndirect requires a compute pass encoder");
            if (arguments.Count != 0) {
                ++calls.DispatchIndirect;
                compute->DispatchIndirect(buffer, arguments.Offset);
            }
            return;
    }
    RADRAY_ABORT("RenderGraph indirect command kind is invalid");
}

bool RenderGraph::ValidateGeometryBuffer(uint32_t pass, Nullable<render::Buffer*> buffer, RgBufferAccess access) {
    auto& impl = *_impl;
    if (!buffer) {
        impl.Error("GeometryBuffer", "Geometry binding requires a non-null buffer", pass);
        return false;
    }
    if (!impl.ValidationFull()) return true;
    if (!impl.ValidatingReady) {
        impl.Passes[pass].GeometryChecks.push_back({buffer.Get(), access});
        return true;
    }
    ++impl.Report.GeometryValidationCalls;
    // Only graph-managed buffers can carry a planned state; a persistent asset never does, so it
    // needs no declaration and costs one hash lookup per distinct buffer per pass.
    const auto tracked = impl.NativeBuffers.find(buffer.Get());
    if (tracked == impl.NativeBuffers.end()) return true;
    // Aliased resources share one native buffer while NativeBuffers only records the physical
    // primary, so match on the pointer and merge every read this pass declared on that buffer.
    const uint32_t required = static_cast<uint32_t>(BufferAccessInfo(access).first);
    auto& local = impl.Passes[pass];
    if (!local.GeometryReadStatesBuilt) {
        for (const auto& declared : local.GetAccesses()) {
            ++impl.Report.GeometryDeclarationScans;
            const Impl::Resource& resource = impl.Resources[declared.Resource];
            const bool realized = resource.ExternalBuffer || resource.Readback || resource.PoolBuffer;
            if (declared.Read && !resource.Def().IsTexture && realized) local.GeometryReadStates[resource.NativeBuffer()] |= declared.State;
        }
        local.GeometryReadStatesBuilt = true;
    }
    const auto read = local.GeometryReadStates.find(buffer.Get());
    const uint32_t states = read == local.GeometryReadStates.end() ? 0 : read->second;
    if ((states & required) == required) return true;
    impl.Error("UndeclaredGeometryRead", "Graph-managed geometry requires a matching Vertex or Index read declaration in this pass",
               pass, tracked->second);
    return false;
}

RenderGraphGraphicsCommands::RenderGraphGraphicsCommands(RenderGraph& graph, uint32_t pass, render::GraphicsCommandEncoder& encoder)
    : _graph(graph), _pass(pass), _encoder(encoder), _calls(graph._impl->Passes[pass].CommandCalls) {}

RenderGraphComputeCommands::RenderGraphComputeCommands(RenderGraph& graph, uint32_t pass, render::ComputeCommandEncoder& encoder)
    : _graph(graph), _pass(pass), _encoder(encoder), _calls(graph._impl->Passes[pass].CommandCalls) {}

void RenderGraphGraphicsCommands::DrawIndirect(RgIndirectArgumentsHandle arguments) noexcept {
    _graph.ExecuteIndirect(_pass, arguments, RgIndirectCommand::Draw, &_encoder, nullptr);
}
void RenderGraphGraphicsCommands::DrawIndexedIndirect(RgIndirectArgumentsHandle arguments) noexcept {
    _graph.ExecuteIndirect(_pass, arguments, RgIndirectCommand::DrawIndexed, &_encoder, nullptr);
}
void RenderGraphComputeCommands::DispatchIndirect(RgIndirectArgumentsHandle arguments) noexcept {
    _graph.ExecuteIndirect(_pass, arguments, RgIndirectCommand::Dispatch, nullptr, &_encoder);
}
render::TextureView* RenderGraphRasterContext::GetTextureView(RgTextureViewHandle handle) const { return _graph.ResolveView(_pass, handle); }
render::Buffer* RenderGraphRasterContext::GetBuffer(RgBufferValue handle) const { return _graph.ResolveBuffer(_pass, handle); }
const GraphicsPassState& RenderGraphRasterContext::PassState() const noexcept { return *_graph._impl->Passes[_pass].PassState; }
render::TextureView* RenderGraphComputeContext::GetTextureView(RgTextureViewHandle handle) const { return _graph.ResolveView(_pass, handle); }
render::Buffer* RenderGraphComputeContext::GetBuffer(RgBufferValue handle) const { return _graph.ResolveBuffer(_pass, handle); }

RgPassHandle RenderGraphPrepareContext::GetPassHandle() const noexcept { return {_pass, _graph.GetGeneration()}; }
bool RenderGraphPrepareContext::IsValidationFull() const noexcept { return _graph.IsValidationFull(); }
const RenderGraphRuntimeOptions& RenderGraphPrepareContext::GetRuntimeOptions() const noexcept { return _graph.GetRuntimeOptions(); }
void RenderGraphPrepareContext::DeferReadyValidation(shared_ptr<const void> payload, bool (*validate)(const void*, RenderGraphPrepareContext&)) {
    if (!_graph._impl->ValidationFull()) return;
    if (_graph._impl->ValidatingReady) {
        Reject("ReadyValidation", "Ready validation cannot register another callback");
        return;
    }
    if (!payload || !validate) {
        Reject("ReadyValidation", "Deferred validation requires owned data and a callback");
        return;
    }
    _graph._impl->Passes[_pass].ReadyChecks.push_back({std::move(payload), validate});
}
const GraphicsPassState& RenderGraphPrepareContext::PassState() const noexcept { return *_graph._impl->Passes[_pass].PassState; }
render::TextureView* RenderGraphPrepareContext::GetTextureView(RgTextureViewHandle handle) const { return _graph.ResolveView(_pass, handle); }
render::Buffer* RenderGraphPrepareContext::GetBuffer(RgBufferValue handle) const { return _graph.ResolveBuffer(_pass, handle); }
Nullable<render::GraphicsPipelineState*> RenderGraphPrepareContext::ResolveGraphicsPipeline(
    ShaderProgram& program, const MaterialPipelineState& state, const PrimitiveVertexLayout& layout, PrimitiveTopology topology) {
    return _graph.ResolveGraphicsPipeline(_pass, program, state, layout, topology);
}
Nullable<render::ComputePipelineState*> RenderGraphPrepareContext::ResolveComputePipeline(ShaderProgram& program) {
    return _graph.ResolveComputePipeline(_pass, program);
}
PreparedShaderGroup RenderGraphPrepareContext::CreateParameterSet(ShaderProgram& program, uint32_t group,
                                                                  std::span<const RgParameterBinding> bindings) {
    return _graph.CreateParameterSet(_pass, program, group, bindings);
}
bool RenderGraphPrepareContext::ValidateGeometryBuffer(Nullable<render::Buffer*> buffer, RgBufferAccess access) {
    return _graph.ValidateGeometryBuffer(_pass, buffer, access);
}
void RenderGraphPrepareContext::Reject(std::string_view code, std::string_view message, std::string_view binding) {
    _graph._impl->Error(code, message, _pass, InvalidIndex, binding);
}

}  // namespace radray
