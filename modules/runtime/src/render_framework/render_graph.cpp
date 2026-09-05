#include <radray/runtime/render_framework/render_graph.h>
#include <radray/runtime/render_framework/render_graph_runtime.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <type_traits>
#include <radray/logger.h>
#include <radray/utility.h>

namespace radray {
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
}

struct RenderGraph::Impl {
    struct Resource {
        string Name;
        std::source_location Location;
        bool IsTexture{false};
        render::TextureDescriptor TextureDesc;
        render::BufferDescriptor BufferDesc;
        Nullable<RenderExternalTexture*> ExternalTexture{nullptr};
        Nullable<RenderExternalBuffer*> ExternalBuffer{nullptr};
        RenderGraphExternalAccess ExternalAccess{RenderGraphExternalAccess::ReadWrite};
        Nullable<PooledTexture*> PoolTexture{nullptr};
        Nullable<PooledBuffer*> PoolBuffer{nullptr};
        vector<uint32_t> States;
        vector<uint8_t> Valid;
        bool Written{false};
        uint32_t CellCount() const { return IsTexture ? TextureDesc.MipLevels * (TextureDesc.Dim == render::TextureDimension::Dim3D ? 1 : TextureDesc.DepthOrArraySize) : 1; }
        render::Texture* NativeTexture() const { return ExternalTexture ? ExternalTexture->Texture : PoolTexture->Texture.get(); }
        render::Buffer* NativeBuffer() const { return ExternalBuffer ? ExternalBuffer->Buffer : PoolBuffer->Buffer.get(); }
        bool External() const { return ExternalTexture || ExternalBuffer; }
    };
    struct View {
        uint32_t Resource;
        TextureViewKey Key;
        Nullable<render::TextureView*> Native;
    };
    struct Access {
        uint32_t Resource;
        render::SubresourceRange Range;
        uint32_t State;
        bool Read, Write, ValidAfter;
    };
    struct CellAccess {
        uint32_t Resource, Cell, State;
        bool Read, Write, ValidAfter;
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
                          Resolve };
    struct Copy {
        CopyType Type;
        uint32_t Source, Destination;
        uint64_t Size{0}, SourceOffset{0}, DestinationOffset{0};
        render::SubresourceRange SourceRange{0, 1, 0, 1}, DestinationRange{0, 1, 0, 1};
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
        vector<std::optional<Color>> Colors;
        std::optional<Depth> DepthAttachment;
        std::optional<Copy> CopyOp;
        bool SideEffect{false};
        Nullable<render::RenderPass*> NativePass{nullptr};
        Nullable<render::Framebuffer*> Framebuffer{nullptr};
        std::optional<GraphicsPassState> PassState;
        vector<render::ColorClearValue> Clears;
        vector<render::ResourceBarrierDescriptor> Barriers;
        uint32_t Width{0}, Height{0}, Layers{0}, Samples{0};
        render::ShaderStages UavWriteStages{render::ShaderStage::UNKNOWN};
        bool AllowUavWrites{false};
    };
    render::Device& Device;
    RenderResourcePool& Pool;
    render::RenderPassRegistry& Registry;
    Nullable<RenderGraphFrameResources*> FrameResources{nullptr};
    uint64_t Generation;
    bool Frozen{false}, Compiled{false}, Executed{false};
    vector<Resource> Resources;
    vector<View> Views;
    vector<Pass> Passes;
    vector<IndirectArguments> IndirectArgumentsRecords;
    vector<ComputeProgram> ComputePrograms;
    vector<ParameterSet> ParameterSets;
    RenderGraphExecutionReport Report;

    Impl(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry,
         Nullable<RenderGraphFrameResources*> frameResources, std::string_view name)
        : Device(device), Pool(pool), Registry(registry), FrameResources(frameResources), Generation(NextGraphGeneration.fetch_add(1, std::memory_order_relaxed)) {
        if (Generation == 0 || Generation == UINT64_MAX) RADRAY_ABORT("RenderGraph generation exhausted");
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
    void CommitStates() {
        for (auto& resource : Resources) {
            if (resource.States.empty()) continue;
            if (resource.IsTexture) {
                auto states = resource.ExternalTexture ? resource.ExternalTexture->SubresourceStates : resource.PoolTexture ? std::span<render::TextureStates>{resource.PoolTexture->States}
                                                                                                                            : std::span<render::TextureStates>{};
                for (size_t i = 0; i < states.size(); ++i) states[i] = static_cast<render::TextureState>(resource.States[i]);
                if (resource.ExternalTexture) {
                    std::copy(resource.Valid.begin(), resource.Valid.end(), resource.ExternalTexture->ContentValid.begin());
                    resource.ExternalTexture->Written = resource.Written;
                }
            } else if (resource.ExternalBuffer) {
                resource.ExternalBuffer->State = static_cast<render::BufferState>(resource.States[0]);
                resource.ExternalBuffer->ContentValid = resource.Valid[0] != 0;
                resource.ExternalBuffer->Written = resource.Written;
            } else if (resource.PoolBuffer)
                resource.PoolBuffer->State = static_cast<render::BufferState>(resource.States[0]);
        }
    }
    bool ValidateResources();
    bool NormalizePasses();
    void Cull();
    bool Realize();
    void PlanBarriers();
};

RenderGraph::RenderGraph(render::Device& device, RenderResourcePool& pool, render::RenderPassRegistry& registry, std::string_view name)
    : _impl(make_unique<Impl>(device, pool, registry, nullptr, name)) {}
RenderGraph::RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                         render::RenderPassRegistry& registry, std::string_view name)
    : _impl(make_unique<Impl>(device, resources.GetPool(), registry, &resources, name)) {}
RenderGraph::RenderGraph(render::Device& device, RenderGraphFrameResources& resources,
                         render::RenderPassRegistry& registry, std::string_view name, uint64_t& generation)
    : RenderGraph(device, resources, registry, name) { generation = _impl->Generation; }
RenderGraph::~RenderGraph() = default;
uint64_t RenderGraph::GetGeneration() const noexcept { return _impl->Generation; }
const RenderGraphExecutionReport& RenderGraph::GetReport() const noexcept { return _impl->Report; }

RgTextureHandle RenderGraph::CreateTexture(const render::TextureDescriptor& desc, std::string_view name, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    const auto index = static_cast<uint32_t>(impl.Resources.size());
    Impl::Resource resource{};
    resource.Name = name;
    resource.Location = location;
    resource.IsTexture = true;
    resource.TextureDesc = desc;
    impl.Resources.push_back(std::move(resource));
    return {index, impl.Generation};
}
RgBufferHandle RenderGraph::CreateBuffer(const render::BufferDescriptor& desc, std::string_view name, std::source_location location) {
    auto& impl = *_impl;
    if (!impl.Mutable()) return {};
    const auto index = static_cast<uint32_t>(impl.Resources.size());
    Impl::Resource resource{};
    resource.Name = name;
    resource.Location = location;
    resource.BufferDesc = desc;
    impl.Resources.push_back(std::move(resource));
    return {index, impl.Generation};
}
RgTextureHandle RenderGraph::ImportTexture(RenderExternalTexture& texture, std::string_view name, RenderGraphExternalAccess access, std::source_location location) {
    auto& impl = *_impl;
    for (const auto& existing : impl.Resources) {
        if (existing.ExternalTexture && existing.ExternalTexture->Texture == texture.Texture) {
            impl.Error("DuplicateExternal", "A native texture may be imported only once per graph");
            return {};
        }
    }
    auto result = CreateTexture(texture.Desc, name, location);
    if (result.IsValid()) {
        auto& resource = impl.Resources[result.Index];
        resource.ExternalTexture = &texture;
        resource.ExternalAccess = access;
    }
    return result;
}
RgBufferHandle RenderGraph::ImportBuffer(RenderExternalBuffer& buffer, std::string_view name, RenderGraphExternalAccess access, std::source_location location) {
    auto& impl = *_impl;
    for (const auto& existing : impl.Resources) {
        if (existing.ExternalBuffer && existing.ExternalBuffer->Buffer == buffer.Buffer) {
            impl.Error("DuplicateExternal", "A native buffer may be imported only once per graph");
            return {};
        }
    }
    auto result = CreateBuffer(buffer.Desc, name, location);
    if (result.IsValid()) {
        auto& resource = impl.Resources[result.Index];
        resource.ExternalBuffer = &buffer;
        resource.ExternalAccess = access;
    }
    return result;
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

RgTextureViewHandle RenderGraph::UseTexture(uint32_t pass, RgTextureHandle texture, RgTextureViewDesc view,
                                             render::TextureViewUsage usage, bool read, bool write, bool validAfter,
                                             render::ShaderStages uavWriteStages) {
    auto& impl = *_impl;
    if (!impl.Mutable() || !impl.Handle(texture.Index, texture.Generation, true, pass)) return {};
    const auto& desc = impl.Resources[texture.Index].TextureDesc;
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
    const TextureViewKey key{view.Dimension, view.Format, *range, usage};
    uint32_t index = 0;
    for (; index < impl.Views.size(); ++index)
        if (impl.Views[index].Resource == texture.Index && impl.Views[index].Key == key) break;
    if (index == impl.Views.size()) impl.Views.push_back({texture.Index, key, nullptr});
    impl.Passes[pass].Accesses.push_back({texture.Index, *range, StateFor(usage), read, write, validAfter});
    AddUnique(impl.Passes[pass].DeclaredViews, index);
    if (write && usage == render::TextureViewUsage::UnorderedAccess &&
        impl.Report.Passes[pass].Type == RgPassType::Raster) {
        auto& rasterPass = impl.Passes[pass];
        rasterPass.AllowUavWrites = true;
        rasterPass.UavWriteStages |= uavWriteStages ? uavWriteStages : render::ShaderStages{render::ShaderStage::Graphics};
    }
    return {index, impl.Generation};
}
RgBufferHandle RenderGraph::UseBuffer(uint32_t pass, RgBufferHandle buffer, RgBufferAccess access, bool read, bool write,
                                      render::ShaderStages uavWriteStages) {
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
    impl.Passes[pass].Accesses.push_back({buffer.Index, {0, 1, 0, 1}, static_cast<uint32_t>(state), read, write, true});
    AddUnique(impl.Passes[pass].DeclaredBuffers, buffer.Index);
    if (write && access == RgBufferAccess::UnorderedAccess &&
        impl.Report.Passes[pass].Type == RgPassType::Raster) {
        auto& rasterPass = impl.Passes[pass];
        rasterPass.AllowUavWrites = true;
        rasterPass.UavWriteStages |= uavWriteStages ? uavWriteStages : render::ShaderStages{render::ShaderStage::Graphics};
    }
    return buffer;
}
RgTextureViewHandle RenderGraphPassBuilder::ReadTexture(RgTextureHandle texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::Resource, true, false, true); }
RgBufferHandle RenderGraphPassBuilder::ReadBuffer(RgBufferHandle buffer, RgBufferAccess access) { return _graph.UseBuffer(_pass, buffer, access, true, false); }
RgBufferHandle RenderGraphPassBuilder::WriteBuffer(RgBufferHandle buffer, RgBufferAccess access) { return _graph.UseBuffer(_pass, buffer, access, false, true); }
RgBufferHandle RenderGraphPassBuilder::ReadWriteBuffer(RgBufferHandle buffer, RgBufferAccess access) { return _graph.UseBuffer(_pass, buffer, access, true, true); }
RgIndirectArgumentsHandle RenderGraphPassBuilder::ReadIndirectArguments(
    RgBufferHandle buffer, RgIndirectCommand command, uint64_t offset, uint32_t count) {
    return _graph.AddIndirectArguments(_pass, buffer, command, offset, count);
}
RgParameterSetHandle RenderGraphPassBuilder::CreateParameterSet(
    ShaderProgram& program, uint32_t group, std::span<const RgParameterBinding> bindings) {
    return _graph.AddParameterSet(_pass, program, group, bindings);
}
void RenderGraphPassBuilder::SetSideEffect() { _graph._impl->Passes[_pass].SideEffect = true; }
RgTextureViewHandle RenderGraphComputeBuilder::WriteTexture(RgTextureHandle texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess, false, true, true); }
RgTextureViewHandle RenderGraphComputeBuilder::ReadWriteTexture(RgTextureHandle texture, const RgTextureViewDesc& view) { return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess, true, true, true); }
RgComputeProgramHandle RenderGraphComputeBuilder::UseComputeProgram(ShaderProgram& program) { return _graph.AddComputeProgram(_pass, program); }
RgTextureViewHandle RenderGraphRasterBuilder::WriteTexture(
    RgTextureHandle texture, render::ShaderStages stages, const RgTextureViewDesc& view) {
    return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess,
                             false, true, true, stages);
}
RgTextureViewHandle RenderGraphRasterBuilder::ReadWriteTexture(
    RgTextureHandle texture, render::ShaderStages stages, const RgTextureViewDesc& view) {
    return _graph.UseTexture(_pass, texture, view, render::TextureViewUsage::UnorderedAccess,
                             true, true, true, stages);
}

RgIndirectArgumentsHandle RenderGraph::AddIndirectArguments(
    uint32_t pass, RgBufferHandle buffer, RgIndirectCommand command,
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
            if (!UseBuffer(pass, buffer->Buffer, graphAccess, read, write, info->Stages).IsValid()) return {};
            if (write && (range->Offset != 0 || range->Size != impl.Resources[buffer->Buffer.Index].BufferDesc.Size)) {
                // Buffer contents are tracked as one cell. A partial write therefore consumes the
                // previous whole-resource contents so the untouched bytes remain valid.
                impl.Passes[pass].Accesses.back().Read = true;
            }
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
                fail("MissingParameterBinding", "A required binding array element is missing", name.value());
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
RgTextureViewHandle RenderGraphRasterBuilder::SetColorAttachment(uint32_t slot, RgTextureHandle texture, const RgColorAttachmentDesc& desc) {
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
RgTextureViewHandle RenderGraphRasterBuilder::SetDepthAttachment(RgTextureHandle texture, const RgDepthAttachmentDesc& desc) {
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

RgPassHandle RenderGraph::AddCopyBufferPass(std::string_view name, RgBufferHandle source, RgBufferHandle destination,
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
    if (size != destinationSize) _impl->Passes[pass.Index].Accesses.back().Read = true;
    _impl->Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::Buffer, source.Index, destination.Index, size, sourceOffset, destinationOffset};
    return pass;
}
RgPassHandle RenderGraph::AddCopyTexturePass(std::string_view name, RgTextureHandle source, RgTextureHandle destination,
                                             render::SubresourceRange sourceRange, render::SubresourceRange destinationRange, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) || !impl.Handle(destination.Index, destination.Generation, true, pass.Index)) return pass;
    const auto& src = impl.Resources[source.Index].TextureDesc;
    const auto& dst = impl.Resources[destination.Index].TextureDesc;
    const auto sr = render::NormalizeSubresourceRange(src, sourceRange);
    const auto dr = render::NormalizeSubresourceRange(dst, destinationRange);
    if (!sr || !dr || sr->MipLevelCount != 1 || dr->MipLevelCount != 1 || sr->ArrayLayerCount != dr->ArrayLayerCount ||
        src.Format != dst.Format || src.SampleCount != 1 || dst.SampleCount != 1 || src.Dim == render::TextureDimension::Dim3D || dst.Dim == render::TextureDimension::Dim3D ||
        std::max(1u, src.Width >> sr->BaseMipLevel) != std::max(1u, dst.Width >> dr->BaseMipLevel) ||
        std::max(1u, src.Height >> sr->BaseMipLevel) != std::max(1u, dst.Height >> dr->BaseMipLevel) ||
        !src.Usage.HasFlag(render::TextureUse::CopySource) || !dst.Usage.HasFlag(render::TextureUse::CopyDestination)) {
        impl.Error("CopyTextureDescriptor", "Texture copy needs matching format, mip extents, layer counts and copy usages", pass.Index);
        return pass;
    }
    impl.Passes[pass.Index].Accesses.push_back({source.Index, *sr, static_cast<uint32_t>(render::TextureState::CopySource), true, false, true});
    impl.Passes[pass.Index].Accesses.push_back({destination.Index, *dr, static_cast<uint32_t>(render::TextureState::CopyDestination), false, true, true});
    impl.Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::Texture, source.Index, destination.Index, 0, 0, 0, *sr, *dr};
    return pass;
}
RgPassHandle RenderGraph::AddResolveTexturePass(
    std::string_view name, RgTextureHandle source, RgTextureHandle destination,
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
    impl.Passes[pass.Index].Accesses.push_back({source.Index, *sr, static_cast<uint32_t>(render::TextureState::ResolveSource), true, false, true});
    impl.Passes[pass.Index].Accesses.push_back({destination.Index, *dr, static_cast<uint32_t>(render::TextureState::ResolveDestination), false, true, true});
    impl.Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::Resolve, source.Index, destination.Index, 0, 0, 0, *sr, *dr};
    return pass;
}
RgPassHandle RenderGraph::AddCopyTextureToBufferPass(std::string_view name, RgTextureHandle source, RgBufferHandle destination,
                                                     render::SubresourceRange range, uint64_t destinationOffset, std::source_location location) {
    auto pass = AddPass(name, RgPassType::Copy, location);
    auto& impl = *_impl;
    if (!pass.IsValid() || !impl.Handle(source.Index, source.Generation, true, pass.Index) ||
        !UseBuffer(pass.Index, destination, RgBufferAccess::CopyDestination, false, true).IsValid()) return pass;
    const auto& src = impl.Resources[source.Index].TextureDesc;
    auto normalized = render::NormalizeSubresourceRange(src, range);
    const auto& detail = impl.Device.GetDetail();
    if (!normalized || normalized->MipLevelCount != 1 || normalized->ArrayLayerCount != 1 || src.Dim == render::TextureDimension::Dim3D ||
        src.SampleCount != 1 || !src.Usage.HasFlag(render::TextureUse::CopySource) || destinationOffset % detail.TextureDataPlacementAlignment != 0) {
        impl.Error("CopyTextureRange", "Texture readback requires one non-MSAA 2D subresource and aligned destination", pass.Index);
        return pass;
    }
    const uint64_t row = Align(uint64_t{std::max(1u, src.Width >> normalized->BaseMipLevel)} * render::GetTextureFormatBytesPerPixel(src.Format), detail.TextureDataPitchAlignment);
    const uint64_t size = row * std::max(1u, src.Height >> normalized->BaseMipLevel);
    const auto destinationSize = impl.Resources[destination.Index].BufferDesc.Size;
    if (destinationOffset > destinationSize || size > destinationSize - destinationOffset) {
        impl.Error("CopyRange", "Readback buffer is too small for the aligned texture footprint", pass.Index);
        return pass;
    }
    // Buffer validity is whole-resource; untouched bytes must have prior valid contents.
    if (size != destinationSize) impl.Passes[pass.Index].Accesses.back().Read = true;
    impl.Passes[pass.Index].Accesses.push_back({source.Index, *normalized, static_cast<uint32_t>(render::TextureState::CopySource), true, false, true});
    impl.Passes[pass.Index].CopyOp = Impl::Copy{Impl::CopyType::TextureToBuffer, source.Index, destination.Index, size, 0, destinationOffset, *normalized};
    return pass;
}

bool RenderGraph::Impl::ValidateResources() {
    Report.Resources.reserve(Resources.size());
    for (uint32_t index = 0; index < Resources.size(); ++index) {
        auto& resource = Resources[index];
        string descriptor;
        if (resource.IsTexture) {
            ++Report.Textures;
            auto desc = resource.TextureDesc;
            if (resource.External()) desc.Hints = desc.Hints & render::ResourceHint::Dedicated;
            const auto validation = render::ValidateTextureDescriptor(desc, Device);
            if (!validation.Supported) {
                Error("UnsupportedTexture", validation.Reason, InvalidIndex, index);
                continue;
            }
            descriptor = fmt::format("{} {}x{}x{} mips={} samples={} usage={}", desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Usage);
            resource.Valid.assign(resource.CellCount(), 0);
            if (resource.ExternalTexture) {
                const auto& external = *resource.ExternalTexture;
                if (!(TexturePoolKey{external.Texture->GetDesc()} == TexturePoolKey{external.Desc}) || external.SubresourceStates.size() != resource.CellCount() || external.ContentValid.size() != resource.CellCount()) {
                    Error("ExternalStorage", "External descriptor or state/validity storage does not match native texture", InvalidIndex, index);
                    continue;
                }
                std::copy(external.ContentValid.begin(), external.ContentValid.end(), resource.Valid.begin());
                for (size_t cell = 0; cell < external.SubresourceStates.size(); ++cell) {
                    const auto state = external.SubresourceStates[cell];
                    if (!state || (resource.Valid[cell] && state.HasFlag(render::TextureState::Undefined))) Error("ExternalState", "Valid external contents require a defined state", InvalidIndex, index);
                }
            }
        } else {
            ++Report.Buffers;
            const auto& desc = resource.BufferDesc;
            constexpr uint32_t knownUses = 2047;
            if (desc.Size == 0 || desc.Size > Device.GetCapabilities().Limits.MaxBufferSize || !EnumContains(desc.Memory) || !desc.Usage ||
                (desc.Usage.value() & ~knownUses) != 0 || (desc.Hints.value() & ~uint32_t{7}) != 0 ||
                (!resource.External() && desc.Hints.HasFlag(render::ResourceHint::External)) ||
                (desc.Memory == render::MemoryType::Upload && !desc.Usage.HasFlag(render::BufferUse::MapWrite)) ||
                (desc.Memory == render::MemoryType::ReadBack && !desc.Usage.HasFlag(render::BufferUse::MapRead))) {
                Error("UnsupportedBuffer", "Invalid buffer size, usage, memory or hints", InvalidIndex, index);
                continue;
            }
            descriptor = fmt::format("size={} memory={} usage={}", desc.Size, EnumName(desc.Memory), desc.Usage);
            resource.Valid.assign(1, resource.ExternalBuffer && resource.ExternalBuffer->ContentValid ? 1 : 0);
            if (resource.ExternalBuffer && (!(BufferPoolKey{resource.ExternalBuffer->Buffer->GetDesc()} == BufferPoolKey{desc}) || !resource.ExternalBuffer->State ||
                                            (resource.Valid[0] && resource.ExternalBuffer->State.HasFlag(render::BufferState::Undefined)))) {
                Error("ExternalStorage", "External buffer descriptor or initial state is invalid", InvalidIndex, index);
            }
        }
        if (!EnumContains(resource.ExternalAccess)) Error("ExternalAccess", "Invalid external access mode", InvalidIndex, index);
        Report.Resources.push_back({resource.Name, std::move(descriptor), resource.IsTexture, resource.External()});
    }
    return Report.Diagnostics.empty();
}

bool RenderGraph::Impl::NormalizePasses() {
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        auto& pass = Passes[p];
        unordered_map<uint64_t, uint32_t> cellMap;
        for (const auto& access : pass.Accesses) {
            auto& resource = Resources[access.Resource];
            if (access.Write && resource.External() && resource.ExternalAccess == RenderGraphExternalAccess::ReadOnly) Error("ReadOnlyExternal", "Cannot write a read-only external resource", p, access.Resource);
            for (uint32_t layer = access.Range.BaseArrayLayer; layer < access.Range.BaseArrayLayer + access.Range.ArrayLayerCount; ++layer) {
                for (uint32_t mip = access.Range.BaseMipLevel; mip < access.Range.BaseMipLevel + access.Range.MipLevelCount; ++mip) {
                    const uint32_t cell = resource.IsTexture ? layer * resource.TextureDesc.MipLevels + mip : 0;
                    const uint64_t key = (uint64_t{access.Resource} << 32) | cell;
                    auto [it, inserted] = cellMap.emplace(key, static_cast<uint32_t>(pass.Cells.size()));
                    if (inserted)
                        pass.Cells.push_back({access.Resource, cell, access.State, access.Read, access.Write, access.ValidAfter});
                    else {
                        auto& existing = pass.Cells[it->second];
                        if (existing.Write || access.Write)
                            Error("OverlappingAccess", "Overlapping writes require one explicit read-write declaration; attachment feedback is unsupported", p, access.Resource);
                        else {
                            const auto uav = static_cast<uint32_t>(resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess));
                            if (existing.State != access.State && ((existing.State | access.State) & uav)) Error("IncompatibleReadStates", "UAV and other read layouts cannot be combined", p, access.Resource);
                            existing.State |= access.State;
                        }
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
    struct Content {
        int32_t Producer{-1};
        bool Valid{false};
    };
    vector<vector<Content>> contents(Resources.size());
    for (size_t r = 0; r < Resources.size(); ++r) {
        contents[r].resize(Resources[r].CellCount());
        for (size_t c = 0; c < contents[r].size(); ++c) contents[r][c].Valid = Resources[r].Valid[c] != 0;
    }
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        for (const auto& access : Passes[p].Cells) {
            auto& content = contents[access.Resource][access.Cell];
            if (access.Read) {
                if (!content.Valid) Error("UninitializedRead", fmt::format("Subresource {} has no valid contents (read/Load after creation or Discard)", access.Cell), p, access.Resource);
                if (content.Producer >= 0) AddUnique(Report.Passes[p].DataDependencies, static_cast<uint32_t>(content.Producer));
            }
            if (access.Write) content = {static_cast<int32_t>(p), access.ValidAfter};
        }
    }
    vector<uint32_t> pending;
    for (uint32_t p = 0; p < Passes.size(); ++p)
        if (Passes[p].SideEffect) {
            pending.push_back(p);
            Report.Passes[p].LivenessReason = "explicit side effect";
        }
    for (size_t r = 0; r < Resources.size(); ++r)
        if (Resources[r].ExternalAccess == RenderGraphExternalAccess::ObservableOutput)
            for (const auto& content : contents[r])
                if (content.Producer >= 0) {
                    pending.push_back(static_cast<uint32_t>(content.Producer));
                    Report.Passes[content.Producer].LivenessReason = "observable final content";
                }
    while (!pending.empty()) {
        const uint32_t p = pending.back();
        pending.pop_back();
        auto& report = Report.Passes[p];
        if (report.Live) continue;
        report.Live = true;
        if (report.LivenessReason.empty()) report.LivenessReason = "content consumed by a live pass";
        pending.insert(pending.end(), report.DataDependencies.begin(), report.DataDependencies.end());
    }
    struct Hazard {
        int32_t Writer{-1};
        vector<uint32_t> Readers;
    };
    vector<vector<Hazard>> hazards(Resources.size());
    for (size_t r = 0; r < Resources.size(); ++r) hazards[r].resize(Resources[r].CellCount());
    // Every hazard edge points forward in declaration order, which is already the stable topological order.
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        auto& passReport = Report.Passes[p];
        if (!passReport.Live) {
            passReport.LivenessReason = "unconsumed or overwritten content";
            continue;
        }
        ++Report.LivePasses;
        for (const auto& access : Passes[p].Cells) {
            auto& hazard = hazards[access.Resource][access.Cell];
            if (hazard.Writer >= 0) AddUnique(passReport.HazardDependencies, static_cast<uint32_t>(hazard.Writer));
            if (access.Write) {
                for (const auto reader : hazard.Readers) AddUnique(passReport.HazardDependencies, reader);
                hazard.Readers.clear();
                hazard.Writer = static_cast<int32_t>(p);
            } else
                AddUnique(hazard.Readers, p);
            auto& report = Report.Resources[access.Resource];
            if (report.FirstUse < 0) report.FirstUse = static_cast<int32_t>(p);
            report.LastUse = static_cast<int32_t>(p);
        }
        std::sort(passReport.DataDependencies.begin(), passReport.DataDependencies.end());
        std::sort(passReport.HazardDependencies.begin(), passReport.HazardDependencies.end());
    }
    Report.CulledPasses = Report.DeclaredPasses - Report.LivePasses;
}

bool RenderGraph::Compile() {
    auto& impl = *_impl;
    if (impl.Frozen) return impl.Compiled && impl.Report.Diagnostics.empty();
    impl.Frozen = true;
    impl.Report.DeclaredPasses = static_cast<uint32_t>(impl.Passes.size());
    if (!impl.Report.Diagnostics.empty() || !impl.ValidateResources() || !impl.NormalizePasses()) return false;
    impl.Cull();
    impl.Compiled = impl.Report.Diagnostics.empty();
    return impl.Compiled;
}

bool RenderGraph::Impl::Realize() {
    const uint64_t createdBefore = Pool.GetStats().Created;
    for (uint32_t r = 0; r < Resources.size(); ++r) {
        auto& resource = Resources[r];
        if (Report.Resources[r].FirstUse < 0) continue;
        if (resource.IsTexture) {
            if (!resource.ExternalTexture) {
                resource.PoolTexture = Pool.AcquireTexture(resource.TextureDesc, resource.Name);
                if (!resource.PoolTexture) {
                    Error("TextureAllocation", "Texture allocation failed before recording", InvalidIndex, r);
                    return false;
                }
                Report.Resources[r].PhysicalId = resource.PoolTexture->Id;
            }
            const auto states = resource.ExternalTexture ? std::span<const render::TextureStates>{resource.ExternalTexture->SubresourceStates} : std::span<const render::TextureStates>{resource.PoolTexture->States};
            for (const auto state : states) resource.States.push_back(state.value());
        } else {
            if (!resource.ExternalBuffer) {
                resource.PoolBuffer = Pool.AcquireBuffer(resource.BufferDesc, resource.Name);
                if (!resource.PoolBuffer) {
                    Error("BufferAllocation", "Buffer allocation failed before recording", InvalidIndex, r);
                    return false;
                }
                Report.Resources[r].PhysicalId = resource.PoolBuffer->Id;
            }
            resource.States.push_back((resource.ExternalBuffer ? resource.ExternalBuffer->State : resource.PoolBuffer->State).value());
        }
    }
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        if (!Report.Passes[p].Live) continue;
        auto& pass = Passes[p];
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
        vector<render::RenderPassColorAttachmentDescriptor> colors;
        vector<render::TextureView*> views;
        vector<render::TextureFormat> formats;
        for (const auto& attachment : pass.Colors) {
            const auto& view = Views[attachment->View];
            colors.push_back({view.Key.Format, pass.Samples, attachment->Desc.Load, attachment->Desc.Store});
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
            depth = render::RenderPassDepthStencilAttachmentDescriptor{view.Key.Format, pass.Samples, attachment.Desc.Load, attachment.Desc.Store, attachment.Desc.Load, attachment.Desc.Store, attachment.Desc.ReadOnly};
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
    }
    Report.PhysicalAllocations = static_cast<uint32_t>(Pool.GetStats().Created - createdBefore);
    return true;
}

bool RenderGraph::Prepare() {
    auto& impl = *_impl;
    for (Impl::ComputeProgram& value : impl.ComputePrograms) {
        if (!impl.Report.Passes[value.Pass].Live) continue;
        value.PipelineState = value.Program->GetOrCreateComputePipelineState();
        if (!value.PipelineState) {
            impl.Error("ComputePipelineState", "Compute pipeline state creation failed before recording", value.Pass);
            return false;
        }
    }

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
            impl.Device.CreateShaderParameterSet({
                .Layout = key.Layout,
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
    return true;
}

void RenderGraph::Impl::PlanBarriers() {
    vector<vector<uint32_t>> states;
    vector<vector<uint8_t>> writes;
    for (const auto& resource : Resources) {
        states.push_back(resource.States);
        // An initial UAV state may contain writes from an earlier graph or flight.
        writes.emplace_back(resource.States.size(), 1);
    }
    for (uint32_t p = 0; p < Passes.size(); ++p) {
        if (!Report.Passes[p].Live) continue;
        auto& pass = Passes[p];
        vector<uint32_t> uavResources;
        for (const auto& access : pass.Cells) {
            auto& resource = Resources[access.Resource];
            auto& state = states[access.Resource][access.Cell];
            const uint32_t uav = resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess);
            if (state != access.State) {
                if (resource.IsTexture) {
                    pass.Barriers.push_back(render::BarrierTextureDescriptor{.Target = resource.NativeTexture(), .Before = static_cast<render::TextureState>(state), .After = static_cast<render::TextureState>(access.State), .IsSubresourceBarrier = true, .Range = {access.Cell / resource.TextureDesc.MipLevels, 1, access.Cell % resource.TextureDesc.MipLevels, 1}});
                } else
                    pass.Barriers.push_back(render::BarrierBufferDescriptor{.Target = resource.NativeBuffer(), .Before = static_cast<render::BufferState>(state), .After = static_cast<render::BufferState>(access.State)});
                Report.Barriers.push_back({p, access.Resource, access.Cell, state, access.State, false});
                ++Report.TransitionBarriers;
            } else if (state == uav && (writes[access.Resource][access.Cell] || access.Write))
                AddUnique(uavResources, access.Resource);
            state = access.State;
            writes[access.Resource][access.Cell] = access.Write ? 1 : 0;
        }
        for (const auto r : uavResources) {
            auto& resource = Resources[r];
            render::Resource* native = resource.IsTexture ? static_cast<render::Resource*>(resource.NativeTexture()) : static_cast<render::Resource*>(resource.NativeBuffer());
            pass.Barriers.push_back(render::BarrierUavDescriptor{native});
            Report.Barriers.push_back({p, r, 0, resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess),
                                       resource.IsTexture ? uint32_t(render::TextureState::UnorderedAccess) : uint32_t(render::BufferState::UnorderedAccess), true});
            ++Report.UavBarriers;
        }
    }
}

RenderGraphExecutionResult RenderGraph::Execute(render::CommandBuffer& command) {
    auto& impl = *_impl;
    if (impl.Executed) {
        impl.Error("AlreadyExecuted", "A graph may execute only once");
        return {};
    }
    impl.Executed = true;
    if (!Compile() || !impl.Realize() || !Prepare()) {
        impl.Pool.EndGraph();
        impl.Report.Pool = impl.Pool.GetStats();
        return {};
    }
    impl.PlanBarriers();
    RenderGraphExecutionResult result{true, false};
    for (uint32_t p = 0; p < impl.Passes.size(); ++p) {
        auto& report = impl.Report.Passes[p];
        if (!report.Live) continue;
        auto& pass = impl.Passes[p];
        command.PushDebugGroup(report.Name);
        result.CommandsRecorded = true;
        if (!pass.Barriers.empty()) command.ResourceBarrier(pass.Barriers);
        for (const auto& access : pass.Cells) impl.Resources[access.Resource].States[access.Cell] = access.State;
        if (report.Type == RgPassType::Raster) {
            const auto depthClear = pass.DepthAttachment ? std::optional{pass.DepthAttachment->Desc.Clear} : std::nullopt;
            auto encoder = command.BeginRenderPass({pass.NativePass.Get(), pass.Framebuffer.Get(), pass.Clears,
                                                    depthClear, report.Name, pass.AllowUavWrites});
            if (!encoder) {
                impl.Error("BeginRenderPass", "Encoder creation failed after barriers; actual states are committed for host recovery", p);
                result.Success = false;
            } else {
                RenderGraphRasterContext context(*this, p, *encoder);
                if (pass.Data) pass.Data->Run(context);
                command.EndRenderPass(encoder.Release());
            }
        } else if (report.Type == RgPassType::Compute) {
            auto encoder = command.BeginComputePass();
            if (!encoder) {
                impl.Error("BeginComputePass", "Encoder creation failed after barriers; actual states are committed for host recovery", p);
                result.Success = false;
            } else {
                RenderGraphComputeContext context(*this, p, *encoder);
                if (pass.Data) pass.Data->Run(context);
                command.EndComputePass(encoder.Release());
            }
        } else if (pass.CopyOp) {
            const auto& copy = *pass.CopyOp;
            auto& src = impl.Resources[copy.Source];
            auto& dst = impl.Resources[copy.Destination];
            if (copy.Type == Impl::CopyType::Buffer)
                command.CopyBufferToBuffer(dst.NativeBuffer(), copy.DestinationOffset, src.NativeBuffer(), copy.SourceOffset, copy.Size);
            else if (copy.Type == Impl::CopyType::TextureToBuffer)
                command.CopyTextureToBuffer(dst.NativeBuffer(), copy.DestinationOffset, src.NativeTexture(), copy.SourceRange);
            else if (copy.Type == Impl::CopyType::Resolve)
                command.ResolveTexture({
                    .Destination = dst.NativeTexture(),
                    .DestinationMipLevel = copy.DestinationRange.BaseMipLevel,
                    .DestinationArrayLayer = copy.DestinationRange.BaseArrayLayer,
                    .Source = src.NativeTexture(),
                    .SourceMipLevel = copy.SourceRange.BaseMipLevel,
                    .SourceArrayLayer = copy.SourceRange.BaseArrayLayer,
                    .ArrayLayerCount = copy.SourceRange.ArrayLayerCount});
            else
                command.CopyTextureToTexture({.Destination = dst.NativeTexture(), .DestinationMipLevel = copy.DestinationRange.BaseMipLevel, .DestinationArrayLayer = copy.DestinationRange.BaseArrayLayer, .Source = src.NativeTexture(), .SourceMipLevel = copy.SourceRange.BaseMipLevel, .SourceArrayLayer = copy.SourceRange.BaseArrayLayer, .Width = std::max(1u, src.TextureDesc.Width >> copy.SourceRange.BaseMipLevel), .Height = std::max(1u, src.TextureDesc.Height >> copy.SourceRange.BaseMipLevel), .ArrayLayerCount = copy.SourceRange.ArrayLayerCount});
        }
        command.PopDebugGroup();
        if (!result.Success) break;
        report.Executed = true;
        for (const auto& access : pass.Cells)
            if (access.Write) {
                auto& resource = impl.Resources[access.Resource];
                resource.Valid[access.Cell] = access.ValidAfter ? 1 : 0;
                resource.Written = access.ValidAfter;
            }
    }
    impl.CommitStates();
    impl.Pool.EndGraph();
    impl.Report.Pool = impl.Pool.GetStats();
    return result;
}

bool RenderGraph::WasWritten(RgTextureHandle handle) const noexcept {
    return handle.Generation == _impl->Generation && handle.Index < _impl->Resources.size() && _impl->Resources[handle.Index].IsTexture && _impl->Resources[handle.Index].Written;
}
render::TextureView* RenderGraph::ResolveView(uint32_t pass, RgTextureViewHandle handle) const {
    const auto& impl = *_impl;
    const auto& declared = impl.Passes[pass].DeclaredViews;
    if (handle.Generation != impl.Generation || std::find(declared.begin(), declared.end(), handle.Index) == declared.end() || !impl.Views[handle.Index].Native) RADRAY_ABORT("RenderGraph texture view was not declared by this pass");
    return impl.Views[handle.Index].Native.Get();
}
render::Buffer* RenderGraph::ResolveBuffer(uint32_t pass, RgBufferHandle handle) const {
    const auto& impl = *_impl;
    const auto& declared = impl.Passes[pass].DeclaredBuffers;
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
render::Buffer* RenderGraphRasterContext::GetBuffer(RgBufferHandle handle) const { return _graph.ResolveBuffer(_pass, handle); }
void RenderGraphRasterContext::BindParameterSet(RgParameterSetHandle handle) noexcept {
    _graph.BindParameterSet(_pass, handle, &_encoder._encoder, nullptr);
}
const GraphicsPassState& RenderGraphRasterContext::PassState() const noexcept { return *_graph._impl->Passes[_pass].PassState; }
render::TextureView* RenderGraphComputeContext::GetTextureView(RgTextureViewHandle handle) const { return _graph.ResolveView(_pass, handle); }
render::Buffer* RenderGraphComputeContext::GetBuffer(RgBufferHandle handle) const { return _graph.ResolveBuffer(_pass, handle); }
void RenderGraphComputeContext::BindParameterSet(RgParameterSetHandle handle) noexcept {
    _graph.BindParameterSet(_pass, handle, nullptr, &_encoder._encoder);
}
void RenderGraphComputeContext::BindComputeProgram(RgComputeProgramHandle handle) noexcept {
    _graph.BindComputeProgram(_pass, handle, _encoder._encoder);
}

}  // namespace radray
