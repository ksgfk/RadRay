#include <radray/runtime/rdg.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <numeric>
#include <map>
#include <memory_resource>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_device.h>
#include <radray/gpu_impl/validation.h>

#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray {
namespace detail {

enum class RDGPassKind { Raster,
                         Compute,
                         Copy,
                         Upload,
                         HostWrite };

struct RDGColorAttachment {
    uint32_t Slot;
    RDGTextureHandle Texture;
    gpu::LoadAction Load;
    gpu::StoreAction Store;
    gpu::ColorClearValue Clear;
    Nullable<gpu::TextureView*> View;
    gpu::SubresourceRange Range{0, gpu::SubresourceRange::All, 0, 1};
};

struct RDGDepthAttachment {
    RDGTextureHandle Texture;
    gpu::TextureState State;
    gpu::LoadAction DepthLoad;
    gpu::StoreAction DepthStore;
    gpu::LoadAction StencilLoad;
    gpu::StoreAction StencilStore;
    gpu::DepthStencilClearValue Clear;
    Nullable<gpu::TextureView*> View;
    gpu::SubresourceRange Range{0, gpu::SubresourceRange::All, 0, 1};
};

struct RDGUserPass {
    std::pmr::string Name;
    RDGAccessTable AccessTable;
    std::pmr::vector<MeshPassId> MeshPasses;
    std::pmr::vector<RDGColorAttachment> Colors;
    std::optional<RDGDepthAttachment> Depth{};
    ErasedPassCallback Callback{};
    Nullable<bool (*)(const void*, RDGRasterPassContext&) noexcept> RasterExecute{nullptr};
    Nullable<bool (*)(const void*, RDGComputePassContext&) noexcept> ComputeExecute{nullptr};
    Nullable<bool (*)(const void*, RDGCopyPassContext&) noexcept> CopyExecute{nullptr};
    Nullable<bool (*)(void*, RDGPrepareContext&) noexcept> Prepare{nullptr};
    uint32_t Width{1}, Height{1}, Layers{1};
    std::optional<Rect> RenderArea{};
};

struct RDGUpload {
    RDGUploadSource Source;
    RDGAccess Access;
    uint64_t UploadOffset{0};
    uint64_t SourceRowPitch{0};
    gpu::TextureRegion2D Region{};
    uint32_t SourceRowsPerImage{0};
};

struct RDGPass {
    RDGPassKind Kind;
    uint32_t Payload;
    std::pmr::vector<uint32_t> DataDependencies;
    std::pmr::vector<uint32_t> OrderDependencies;
    uint32_t PreparedUpload{std::numeric_limits<uint32_t>::max()};
    bool NeverCull{false};
    bool Alive{false};
    bool OrderBarrier{false};
};

struct RDGPreparedUpload {
    uint64_t ActualSize{0};
    uint32_t StagingPitch{0};
    UploadAllocation Staging{};
    std::pmr::vector<gpu::ResourceBarrierDescriptor> Barriers;
};

struct RDGTexture {
    gpu::TextureDescriptor Desc;
    Nullable<gpu::Texture*> Imported{nullptr};
    gpu::TextureState InitialState{gpu::TextureState::Undefined};
    std::optional<gpu::TextureState> FinalState{};
    std::optional<uint32_t> LastWriter{};
    std::pmr::vector<uint32_t> Readers;
    RDGExternalOwnership Ownership{};
    Nullable<GpuTexture*> Persistent{nullptr};
    IntrusivePtr<GpuTexture> Owner{};
    bool Output{false};
    bool Alive{false};
};

struct RDGBuffer {
    gpu::BufferDescriptor Desc;
    Nullable<gpu::Buffer*> Imported{nullptr};
    gpu::BufferStates InitialState{gpu::BufferState::Undefined};
    std::optional<gpu::BufferStates> FinalState{};
    std::optional<uint32_t> LastWriter{};
    std::pmr::vector<uint32_t> Readers;
    RDGExternalOwnership Ownership{};
    Nullable<GpuBuffer*> Persistent{nullptr};
    IntrusivePtr<GpuBuffer> Owner{};
    IntrusivePtr<GpuUploadBuffer> UploadOwner{};
    bool Dynamic{false};
    bool OwnerWrites{false};
    uint64_t Alignment{1};
    bool Output{false};
    bool Alive{false};
};

struct RDGExecutionGroup {
    RDGPassKind Kind;
    std::pmr::vector<uint32_t> Passes;
    std::pmr::vector<gpu::ResourceBarrierDescriptor> Barriers;
    std::pmr::vector<gpu::RenderPassColorAttachment> Colors;
    std::optional<gpu::RenderPassDepthStencilAttachment> Depth{};
    bool Uav{false};
};

struct RDGData {
    RDGData(std::pmr::memory_resource& memory, std::pmr::memory_resource& scratch) noexcept
        : MemoryResource(memory), ScratchResource(scratch) {}
    ~RDGData() noexcept;
    std::pmr::memory_resource& MemoryResource;
    std::pmr::memory_resource& ScratchResource;
    RDGDeviceConstraints Constraints;
    RDGCompileOptions Options;
    std::pmr::vector<RDGTexture> Textures{&MemoryResource};
    std::pmr::vector<RDGBuffer> Buffers{&MemoryResource};
    std::pmr::unordered_map<gpu::Texture*, uint32_t> TextureImports{&MemoryResource};
    std::pmr::unordered_map<gpu::Buffer*, uint32_t> BufferImports{&MemoryResource};
    std::pmr::vector<RDGPass> Passes{&MemoryResource};
    std::pmr::vector<RDGUserPass> UserPasses{&MemoryResource};
    std::pmr::vector<RDGUpload> Uploads{&MemoryResource};
    std::pmr::vector<uint32_t> ScheduledPasses{&MemoryResource};
    std::pmr::vector<RDGExecutionGroup> Groups{&MemoryResource};
    std::pmr::vector<MeshPassId> AliveMeshPasses{&MemoryResource};
    bool Failed{false};
    bool Sealed{false};
};

template <class T, class State>
struct RDGPhysicalResource {
    Nullable<T*> Resource{nullptr};
    State CurrentState;
    bool PendingWrite{false};
    bool PendingExternalAcquire{false};
    bool NeedsExternalRelease{false};
};

class RDGStateTracker {
public:
    explicit RDGStateTracker(RDGData& data) noexcept : _data(data) {}
    void Initialize();
    void BindTexture(uint32_t index, gpu::Texture* texture, gpu::TextureState initialState) noexcept;
    void BindBuffer(uint32_t index, gpu::Buffer* buffer, gpu::BufferStates initialState) noexcept;
    const RDGPhysicalResource<gpu::Texture, gpu::TextureState>& GetTexture(uint32_t index) const noexcept { return _textures[index]; }
    const RDGPhysicalResource<gpu::Buffer, gpu::BufferStates>& GetBuffer(uint32_t index) const noexcept { return _buffers[index]; }
    void PrepareHostWrite(uint32_t index) noexcept;
    void PrepareBarriers(UploadArena& uploads, std::span<RDGPreparedUpload> preparedUploads, std::span<const uint32_t> uploadCounts);
    std::span<const gpu::ResourceBarrierDescriptor> GetBeginBarriers() const noexcept { return _beginBarriers; }
    std::span<const gpu::ResourceBarrierDescriptor> GetFinalBarriers() const noexcept { return _finalBarriers; }
    std::span<const gpu::ResourceBarrierDescriptor> GetReleaseBarriers() const noexcept { return _releaseBarriers; }

private:
    void TransitionTexture(uint32_t index, gpu::TextureState next, bool write, std::pmr::vector<gpu::ResourceBarrierDescriptor>& barriers) noexcept;
    void TransitionBuffer(uint32_t index, gpu::BufferStates next, bool write, std::pmr::vector<gpu::ResourceBarrierDescriptor>& barriers) noexcept;
    RDGData& _data;
    std::pmr::vector<RDGPhysicalResource<gpu::Texture, gpu::TextureState>> _textures{&_data.MemoryResource};
    std::pmr::vector<RDGPhysicalResource<gpu::Buffer, gpu::BufferStates>> _buffers{&_data.MemoryResource};
    std::pmr::vector<gpu::ResourceBarrierDescriptor> _beginBarriers{&_data.MemoryResource};
    std::pmr::vector<gpu::ResourceBarrierDescriptor> _finalBarriers{&_data.MemoryResource};
    std::pmr::vector<gpu::ResourceBarrierDescriptor> _releaseBarriers{&_data.MemoryResource};
};

struct RDGExecution {
    struct FrameView {
        RDGTextureHandle Texture;
        gpu::TextureViewDescriptor Descriptor;
        gpu::TextureView* View;
    };
    RDGData& Data;
    FrameSlot& Slot;
    RDGTransientResourcePool& Pool;
    UploadArena& Uploads;
    Nullable<gpu::CommandBuffer*> Command{nullptr};
    RDGStateTracker States{Data};
    Nullable<const RDGPass*> Pass{nullptr};
    std::pmr::vector<FrameView> Views{&Data.MemoryResource};
    std::pmr::vector<unique_ptr<gpu::TextureView>> ViewOwners{&Data.MemoryResource};
    std::pmr::vector<RDGBufferSlice> Slices{&Data.MemoryResource};
    std::pmr::vector<RDGPreparedUpload> PreparedUploads{&Data.MemoryResource};
};

}  // namespace detail

namespace {

using namespace detail;

bool IsUpload(const RDGPass& pass) noexcept {
    return pass.Kind == RDGPassKind::Upload || pass.Kind == RDGPassKind::HostWrite;
}

RDGUserPass& User(RDGData& data, const RDGPass& pass) noexcept { return data.UserPasses[pass.Payload]; }
const RDGUserPass& User(const RDGData& data, const RDGPass& pass) noexcept { return data.UserPasses[pass.Payload]; }
RDGUpload& Upload(RDGData& data, const RDGPass& pass) noexcept { return data.Uploads[pass.Payload]; }
const RDGUpload& Upload(const RDGData& data, const RDGPass& pass) noexcept { return data.Uploads[pass.Payload]; }
std::span<const RDGAccess> Accesses(const RDGData& data, const RDGPass& pass) noexcept {
    if (IsUpload(pass)) return {&Upload(data, pass).Access, 1};
    return User(data, pass).AccessTable.Accesses;
}
std::string_view PassName(const RDGData& data, const RDGPass& pass) noexcept {
    if (IsUpload(pass)) return Upload(data, pass).Access.Texture ? "UploadTexture" : "UploadBuffer";
    return User(data, pass).Name;
}
RDGPreparedUpload& Prepared(RDGExecution& execution, const RDGPass& pass) noexcept { return execution.PreparedUploads[pass.PreparedUpload]; }

void RequireOpen(const RDGData& data) noexcept {
    if (data.Sealed) RADRAY_ABORT("RDG: declarations cannot change after Compile");
}

void Fail(RDGData& data, std::string_view message) noexcept {
    RADRAY_ERR_LOG("RDG: {}", message);
    data.Failed = true;
}

bool SameDescriptor(const gpu::TextureDescriptor& a, const gpu::TextureDescriptor& b) noexcept {
    return a.Dim == b.Dim && a.Width == b.Width && a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels && a.SampleCount == b.SampleCount && a.Format == b.Format && a.Memory == b.Memory && a.Usage == b.Usage && a.Hints == b.Hints;
}

bool SameDescriptor(const gpu::TextureViewDescriptor& a, const gpu::TextureViewDescriptor& b) noexcept {
    return a.Target == b.Target && a.Dim == b.Dim && a.Format == b.Format && a.Range == b.Range && a.Usage == b.Usage;
}

gpu::TextureViewUsage DepthUsage(gpu::TextureState state) noexcept {
    switch (state) {
        case gpu::TextureState::DepthRead: return gpu::TextureViewUsage::DepthRead;
        case gpu::TextureState::DepthWrite: return gpu::TextureViewUsage::DepthWrite;
        case gpu::TextureState::DepthReadStencilWrite: return gpu::TextureViewUsage::DepthReadStencilWrite;
        case gpu::TextureState::DepthWriteStencilRead: return gpu::TextureViewUsage::DepthWriteStencilRead;
        default: return gpu::TextureViewUsage::UNKNOWN;
    }
}

bool IsWriteState(gpu::TextureState state) noexcept {
    return state == gpu::TextureState::RenderTarget || state == gpu::TextureState::DepthWrite ||
           state == gpu::TextureState::DepthReadStencilWrite || state == gpu::TextureState::DepthWriteStencilRead ||
           state == gpu::TextureState::UnorderedAccess || state == gpu::TextureState::CopyDestination;
}

bool IsWriteState(gpu::BufferStates state) noexcept {
    return state == gpu::BufferState::HostWrite || state == gpu::BufferState::UnorderedAccess || state == gpu::BufferState::CopyDestination;
}

bool IsReadOnlyBufferState(gpu::BufferStates state) noexcept {
    constexpr auto kReadOnlyBufferStates = gpu::BufferState::CopySource | gpu::BufferState::Vertex | gpu::BufferState::Index |
                                           gpu::BufferState::CBuffer | gpu::BufferState::ShaderRead | gpu::BufferState::Indirect;
    return state && !(state & ~kReadOnlyBufferStates);
}

bool ValidBufferState(const gpu::BufferDescriptor& desc, gpu::BufferStates state) noexcept {
    if (IsReadOnlyBufferState(state)) {
        const auto compatible = [&](gpu::BufferState bit, gpu::BufferUse usage) {
            return !state.HasFlag(bit) || desc.Usage.HasFlag(usage);
        };
        return compatible(gpu::BufferState::CopySource, gpu::BufferUse::CopySource) &&
               compatible(gpu::BufferState::Vertex, gpu::BufferUse::Vertex) &&
               compatible(gpu::BufferState::Index, gpu::BufferUse::Index) &&
               compatible(gpu::BufferState::CBuffer, gpu::BufferUse::CBuffer) &&
               compatible(gpu::BufferState::ShaderRead, gpu::BufferUse::Resource) &&
               compatible(gpu::BufferState::Indirect, gpu::BufferUse::Indirect);
    }
    using enum gpu::BufferState;
    switch (static_cast<gpu::BufferState>(state)) {
        case Undefined:
        case Common: return true;
        case gpu::BufferState::CopySource: return desc.Usage.HasFlag(gpu::BufferUse::CopySource);
        case gpu::BufferState::CopyDestination: return desc.Usage.HasFlag(gpu::BufferUse::CopyDestination);
        case gpu::BufferState::Vertex: return desc.Usage.HasFlag(gpu::BufferUse::Vertex);
        case gpu::BufferState::Index: return desc.Usage.HasFlag(gpu::BufferUse::Index);
        case gpu::BufferState::CBuffer: return desc.Usage.HasFlag(gpu::BufferUse::CBuffer);
        case ShaderRead: return desc.Usage.HasFlag(gpu::BufferUse::Resource);
        case gpu::BufferState::UnorderedAccess: return desc.Usage.HasFlag(gpu::BufferUse::UnorderedAccess);
        case gpu::BufferState::Indirect: return desc.Usage.HasFlag(gpu::BufferUse::Indirect);
        case HostRead: return desc.Memory == gpu::MemoryType::ReadBack && desc.Usage.HasFlag(gpu::BufferUse::MapRead);
        case HostWrite: return desc.Memory == gpu::MemoryType::Upload && desc.Usage.HasFlag(gpu::BufferUse::MapWrite);
        default: return false;
    }
}

bool ValidTextureState(const gpu::TextureDescriptor& desc, gpu::TextureState state) noexcept {
    using enum gpu::TextureState;
    switch (state) {
        case Undefined:
        case Common:
        case Present: return true;
        case CopySource: return desc.Usage.HasFlag(gpu::TextureUse::CopySource);
        case CopyDestination: return desc.Usage.HasFlag(gpu::TextureUse::CopyDestination);
        case ShaderRead: return desc.Usage.HasFlag(gpu::TextureUse::Resource);
        case RenderTarget: return desc.Usage.HasFlag(gpu::TextureUse::RenderTarget) && !gpu::IsDepthStencilFormat(desc.Format);
        case DepthRead: return desc.Usage.HasFlag(gpu::TextureUse::DepthStencilRead) && gpu::IsDepthStencilFormat(desc.Format);
        case DepthWrite: return desc.Usage.HasFlag(gpu::TextureUse::DepthStencilWrite) && gpu::IsDepthStencilFormat(desc.Format);
        case DepthReadStencilWrite:
        case DepthWriteStencilRead: return desc.Usage.HasFlag(gpu::TextureUse::DepthStencilRead | gpu::TextureUse::DepthStencilWrite) && gpu::IsDepthStencilFormat(desc.Format);
        case UnorderedAccess: return desc.Usage.HasFlag(gpu::TextureUse::UnorderedAccess);
        default: return false;
    }
}

bool ValidAccess(const RDGData& data, const RDGPass& pass, const RDGAccess& access) noexcept;

void AddAccess(RDGData& data, uint32_t passIndex, RDGAccess access) {
    RequireOpen(data);
    if ((access.Texture && access.Index >= data.Textures.size()) || (!access.Texture && access.Index >= data.Buffers.size())) {
        Fail(data, "invalid resource handle");
        return;
    }
    auto& pass = data.Passes[passIndex];
    const auto [existing, inserted] = User(data, pass).AccessTable.FindOrEmplace(access);
    if (inserted) return;
    if (existing->State != access.State) {
        if (!access.Texture && !existing->Write && !access.Write &&
            IsReadOnlyBufferState(gpu::BufferStates{existing->State}) && IsReadOnlyBufferState(gpu::BufferStates{access.State})) {
            access.State |= existing->State;
            if (ValidAccess(data, pass, access)) {
                existing->State = access.State;
                return;
            }
        }
        Fail(data, "conflicting whole-resource states in a pass");
        return;
    }
    existing->Read |= access.Read;
    existing->Write |= access.Write;
    existing->Preserve |= access.Preserve;
}

void BufferAccess(RDGData& data, uint32_t pass, RDGBufferHandle buffer, gpu::BufferStates state, bool read, bool write) {
    AddAccess(data, pass, {false, buffer.Index, state.value(), read, write, write});
}

void TextureAccess(RDGData& data, uint32_t pass, RDGTextureHandle texture, gpu::TextureState state, bool read, bool write) {
    AddAccess(data, pass, {true, texture.Index, static_cast<uint32_t>(state), read, write, write});
}

uint32_t AddNode(RDGData& data, RDGPassKind kind, uint32_t payload) {
    if (data.Passes.size() >= std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("RDG: too many passes");
    const auto index = static_cast<uint32_t>(data.Passes.size());
    data.Passes.push_back({kind, payload, std::pmr::vector<uint32_t>{&data.MemoryResource}, std::pmr::vector<uint32_t>{&data.MemoryResource}});
    return index;
}

uint32_t AddPass(RDGData& data, std::string_view name, RDGPassKind kind) {
    RequireOpen(data);
    if (data.Passes.size() >= std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("RDG: too many passes");
    const auto index = static_cast<uint32_t>(data.Passes.size());
    const auto payload = static_cast<uint32_t>(data.UserPasses.size());
    std::pmr::string passName{name, &data.MemoryResource};
    data.UserPasses.push_back(RDGUserPass{.Name = std::move(passName),
                                          .AccessTable = RDGAccessTable{&data.MemoryResource},
                                          .MeshPasses = std::pmr::vector<MeshPassId>{&data.MemoryResource},
                                          .Colors = std::pmr::vector<RDGColorAttachment>{&data.MemoryResource}});
    AddNode(data, kind, payload);
    return index;
}

bool ValidLoad(gpu::LoadAction action) noexcept {
    return action == gpu::LoadAction::Clear || action == gpu::LoadAction::Load || action == gpu::LoadAction::DontCare;
}

bool ValidStore(gpu::StoreAction action) noexcept {
    return action == gpu::StoreAction::Store || action == gpu::StoreAction::Discard;
}

void ValidateAttachments(RDGData& data, RDGPass& pass) {
    if (pass.Kind != RDGPassKind::Raster) return;
    if (User(data, pass).Colors.empty() && !User(data, pass).Depth) Fail(data, "raster pass requires an attachment");
    std::optional<array<uint32_t, 4>> shape;
    auto& raster = User(data, pass);
    const auto validate = [&](RDGTextureHandle handle, Nullable<gpu::TextureView*> view, gpu::SubresourceRange& range, gpu::TextureViewUsage usage) {
        const auto& resource = data.Textures[handle.Index];
        const auto& desc = resource.Desc;
        if (view) {
            const auto v = view->GetDesc();
            if (!resource.Imported || v.Target != resource.Imported.Get() || !view->IsValid() || v.Usage != usage ||
                gpu::IsDepthStencilFormat(v.Format) != gpu::IsDepthStencilFormat(desc.Format)) {
                Fail(data, "explicit attachment view does not match the imported resource or usage");
                return;
            }
            range = v.Range;
        }
        const auto normalized = gpu::NormalizeSubresourceRange(desc, range);
        if (!normalized || normalized->MipLevelCount != 1 || normalized->BaseMipLevel >= 32 ||
            normalized->Aspects != gpu::GetTextureFormatAspects(desc.Format)) {
            Fail(data, "invalid attachment subresource range");
            return;
        }
        const auto layers = desc.Dim == gpu::TextureDimension::Dim3D ? std::max(1u, desc.DepthOrArraySize >> normalized->BaseMipLevel) : normalized->ArrayLayerCount;
        const array current{std::max(1u, desc.Width >> normalized->BaseMipLevel), std::max(1u, desc.Height >> normalized->BaseMipLevel), layers, desc.SampleCount};
        if (shape && *shape != current) Fail(data, "attachment dimensions, layers and samples must match");
        shape = current;
        range = *normalized;
        const auto area = raster.RenderArea.value_or(Rect{0, 0, current[0], current[1]});
        if (area.X < 0 || area.Y < 0 || !area.Width || !area.Height ||
            uint64_t(area.X) + area.Width > current[0] || uint64_t(area.Y) + area.Height > current[1]) {
            Fail(data, "render area exceeds attachment subresource");
            return;
        }
        const auto totalLayers = desc.Dim == gpu::TextureDimension::Dim3D ? 1u : desc.DepthOrArraySize;
        if (area.Width != current[0] || area.Height != current[1] || range.MipLevelCount != desc.MipLevels ||
            range.BaseArrayLayer || range.ArrayLayerCount != totalLayers) {
            if (const auto access = raster.AccessTable.Find(true, handle.Index)) {
                access->Preserve = true;
            }
        }
    };
    for (uint32_t slot = 0; slot < User(data, pass).Colors.size(); ++slot) {
        auto& color = raster.Colors[slot];
        if (color.Slot != slot) Fail(data, "color attachment slots must be contiguous from zero");
        if (!ValidLoad(color.Load) || !ValidStore(color.Store)) Fail(data, "invalid color attachment load/store");
        validate(color.Texture, color.View, color.Range, gpu::TextureViewUsage::RenderTarget);
    }
    if (User(data, pass).Depth) {
        auto& depth = *raster.Depth;
        const auto usage = DepthUsage(depth.State);
        const bool stencil = gpu::GetTextureFormatAspects(data.Textures[depth.Texture.Index].Desc.Format).HasFlag(gpu::TextureAspect::Stencil);
        if (!ValidLoad(depth.DepthLoad) || !ValidStore(depth.DepthStore) || !ValidLoad(depth.StencilLoad) || !ValidStore(depth.StencilStore)) Fail(data, "invalid depth/stencil load/store");
        if ((gpu::IsDepthReadOnly(usage) && (depth.DepthLoad != gpu::LoadAction::Load || depth.DepthStore != gpu::StoreAction::Store)) ||
            (stencil && gpu::IsStencilReadOnly(usage) && (depth.StencilLoad != gpu::LoadAction::Load || depth.StencilStore != gpu::StoreAction::Store))) Fail(data, "read-only depth/stencil aspects require Load and Store");
        validate(depth.Texture, depth.View, depth.Range, usage);
    }
    if (shape) {
        User(data, pass).Width = (*shape)[0];
        User(data, pass).Height = (*shape)[1];
        User(data, pass).Layers = (*shape)[2];
    }
}

bool ValidAccess(const RDGData& data, const RDGPass& pass, const RDGAccess& access) noexcept {
    if (access.Texture) {
        const auto state = static_cast<gpu::TextureState>(access.State);
        if (!ValidTextureState(data.Textures[access.Index].Desc, state)) return false;
        if (pass.Kind == RDGPassKind::Compute && state != gpu::TextureState::ShaderRead && state != gpu::TextureState::UnorderedAccess) return false;
        if (pass.Kind == RDGPassKind::Raster && state != gpu::TextureState::ShaderRead && state != gpu::TextureState::UnorderedAccess) {
            const bool color = state == gpu::TextureState::RenderTarget && std::any_of(User(data, pass).Colors.begin(), User(data, pass).Colors.end(), [&](const auto& attachment) { return attachment.Texture.Index == access.Index; });
            const bool depth = User(data, pass).Depth && User(data, pass).Depth->Texture.Index == access.Index && User(data, pass).Depth->State == state;
            if (!color && !depth) return false;
        }
        if (access.Write) {
            return state == gpu::TextureState::UnorderedAccess || state == gpu::TextureState::CopyDestination ||
                   state == gpu::TextureState::RenderTarget || DepthUsage(state) == gpu::TextureViewUsage::DepthWrite ||
                   state == gpu::TextureState::DepthReadStencilWrite || state == gpu::TextureState::DepthWriteStencilRead;
        }
        return state == gpu::TextureState::ShaderRead || state == gpu::TextureState::CopySource ||
               state == gpu::TextureState::DepthRead || state == gpu::TextureState::UnorderedAccess;
    }
    const auto state = gpu::BufferStates{access.State};
    if (!ValidBufferState(data.Buffers[access.Index].Desc, state)) return false;
    if (pass.Kind == RDGPassKind::Raster || pass.Kind == RDGPassKind::Compute) {
        if (state == gpu::BufferState::UnorderedAccess) return true;
        auto allowed = gpu::BufferState::CBuffer | gpu::BufferState::ShaderRead | gpu::BufferState::Indirect;
        if (pass.Kind == RDGPassKind::Raster) allowed |= gpu::BufferState::Vertex | gpu::BufferState::Index;
        return !access.Write && state && !(state & ~allowed);
    }
    if (access.Write) return state == gpu::BufferState::CopyDestination ||
                             (pass.Kind == RDGPassKind::HostWrite && state == gpu::BufferState::HostWrite);
    return state == gpu::BufferState::CopySource;
}

RDGTextureViewDescriptor DefaultViewRequest(gpu::TextureViewUsage usage) noexcept {
    const uint32_t mipCount = usage == gpu::TextureViewUsage::Resource ? gpu::SubresourceRange::All : 1u;
    return {.Range = {0, gpu::SubresourceRange::All, 0, mipCount}, .Usage = usage};
}

gpu::Texture& ResolveTexture(const RDGExecution& execution, RDGTextureHandle handle) noexcept {
    if (handle.Index >= execution.Data.Textures.size() || !execution.States.GetTexture(handle.Index).Resource) RADRAY_ABORT("RDG: resolving undeclared texture");
    return *execution.States.GetTexture(handle.Index).Resource;
}

RDGBufferSlice ResolveBuffer(const RDGExecution& execution, RDGBufferHandle handle) noexcept {
    if (handle.Index >= execution.Slices.size()) RADRAY_ABORT("RDG: invalid buffer handle");
    return execution.Slices[handle.Index];
}

Nullable<gpu::TextureView*> ResolveTextureView(RDGExecution& execution, RDGTextureHandle handle, const RDGTextureViewDescriptor& request, bool create) noexcept {
    const auto usage = request.Usage;
    if (handle.Index >= execution.Data.Textures.size() || !execution.States.GetTexture(handle.Index).Resource) return nullptr;
    if (execution.Pass) {
        Nullable<const RDGAccess*> found{nullptr};
        if (IsUpload(*execution.Pass)) {
            const auto& access = Upload(execution.Data, *execution.Pass).Access;
            if (access.Texture && access.Index == handle.Index) found = &access;
        } else {
            found = User(execution.Data, *execution.Pass).AccessTable.Find(true, handle.Index);
        }
        if (!found) return nullptr;
        const auto state = static_cast<gpu::TextureState>(found->State);
        const bool compatible = (usage == gpu::TextureViewUsage::Resource && state == gpu::TextureState::ShaderRead) ||
                                (usage == gpu::TextureViewUsage::UnorderedAccess && state == gpu::TextureState::UnorderedAccess) ||
                                (usage == gpu::TextureViewUsage::RenderTarget && state == gpu::TextureState::RenderTarget) ||
                                (usage != gpu::TextureViewUsage::UNKNOWN && usage == DepthUsage(state));
        if (!compatible) return nullptr;
    }
    auto& texture = ResolveTexture(execution, handle);
    const auto desc = texture.GetDesc();
    auto dim = request.Dim == gpu::TextureDimension::UNKNOWN ? desc.Dim : request.Dim;
    if (request.Dim == gpu::TextureDimension::UNKNOWN && usage != gpu::TextureViewUsage::Resource &&
        (dim == gpu::TextureDimension::Cube || dim == gpu::TextureDimension::CubeArray)) dim = gpu::TextureDimension::Dim2DArray;
    auto range = request.Range;
    if (!range.Aspects && usage == gpu::TextureViewUsage::Resource && gpu::IsDepthStencilFormat(desc.Format)) range.Aspects = gpu::TextureAspect::Depth;
    if (range.ArrayLayerCount == gpu::SubresourceRange::All && (dim == gpu::TextureDimension::Dim1D || dim == gpu::TextureDimension::Dim2D || dim == gpu::TextureDimension::Dim3D)) range.ArrayLayerCount = 1;
    const auto normalized = gpu::NormalizeSubresourceRange(desc, range);
    if (!normalized) return nullptr;
    const gpu::TextureViewDescriptor descriptor{&texture, dim, request.Format == gpu::TextureFormat::UNKNOWN ? desc.Format : request.Format, *normalized, usage};
    for (const auto& entry : execution.Views) {
        if (entry.Texture == handle && SameDescriptor(entry.Descriptor, descriptor)) return entry.View;
    }
    if (!create) return nullptr;
    const auto& resource = execution.Data.Textures[handle.Index];
    Nullable<gpu::TextureView*> view;
    if (resource.Persistent) {
        view = resource.Persistent->GetOrCreateView(execution.Slot.GetDevice(), {dim, descriptor.Format, *normalized, usage});
    } else if (resource.Imported) {
        auto owner = execution.Slot.GetDevice().CreateTextureView(descriptor);
        if (!owner) return nullptr;
        view = owner.Get();
        execution.ViewOwners.push_back(owner.Release());
    } else {
        view = execution.Pool.AcquireTextureView(descriptor);
    }
    if (view) execution.Views.push_back({handle, descriptor, view.Get()});
    return view;
}

Nullable<gpu::TextureView*> ResolveTextureView(RDGExecution& execution, RDGTextureHandle handle, gpu::TextureViewUsage usage) noexcept {
    return ResolveTextureView(execution, handle, DefaultViewRequest(usage), false);
}

template <class Attachment>
Nullable<gpu::TextureView*> ResolveAttachmentView(RDGExecution& execution, const Attachment& attachment, gpu::TextureViewUsage usage, bool create) noexcept {
    if (attachment.View) return attachment.View;
    return ResolveTextureView(execution, attachment.Texture, {.Range = attachment.Range, .Usage = usage}, create);
}

struct GroupAccess {
    size_t Group{std::numeric_limits<size_t>::max()};
    RDGAccess Access{};
};

size_t AccessIndex(const RDGData& data, const RDGAccess& access) noexcept {
    return access.Texture ? access.Index : data.Textures.size() + access.Index;
}

bool CanMerge(const RDGData& data, const RDGPass& candidate, std::span<const GroupAccess> accesses) noexcept {
    if (data.Groups.empty()) return false;
    const auto& group = data.Groups.back();
    if (group.Kind != candidate.Kind) return false;
    if (group.Kind == RDGPassKind::Upload) return true;
    if (group.Kind != RDGPassKind::Raster && group.Kind != RDGPassKind::Compute) return false;
    const auto& previous = data.Passes[group.Passes.back()];
    if (group.Kind == RDGPassKind::Raster) {
        const auto& left = User(data, previous);
        const auto& right = User(data, candidate);
        const auto aRect = left.RenderArea.value_or(Rect{0, 0, left.Width, left.Height});
        const auto bRect = right.RenderArea.value_or(Rect{0, 0, right.Width, right.Height});
        if (aRect.X != bRect.X || aRect.Y != bRect.Y || aRect.Width != bRect.Width || aRect.Height != bRect.Height) return false;
        if (User(data, previous).Colors.size() != User(data, candidate).Colors.size() || bool(User(data, previous).Depth) != bool(User(data, candidate).Depth)) return false;
        for (size_t i = 0; i < User(data, previous).Colors.size(); ++i) {
            const auto& a = User(data, previous).Colors[i];
            const auto& b = User(data, candidate).Colors[i];
            if (a.Slot != b.Slot || a.Texture != b.Texture || a.View != b.View || a.Range != b.Range || a.Store != gpu::StoreAction::Store || b.Load != gpu::LoadAction::Load) return false;
        }
        if (User(data, previous).Depth) {
            const auto& a = *User(data, previous).Depth;
            const auto& b = *User(data, candidate).Depth;
            if (a.Texture != b.Texture || a.View != b.View || a.Range != b.Range || a.State != b.State || a.DepthStore != gpu::StoreAction::Store || b.DepthLoad != gpu::LoadAction::Load) return false;
            const bool stencil = gpu::GetTextureFormatAspects(data.Textures[a.Texture.Index].Desc.Format).HasFlag(gpu::TextureAspect::Stencil);
            if (stencil && (a.StencilStore != gpu::StoreAction::Store || b.StencilLoad != gpu::LoadAction::Load)) return false;
        }
    }
    for (const auto& access : Accesses(data, candidate)) {
        const auto& entry = accesses[AccessIndex(data, access)];
        if (entry.Group != data.Groups.size() - 1) continue;
        const auto& old = entry.Access;
        if (old.State != access.State) return false;
        if (candidate.Kind == RDGPassKind::Raster && access.Texture) {
            if (User(data, candidate).Depth && User(data, candidate).Depth->Texture.Index == access.Index && access.State == static_cast<uint32_t>(User(data, candidate).Depth->State)) continue;
            if (access.State == static_cast<uint32_t>(gpu::TextureState::RenderTarget) &&
                std::any_of(User(data, candidate).Colors.begin(), User(data, candidate).Colors.end(), [&](const auto& color) { return color.Texture.Index == access.Index; })) continue;
        }
        if (old.Write || access.Write) return false;
    }
    return true;
}

bool SchedulePasses(RDGData& data) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile.Schedule");
    const auto count = data.Passes.size();
    std::pmr::vector<std::pmr::vector<uint32_t>> dependents(count, &data.ScratchResource);
    std::pmr::vector<uint32_t> inDegree(count, 0, &data.ScratchResource);
    std::pmr::vector<uint32_t> seen(count, std::numeric_limits<uint32_t>::max(), &data.ScratchResource);
    std::pmr::set<uint32_t> readyHost{&data.ScratchResource};
    std::pmr::set<uint32_t> readyUpload{&data.ScratchResource};
    std::pmr::set<uint32_t> readyOther{&data.ScratchResource};
    const auto enqueue = [&](uint32_t index) {
        const auto kind = data.Passes[index].Kind;
        (kind == RDGPassKind::HostWrite ? readyHost : kind == RDGPassKind::Upload || kind == RDGPassKind::Copy ? readyUpload
                                                                                                               : readyOther)
            .insert(index);
    };
    size_t aliveCount = 0;
    for (uint32_t index = 0; index < count; ++index) {
        const auto& pass = data.Passes[index];
        if (!pass.Alive) continue;
        ++aliveCount;
        const auto add = [&](const auto& dependencies) {
            for (const auto dep : dependencies) {
                if (!data.Passes[dep].Alive || seen[dep] == index) continue;
                seen[dep] = index;
                dependents[dep].push_back(index);
                ++inDegree[index];
            }
        };
        add(pass.DataDependencies);
        add(pass.OrderDependencies);
        if (!inDegree[index]) enqueue(index);
    }
    std::pmr::vector<GroupAccess> accesses(data.Textures.size() + data.Buffers.size(), &data.ScratchResource);
    data.ScheduledPasses.reserve(aliveCount);
    while (!readyHost.empty() || !readyUpload.empty() || !readyOther.empty()) {
        uint32_t index;
        if (!readyHost.empty()) {
            index = *readyHost.begin();
            readyHost.erase(readyHost.begin());
        } else {
            auto merge = readyOther.end();
            if (!data.Groups.empty() && data.Groups.back().Kind != RDGPassKind::Upload && data.Groups.back().Kind != RDGPassKind::Copy) {
                merge = std::find_if(readyOther.begin(), readyOther.end(), [&](uint32_t value) { return CanMerge(data, data.Passes[value], accesses); });
            }
            if (merge != readyOther.end()) {
                index = *merge;
                readyOther.erase(merge);
            } else if (!readyUpload.empty()) {
                index = *readyUpload.begin();
                readyUpload.erase(readyUpload.begin());
            } else {
                index = *readyOther.begin();
                readyOther.erase(readyOther.begin());
            }
        }
        const auto& pass = data.Passes[index];
        data.ScheduledPasses.push_back(index);
        if (pass.Kind != RDGPassKind::HostWrite) {
            if (!CanMerge(data, pass, accesses)) data.Groups.push_back({pass.Kind,
                                                                        std::pmr::vector<uint32_t>{&data.MemoryResource},
                                                                        std::pmr::vector<gpu::ResourceBarrierDescriptor>{&data.MemoryResource},
                                                                        std::pmr::vector<gpu::RenderPassColorAttachment>{&data.MemoryResource}});
            data.Groups.back().Passes.push_back(index);
            for (const auto& access : Accesses(data, pass)) {
                auto& entry = accesses[AccessIndex(data, access)];
                if (entry.Group != data.Groups.size() - 1)
                    entry = {data.Groups.size() - 1, access};
                else
                    entry.Access.Write |= access.Write;
            }
        }
        for (const auto next : dependents[index]) {
            if (--inDegree[next] == 0) enqueue(next);
        }
    }
    if (data.ScheduledPasses.size() != aliveCount) Fail(data, "cannot schedule all live passes");
    return !data.Failed;
}

std::optional<uint64_t> AlignBytes(uint64_t value, uint64_t alignment) noexcept {
    alignment = std::max(uint64_t{1}, alignment);
    const auto padding = (alignment - value % alignment) % alignment;
    if (padding > std::numeric_limits<uint64_t>::max() - value) return std::nullopt;
    return value + padding;
}

std::optional<uint64_t> MergeAlignment(uint64_t a, uint64_t b) noexcept {
    a = std::max(uint64_t{1}, a);
    b = std::max(uint64_t{1}, b);
    const auto factor = a / std::gcd(a, b);
    if (factor > std::numeric_limits<uint64_t>::max() / b) return std::nullopt;
    return factor * b;
}

gpu::BufferStates BufferReads(gpu::BufferUses usage) noexcept {
    gpu::BufferStates result{};
    if (usage.HasFlag(gpu::BufferUse::CopySource)) result |= gpu::BufferState::CopySource;
    if (usage.HasFlag(gpu::BufferUse::Vertex)) result |= gpu::BufferState::Vertex;
    if (usage.HasFlag(gpu::BufferUse::Index)) result |= gpu::BufferState::Index;
    if (usage.HasFlag(gpu::BufferUse::CBuffer)) result |= gpu::BufferState::CBuffer;
    if (usage.HasFlag(gpu::BufferUse::Resource)) result |= gpu::BufferState::ShaderRead;
    if (usage.HasFlag(gpu::BufferUse::Indirect)) result |= gpu::BufferState::Indirect;
    return result;
}

bool ApplyDeviceConstraints(RDGDeviceConstraints& graph, const RDGDeviceConstraints& device) noexcept {
    for (const auto field : {&RDGDeviceConstraints::BufferCopyAlignment, &RDGDeviceConstraints::TexturePlacementAlignment,
                             &RDGDeviceConstraints::TexturePitchAlignment, &RDGDeviceConstraints::CBufferSizeAlignment,
                             &RDGDeviceConstraints::CBufferOffsetAlignment, &RDGDeviceConstraints::StorageOffsetAlignment,
                             &RDGDeviceConstraints::TexelOffsetAlignment}) {
        if (graph.*field == device.*field) continue;
        const auto alignment = MergeAlignment(graph.*field, device.*field);
        if (!alignment) return false;
        graph.*field = *alignment;
    }
    for (const auto field : {&RDGDeviceConstraints::MaxBufferSize, &RDGDeviceConstraints::MaxCBufferRange, &RDGDeviceConstraints::MaxStorageRange}) {
        const auto maximum = device.*field;
        if (maximum && (!(graph.*field) || maximum < graph.*field)) graph.*field = maximum;
    }
    return true;
}

std::optional<UploadAllocation> AllocateStagingBuffer(UploadArena& arena, const RDGDeviceConstraints& c, uint64_t size, uint64_t alignment) {
    const auto merged = MergeAlignment(alignment, c.BufferCopyAlignment);
    if (!merged || (c.MaxBufferSize && size > c.MaxBufferSize)) return std::nullopt;
    return arena.AllocateStaging(size, *merged);
}

std::optional<UploadAllocation> AllocateBuffer(UploadArena& arena, const RDGDeviceConstraints& c, uint64_t size,
                                               gpu::BufferUses usage, uint64_t alignment, gpu::BufferStates reads) {
    const auto merge = [&](uint64_t value) {
        const auto result = MergeAlignment(alignment, value);
        if (!result) return false;
        alignment = *result;
        return true;
    };
    if (!merge(c.BufferCopyAlignment)) return std::nullopt;
    if (usage.HasFlag(gpu::BufferUse::Index) && !merge(4)) return std::nullopt;
    if (usage.HasFlag(gpu::BufferUse::Indirect) && !merge(4)) return std::nullopt;
    if (usage.HasFlag(gpu::BufferUse::CBuffer) && !merge(c.CBufferOffsetAlignment)) return std::nullopt;
    if (usage.HasFlag(gpu::BufferUse::Resource) && (!merge(c.StorageOffsetAlignment) || !merge(c.TexelOffsetAlignment))) return std::nullopt;
    if ((c.MaxBufferSize && size > c.MaxBufferSize) ||
        (usage.HasFlag(gpu::BufferUse::CBuffer) && c.MaxCBufferRange && size > c.MaxCBufferRange) ||
        (usage.HasFlag(gpu::BufferUse::Resource) && c.MaxStorageRange && size > c.MaxStorageRange)) return std::nullopt;
    const auto reserved = AlignBytes(size, usage.HasFlag(gpu::BufferUse::CBuffer) ? c.CBufferSizeAlignment : 1);
    if (!reserved) return std::nullopt;
    return arena.Allocate(size, alignment, *reserved, reads);
}

}  // namespace

namespace detail {

namespace {

uint32_t HashRDGAccessKey(bool texture, uint32_t index) noexcept {
    uint64_t key = (uint64_t{index} << 1) | uint64_t{texture ? 1u : 0u};
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9ULL;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebULL;
    key ^= key >> 31;
    return static_cast<uint32_t>(key);
}

}  // namespace

RDGAccessTable::RDGAccessTable(std::pmr::memory_resource* memory) noexcept
    : Accesses(memory), AccessBuckets(memory) {}

Nullable<RDGAccess*> RDGAccessTable::Find(bool texture, uint32_t index) noexcept {
    if (AccessBuckets.empty()) {
        for (auto& entry : Accesses) {
            if (entry.Texture == texture && entry.Index == index) return &entry;
        }
        return nullptr;
    }
    const size_t mask = AccessBuckets.size() - 1;
    size_t bucket = HashRDGAccessKey(texture, index) & mask;
    while (true) {
        const uint32_t slot = AccessBuckets[bucket];
        if (slot == kRDGInvalidAccessSlot) return nullptr;
        auto& entry = Accesses[slot];
        if (entry.Texture == texture && entry.Index == index) return &entry;
        bucket = (bucket + 1) & mask;
    }
}

Nullable<const RDGAccess*> RDGAccessTable::Find(bool texture, uint32_t index) const noexcept {
    if (AccessBuckets.empty()) {
        for (const auto& entry : Accesses) {
            if (entry.Texture == texture && entry.Index == index) return &entry;
        }
        return nullptr;
    }
    const size_t mask = AccessBuckets.size() - 1;
    size_t bucket = HashRDGAccessKey(texture, index) & mask;
    while (true) {
        const uint32_t slot = AccessBuckets[bucket];
        if (slot == kRDGInvalidAccessSlot) return nullptr;
        const auto& entry = Accesses[slot];
        if (entry.Texture == texture && entry.Index == index) return &entry;
        bucket = (bucket + 1) & mask;
    }
}

std::pair<RDGAccess*, bool> RDGAccessTable::FindOrEmplace(const RDGAccess& access) {
    if (AccessBuckets.empty()) {
        for (auto& entry : Accesses) {
            if (entry.Texture == access.Texture && entry.Index == access.Index) return {&entry, false};
        }
        Accesses.push_back(access);
        if (Accesses.size() > kRDGLinearAccessThreshold) Rehash(64);
        return {&Accesses.back(), true};
    }
    const size_t mask = AccessBuckets.size() - 1;
    size_t bucket = HashRDGAccessKey(access.Texture, access.Index) & mask;
    while (true) {
        const uint32_t slot = AccessBuckets[bucket];
        if (slot == kRDGInvalidAccessSlot) break;
        auto& entry = Accesses[slot];
        if (entry.Texture == access.Texture && entry.Index == access.Index) return {&entry, false};
        bucket = (bucket + 1) & mask;
    }
    const uint32_t newSlot = static_cast<uint32_t>(Accesses.size());
    Accesses.push_back(access);
    if (Accesses.size() > AccessBuckets.size() / 2) {
        Rehash(AccessBuckets.size() * 2);
    } else {
        AccessBuckets[bucket] = newSlot;
    }
    return {&Accesses.back(), true};
}

void RDGAccessTable::Rehash(size_t bucketCount) {
    RADRAY_ASSERT(bucketCount >= 64 && (bucketCount & (bucketCount - 1)) == 0);
    AccessBuckets.assign(bucketCount, kRDGInvalidAccessSlot);
    const size_t mask = bucketCount - 1;
    for (uint32_t slot = 0; slot < static_cast<uint32_t>(Accesses.size()); ++slot) {
        const auto& entry = Accesses[slot];
        size_t bucket = HashRDGAccessKey(entry.Texture, entry.Index) & mask;
        while (AccessBuckets[bucket] != kRDGInvalidAccessSlot) {
            bucket = (bucket + 1) & mask;
        }
        AccessBuckets[bucket] = slot;
    }
}

ErasedPassCallback::ErasedPassCallback(void* instance, void (*destroy)(void*) noexcept) noexcept
    : _instance(instance), _destroy(destroy) {}

ErasedPassCallback::ErasedPassCallback(ErasedPassCallback&& other) noexcept
    : _instance(std::exchange(other._instance, nullptr)), _destroy(std::exchange(other._destroy, nullptr)) {}

ErasedPassCallback& ErasedPassCallback::operator=(ErasedPassCallback&& other) noexcept {
    if (this != &other) {
        Reset();
        _instance = std::exchange(other._instance, nullptr);
        _destroy = std::exchange(other._destroy, nullptr);
    }
    return *this;
}

ErasedPassCallback::~ErasedPassCallback() noexcept { Reset(); }

void* ErasedPassCallback::Get() const noexcept { return _instance; }

void ErasedPassCallback::Reset() noexcept {
    if (_instance && _destroy) _destroy(_instance);
    _instance = nullptr;
    _destroy = nullptr;
}

RDGData::~RDGData() noexcept {
    for (auto& pass : UserPasses) pass.Callback.Reset();
}

RDGDeviceConstraints QueryRDGDeviceConstraints(const gpu::Device& device) noexcept {
    const auto& capabilities = device.GetCapabilities();
    const auto& detail = capabilities.Detail;
    const auto& limits = capabilities.Limits;
    return {detail.BufferCopyOffsetAlignment, detail.TextureDataPlacementAlignment, detail.TextureDataPitchAlignment,
            detail.CBufferAlignment, limits.CBufferOffsetAlignment, limits.StorageBufferOffsetAlignment,
            limits.TexelBufferOffsetAlignment, limits.MaxBufferSize, limits.MaxUniformBufferRange, limits.MaxStorageBufferRange};
}

void RDGStateTracker::Initialize() {
    _textures.resize(_data.Textures.size());
    _buffers.resize(_data.Buffers.size());
}

void RDGStateTracker::BindTexture(uint32_t index, gpu::Texture* texture, gpu::TextureState initialState) noexcept {
    const auto& ownership = _data.Textures[index].Ownership;
    _textures[index] = {texture, initialState, IsWriteState(initialState), ownership.Acquire, ownership.Release};
}

void RDGStateTracker::BindBuffer(uint32_t index, gpu::Buffer* buffer, gpu::BufferStates initialState) noexcept {
    const auto& ownership = _data.Buffers[index].Ownership;
    _buffers[index] = {buffer, initialState, IsWriteState(initialState), ownership.Acquire, ownership.Release};
}

void RDGStateTracker::TransitionTexture(uint32_t index, gpu::TextureState next, bool write, std::pmr::vector<gpu::ResourceBarrierDescriptor>& barriers) noexcept {
    auto& current = _textures[index];
    const auto before = current.CurrentState;
    if (current.PendingExternalAcquire) {
        barriers.emplace_back(gpu::BarrierTextureDescriptor{.Target = current.Resource.Get(), .Before = before, .After = next, .ExternalTransfer = gpu::ExternalOwnershipTransfer::Acquire});
        current.PendingExternalAcquire = false;
    } else if (before == gpu::TextureState::UnorderedAccess && next == before && (current.PendingWrite || write)) {
        barriers.emplace_back(gpu::BarrierUavDescriptor{current.Resource.Get()});
    } else if (before != next || current.PendingWrite) {
        barriers.emplace_back(gpu::BarrierTextureDescriptor{current.Resource.Get(), before, next});
    }
    current.CurrentState = next;
    current.PendingWrite = write;
}

void RDGStateTracker::TransitionBuffer(uint32_t index, gpu::BufferStates next, bool write, std::pmr::vector<gpu::ResourceBarrierDescriptor>& barriers) noexcept {
    auto& current = _buffers[index];
    if (!current.Resource) return;
    const auto& logical = _data.Buffers[index];
    if (!logical.Imported && logical.Desc.Memory == gpu::MemoryType::Upload) return;
    const auto before = current.CurrentState;
    if (current.PendingExternalAcquire) {
        barriers.emplace_back(gpu::BarrierBufferDescriptor{.Target = current.Resource.Get(), .Before = before, .After = next, .ExternalTransfer = gpu::ExternalOwnershipTransfer::Acquire});
        current.PendingExternalAcquire = false;
    } else if (before == gpu::BufferState::UnorderedAccess && next == before && (current.PendingWrite || write)) {
        barriers.emplace_back(gpu::BarrierUavDescriptor{current.Resource.Get()});
    } else if (before != next || current.PendingWrite) {
        barriers.emplace_back(gpu::BarrierBufferDescriptor{current.Resource.Get(), before, next});
    }
    current.CurrentState = next;
    current.PendingWrite = write;
}

void RDGStateTracker::PrepareHostWrite(uint32_t index) noexcept {
    TransitionBuffer(index, gpu::BufferState::HostWrite, false, _beginBarriers);
}

void RDGStateTracker::PrepareBarriers(UploadArena& uploads, std::span<RDGPreparedUpload> preparedUploads, std::span<const uint32_t> uploadCounts) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.PlanBarriers");
    uploads.AppendReadBarriers(_beginBarriers);
    std::pmr::vector<size_t> seen(_data.Textures.size() + _data.Buffers.size(), std::numeric_limits<size_t>::max(), &_data.ScratchResource);
    for (size_t groupIndex = 0; groupIndex < _data.Groups.size(); ++groupIndex) {
        auto& group = _data.Groups[groupIndex];
        // 上传区间只在当前批次内优化；区间覆盖保守地包含纹理行间空隙。
        using RangeMap = std::pmr::map<uint64_t, uint64_t>;
        std::pmr::map<std::pair<uint32_t, uint64_t>, RangeMap> writtenRanges{&_data.ScratchResource};
        for (const auto passIndex : group.Passes) {
            auto& pass = _data.Passes[passIndex];
            for (const auto& access : Accesses(_data, pass)) {
                group.Uav |= access.Write && access.State == (access.Texture ? static_cast<uint32_t>(gpu::TextureState::UnorderedAccess) : static_cast<uint32_t>(gpu::BufferState::UnorderedAccess));
                auto& stamp = seen[AccessIndex(_data, access)];
                if (group.Kind == RDGPassKind::Upload) {
                    if (!preparedUploads[pass.PreparedUpload].ActualSize) continue;
                    // 仅有一个生产者的资源不需要区间索引。
                    if (uploadCounts[AccessIndex(_data, access)] == 1) {
                        if (access.Texture)
                            TransitionTexture(access.Index, static_cast<gpu::TextureState>(access.State), true, group.Barriers);
                        else
                            TransitionBuffer(access.Index, gpu::BufferStates{access.State}, true, group.Barriers);
                        stamp = groupIndex;
                        continue;
                    }
                    uint64_t begin = Upload(_data, pass).UploadOffset, end = begin + preparedUploads[pass.PreparedUpload].ActualSize, subresource = 0;
                    if (access.Texture) {
                        const auto width = std::max(1u, _data.Textures[access.Index].Desc.Width >> Upload(_data, pass).Region.MipLevel);
                        begin = uint64_t{Upload(_data, pass).Region.Y} * width + Upload(_data, pass).Region.X;
                        end = begin + uint64_t{Upload(_data, pass).Region.Height - 1} * width + Upload(_data, pass).Region.Width;
                        subresource = (uint64_t{Upload(_data, pass).Region.ArrayLayer} << 32) | Upload(_data, pass).Region.MipLevel;
                    }
                    auto& ranges = writtenRanges[{static_cast<uint32_t>(AccessIndex(_data, access)), subresource}];
                    auto next = ranges.lower_bound(begin);
                    bool overlap = next != ranges.end() && next->first < end;
                    if (next != ranges.begin()) overlap |= std::prev(next)->second > begin;
                    auto& barriers = stamp == groupIndex ? preparedUploads[pass.PreparedUpload].Barriers : group.Barriers;
                    if (stamp != groupIndex || overlap) {
                        if (access.Texture)
                            TransitionTexture(access.Index, static_cast<gpu::TextureState>(access.State), true, barriers);
                        else
                            TransitionBuffer(access.Index, gpu::BufferStates{access.State}, true, barriers);
                    }
                    if (overlap) ranges.clear();
                    ranges.emplace(begin, end);
                    stamp = groupIndex;
                } else if (stamp != groupIndex) {
                    stamp = groupIndex;
                    if (access.Texture)
                        TransitionTexture(access.Index, static_cast<gpu::TextureState>(access.State), access.Write, group.Barriers);
                    else
                        TransitionBuffer(access.Index, gpu::BufferStates{access.State}, access.Write, group.Barriers);
                } else if (access.Texture) {
                    _textures[access.Index].PendingWrite |= access.Write;
                } else {
                    _buffers[access.Index].PendingWrite |= access.Write;
                }
            }
        }
    }
    for (uint32_t i = 0; i < _data.Textures.size(); ++i) {
        const auto& resource = _data.Textures[i];
        if (!resource.Alive) continue;
        if (resource.FinalState || _textures[i].PendingExternalAcquire)
            TransitionTexture(i, resource.FinalState.value_or(_textures[i].CurrentState), false, _finalBarriers);
    }
    for (uint32_t i = 0; i < _data.Buffers.size(); ++i) {
        const auto& resource = _data.Buffers[i];
        if (!resource.Alive) continue;
        if (resource.FinalState || _buffers[i].PendingExternalAcquire)
            TransitionBuffer(i, resource.FinalState.value_or(_buffers[i].CurrentState), false, _finalBarriers);
    }
    for (const auto& resource : _textures) {
        if (resource.NeedsExternalRelease) _releaseBarriers.emplace_back(gpu::BarrierTextureDescriptor{.Target = resource.Resource.Get(), .Before = resource.CurrentState, .After = resource.CurrentState, .ExternalTransfer = gpu::ExternalOwnershipTransfer::Release});
    }
    for (const auto& resource : _buffers) {
        if (resource.NeedsExternalRelease) _releaseBarriers.emplace_back(gpu::BarrierBufferDescriptor{.Target = resource.Resource.Get(), .Before = resource.CurrentState, .After = resource.CurrentState, .ExternalTransfer = gpu::ExternalOwnershipTransfer::Release});
    }
}

}  // namespace detail

class RDGTransientResourcePool::Impl {
public:
    struct TextureEntry {
        gpu::TextureDescriptor Desc;
        unique_ptr<gpu::Texture> Resource;
        gpu::TextureState State{gpu::TextureState::Undefined};
        bool Used{true};
        uint32_t UnusedFrames{0};
    };
    struct BufferEntry {
        gpu::BufferDescriptor Desc;
        unique_ptr<gpu::Buffer> Resource;
        gpu::BufferStates State;
        bool Used{true};
        uint32_t UnusedFrames{0};
    };
    struct ViewEntry {
        gpu::TextureViewDescriptor Desc;
        unique_ptr<gpu::TextureView> View;
    };
    explicit Impl(gpu::Device& device) noexcept : Device(device) {}
    gpu::Device& Device;
    vector<TextureEntry> Textures;
    vector<BufferEntry> Buffers;
    vector<ViewEntry> Views;
};

RDGTransientResourcePool::RDGTransientResourcePool(gpu::Device& device) noexcept : _impl(make_unique<Impl>(device)) {}
RDGTransientResourcePool::~RDGTransientResourcePool() noexcept = default;

void RDGTransientResourcePool::BeginFrame() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.BeginFrame");
    RADRAY_PROFILE_PLOT("RadRay.RDG.Pool.Textures", static_cast<int64_t>(_impl->Textures.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.Pool.Buffers", static_cast<int64_t>(_impl->Buffers.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.Pool.Views", static_cast<int64_t>(_impl->Views.size()));
    for (size_t i = 0; i < _impl->Textures.size();) {
        auto& entry = _impl->Textures[i];
        if (entry.Used) {
            entry.Used = false;
            entry.UnusedFrames = 0;
            ++i;
        } else if (++entry.UnusedFrames >= 8) {
            gpu::Texture* target = entry.Resource.get();
            std::erase_if(_impl->Views, [target](const Impl::ViewEntry& view) {
                return view.Desc.Target == target;
            });
            _impl->Textures.erase(_impl->Textures.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    for (size_t i = 0; i < _impl->Buffers.size();) {
        auto& entry = _impl->Buffers[i];
        if (entry.Used) {
            entry.Used = false;
            entry.UnusedFrames = 0;
            ++i;
        } else if (++entry.UnusedFrames >= 8) {
            _impl->Buffers.erase(_impl->Buffers.begin() + static_cast<ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void RDGTransientResourcePool::Clear() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.Clear");
    _impl->Views.clear();
    _impl->Textures.clear();
    _impl->Buffers.clear();
}

RDGTransientTextureAllocation RDGTransientResourcePool::AcquireTexture(const gpu::TextureDescriptor& desc) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.AcquireTexture");
    for (auto& entry : _impl->Textures) {
        if (!entry.Used && SameDescriptor(entry.Desc, desc)) {
            entry.Used = true;
            entry.UnusedFrames = 0;
            return {entry.Resource.get(), entry.State};
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.CreateTexture");
        auto resource = _impl->Device.CreateTexture(desc);
        if (!resource) return {};
        auto& entry = _impl->Textures.emplace_back(Impl::TextureEntry{desc, resource.Release()});
        return {entry.Resource.get(), entry.State};
    }
}

RDGTransientBufferAllocation RDGTransientResourcePool::AcquireBuffer(const gpu::BufferDescriptor& desc) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.AcquireBuffer");
    uint64_t bucketSize = desc.Size;
    if (desc.Size > 0) {
        if (desc.Size <= 4096) {
            bucketSize = ((desc.Size + 255) / 256) * 256;
        } else if (desc.Size <= (uint64_t{1} << 63)) {
            bucketSize = std::bit_ceil(desc.Size);
        } else {
            bucketSize = desc.Size;
        }
        const uint64_t maxBufferSize = _impl->Device.GetCapabilities().Limits.MaxBufferSize;
        if (maxBufferSize != 0 && bucketSize > maxBufferSize) bucketSize = desc.Size;
    }
    Nullable<Impl::BufferEntry*> best{nullptr};
    for (auto& entry : _impl->Buffers) {
        if (!entry.Used && entry.Desc.Memory == desc.Memory && entry.Desc.Usage == desc.Usage &&
            entry.Desc.Hints == desc.Hints && entry.Desc.Size >= desc.Size && entry.Desc.Size <= bucketSize) {
            if (!best || entry.Desc.Size < best->Desc.Size) best = &entry;
        }
    }
    if (best) {
        best->Used = true;
        best->UnusedFrames = 0;
        return {best->Resource.get(), best->State};
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.CreateBuffer");
        auto actualDesc = desc;
        actualDesc.Size = bucketSize;
        auto resource = _impl->Device.CreateBuffer(actualDesc);
        if (!resource && bucketSize > desc.Size) {
            actualDesc.Size = desc.Size;
            resource = _impl->Device.CreateBuffer(actualDesc);
        }
        if (!resource) return {};
        const auto state = actualDesc.Memory == gpu::MemoryType::Upload ? gpu::BufferState::HostWrite : gpu::BufferState::Undefined;
        auto& entry = _impl->Buffers.emplace_back(Impl::BufferEntry{actualDesc, resource.Release(), state});
        return {entry.Resource.get(), entry.State};
    }
}

void RDGTransientResourcePool::CommitTextureState(gpu::Texture* texture, gpu::TextureState state) noexcept {
    for (auto& entry : _impl->Textures) {
        if (entry.Resource.get() == texture && entry.Used) {
            entry.State = state;
            return;
        }
    }
    RADRAY_ABORT("RDG: committing texture not borrowed from this pool");
}

void RDGTransientResourcePool::CommitBufferState(gpu::Buffer* buffer, gpu::BufferStates state) noexcept {
    for (auto& entry : _impl->Buffers) {
        if (entry.Resource.get() == buffer && entry.Used) {
            entry.State = state;
            return;
        }
    }
    RADRAY_ABORT("RDG: committing buffer not borrowed from this pool");
}

Nullable<gpu::TextureView*> RDGTransientResourcePool::AcquireTextureView(const gpu::TextureViewDescriptor& desc) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.AcquireView");
    for (auto& entry : _impl->Views) {
        if (SameDescriptor(entry.Desc, desc)) return entry.View.get();
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.CreateView");
        auto view = _impl->Device.CreateTextureView(desc);
        if (!view) return nullptr;
        return _impl->Views.emplace_back(Impl::ViewEntry{desc, view.Release()}).View.get();
    }
}

RDGBlackboard::RDGBlackboard(FrameSlot& slot) : _slot(slot), _entries(&slot.GetMemoryResource()) {}

RDGBuilder::RDGBuilder(FrameSlot& slot, RDGCompileOptions options) noexcept
    : _slot(slot), _data(*slot.Emplace<detail::RDGData>(slot.GetMemoryResource(), slot.GetScratchMemoryResource())), _blackboard(*slot.Emplace<RDGBlackboard>(slot)) {
    _data.Constraints = detail::QueryRDGDeviceConstraints(slot.GetDevice());
    _data.Options = options;
}

RDGBuilder::RDGBuilder(FrameSlot& slot, RDGDeviceConstraints constraints, RDGCompileOptions options) noexcept
    : RDGBuilder(slot, options) {
    if (!ApplyDeviceConstraints(constraints, _data.Constraints)) Fail(_data, "graph and device alignment requirements overflow");
    _data.Constraints = constraints;
}

RDGBlackboard& RDGBuilder::GetBlackboard() noexcept { return _blackboard; }
bool RDGBuilder::IsDynamicBuffer(RDGBufferHandle buffer) const noexcept {
    return buffer.Index < _data.Buffers.size() && _data.Buffers[buffer.Index].Dynamic;
}

std::optional<gpu::BufferDescriptor> RDGBuilder::GetBufferDescriptor(RDGBufferHandle buffer) const noexcept {
    if (buffer.Index >= _data.Buffers.size()) return std::nullopt;
    return _data.Buffers[buffer.Index].Desc;
}

std::optional<gpu::TextureDescriptor> RDGBuilder::GetTextureDescriptor(RDGTextureHandle texture) const noexcept {
    if (texture.Index >= _data.Textures.size()) return std::nullopt;
    return _data.Textures[texture.Index].Desc;
}

RDGPassHandle RDGBuilder::AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value) {
    RequireOpen(_data);
    const auto desc = GetTextureDescriptor(texture);
    if (!desc || desc->MipLevels != 1) {
        Fail(_data, "color clear requires a single-mip color attachment");
        return {};
    }
    return AddClearColor(texture, value, {0, gpu::SubresourceRange::All, 0, 1});
}

RDGPassHandle RDGBuilder::AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value, gpu::SubresourceRange range, std::optional<Rect> renderArea) {
    RequireOpen(_data);
    const auto desc = GetTextureDescriptor(texture);
    if (!desc || range.MipLevelCount != 1 || !desc->Usage.HasFlag(gpu::TextureUse::RenderTarget) || gpu::IsDepthStencilFormat(desc->Format)) {
        Fail(_data, "color clear requires one mip of a color attachment");
        return {};
    }
    struct Clear {};
    auto pass = AddRasterPass("ClearColor", Clear{}, +[](const Clear&, RDGRasterPassContext&) noexcept { return true; });
    pass.WriteColor(0, texture, gpu::LoadAction::Clear, gpu::StoreAction::Store, value, range);
    if (renderArea) pass.SetRenderArea(*renderArea);
    return pass.GetHandle();
}

RDGPassHandle RDGBuilder::AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value, RDGAttachmentSlice slice, std::optional<Rect> renderArea) {
    return AddClearColor(texture, value, slice.ToSubresourceRange(), renderArea);
}

RDGPassHandle RDGBuilder::AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value, bool clearDepth, bool clearStencil) {
    RequireOpen(_data);
    const auto desc = GetTextureDescriptor(texture);
    if (!desc || desc->MipLevels != 1) {
        Fail(_data, "depth/stencil clear requires a single-mip depth/stencil attachment");
        return {};
    }
    return AddClearDepthStencil(texture, value, {0, gpu::SubresourceRange::All, 0, 1}, clearDepth, clearStencil);
}

RDGPassHandle RDGBuilder::AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value, gpu::SubresourceRange range,
                                               bool clearDepth, bool clearStencil, std::optional<Rect> renderArea) {
    RequireOpen(_data);
    const auto desc = GetTextureDescriptor(texture);
    if (!desc || range.MipLevelCount != 1 || !desc->Usage.HasFlag(gpu::TextureUse::DepthStencilWrite) || !gpu::IsDepthStencilFormat(desc->Format)) {
        Fail(_data, "depth/stencil clear requires one mip of a depth/stencil attachment");
        return {};
    }
    const bool stencil = gpu::GetTextureFormatAspects(desc->Format).HasFlag(gpu::TextureAspect::Stencil);
    if (!clearDepth && !(clearStencil && stencil)) {
        Fail(_data, "no selected depth/stencil aspect exists on the attachment");
        return {};
    }
    struct Clear {};
    auto pass = AddRasterPass("ClearDepthStencil", Clear{}, +[](const Clear&, RDGRasterPassContext&) noexcept { return true; });
    pass.SetDepthStencil(texture, gpu::TextureState::DepthWrite, clearDepth ? gpu::LoadAction::Clear : gpu::LoadAction::Load, gpu::StoreAction::Store,
                         !stencil ? gpu::LoadAction::DontCare : clearStencil ? gpu::LoadAction::Clear : gpu::LoadAction::Load,
                         stencil ? gpu::StoreAction::Store : gpu::StoreAction::Discard, value, range);
    if (renderArea) pass.SetRenderArea(*renderArea);
    return pass.GetHandle();
}

RDGPassHandle RDGBuilder::AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value, RDGAttachmentSlice slice,
                                               bool clearDepth, bool clearStencil, std::optional<Rect> renderArea) {
    return AddClearDepthStencil(texture, value, slice.ToSubresourceRange(), clearDepth, clearStencil, renderArea);
}

RDGTextureHandle RDGBuilder::ImportTexture(gpu::Texture* texture, gpu::TextureState initialState, std::optional<gpu::TextureState> finalState, RDGExternalOwnership ownership) {
    RequireOpen(_data);
    const auto found = _data.TextureImports.find(texture);
    if (found != _data.TextureImports.end()) {
        const auto i = found->second;
        auto& previous = _data.Textures[i];
        if (previous.InitialState != initialState || (previous.FinalState && finalState && previous.FinalState != finalState) || previous.Ownership != ownership) Fail(_data, "inconsistent duplicate texture import");
        if (finalState) previous.FinalState = finalState;
        return {i};
    }
    const auto index = static_cast<uint32_t>(_data.Textures.size());
    _data.Textures.push_back({.Desc = texture->GetDesc(), .Imported = texture, .InitialState = initialState, .FinalState = finalState, .Readers = std::pmr::vector<uint32_t>{&_data.MemoryResource}, .Ownership = ownership});
    _data.TextureImports.emplace(texture, index);
    return {index};
}

RDGBufferHandle RDGBuilder::ImportBuffer(gpu::Buffer* buffer, gpu::BufferStates initialState, std::optional<gpu::BufferStates> finalState, RDGExternalOwnership ownership) {
    RequireOpen(_data);
    const auto found = _data.BufferImports.find(buffer);
    if (found != _data.BufferImports.end()) {
        const auto i = found->second;
        auto& previous = _data.Buffers[i];
        if (previous.InitialState != initialState || (previous.FinalState && finalState && previous.FinalState != finalState) || previous.Ownership != ownership) Fail(_data, "inconsistent duplicate buffer import");
        if (finalState) previous.FinalState = finalState;
        return {i};
    }
    const auto index = static_cast<uint32_t>(_data.Buffers.size());
    _data.Buffers.push_back({.Desc = buffer->GetDesc(), .Imported = buffer, .InitialState = initialState, .FinalState = finalState, .Readers = std::pmr::vector<uint32_t>{&_data.MemoryResource}, .Ownership = ownership});
    _data.BufferImports.emplace(buffer, index);
    return {index};
}

RDGBufferHandle RDGBuilder::ImportPersistentBorrowed(GpuUploadBuffer& buffer) {
    const auto handle = ImportBuffer(buffer.GetBuffer(), gpu::BufferState::HostWrite, gpu::BufferState::HostWrite);
    _data.Buffers[handle.Index].OwnerWrites = true;
    return handle;
}

RDGBufferHandle RDGBuilder::ImportPersistentOwned(const IntrusivePtr<GpuUploadBuffer>& buffer) {
    const auto handle = ImportPersistentBorrowed(*buffer);
    auto& resource = _data.Buffers[handle.Index];
    if (!resource.UploadOwner) resource.UploadOwner = buffer;
    return handle;
}

RDGBufferHandle RDGBuilder::ImportPersistentBorrowed(GpuBuffer& buffer) {
    const auto handle = ImportBuffer(buffer.GetBuffer(), buffer.GetSubmittedState(), gpu::BufferState::Common);
    auto& resource = _data.Buffers[handle.Index];
    if (resource.Persistent && resource.Persistent.Get() != &buffer) Fail(_data, "duplicate buffer wrapper");
    resource.Persistent = &buffer;
    return handle;
}
RDGTextureHandle RDGBuilder::ImportPersistentBorrowed(GpuTexture& texture) {
    const auto finalState = texture.GetDesc().Usage.HasFlag(gpu::TextureUse::Resource) ? std::optional{gpu::TextureState::ShaderRead} : std::nullopt;
    const auto handle = ImportTexture(texture.GetTexture(), texture.GetSubmittedState(), finalState);
    auto& resource = _data.Textures[handle.Index];
    if (resource.Persistent && resource.Persistent.Get() != &texture) Fail(_data, "duplicate texture wrapper");
    resource.Persistent = &texture;
    return handle;
}
RDGBufferHandle RDGBuilder::ImportPersistentOwned(const IntrusivePtr<GpuBuffer>& buffer) {
    const auto handle = ImportPersistentBorrowed(*buffer);
    auto& resource = _data.Buffers[handle.Index];
    if (!resource.Owner) resource.Owner = buffer;
    return handle;
}
RDGTextureHandle RDGBuilder::ImportPersistentOwned(const IntrusivePtr<GpuTexture>& texture) {
    const auto handle = ImportPersistentBorrowed(*texture);
    auto& resource = _data.Textures[handle.Index];
    if (!resource.Owner) resource.Owner = texture;
    return handle;
}

RDGUploadPrepareContext::RDGUploadPrepareContext(FrameSlot& slot, std::span<const MeshPassId> passes) noexcept : _slot(slot), _passes(passes) {}
std::span<const MeshPassId> RDGUploadPrepareContext::GetAliveMeshPasses() const noexcept { return _passes; }
MeshPassFrameDrawList& RDGUploadPrepareContext::GetPassDrawList(MeshPassId pass) noexcept { return _slot.GetPassDrawList(pass); }
uint64_t RDGUploadPrepareContext::GetFrameId() const noexcept { return _slot.GetFrameId(); }
uint32_t RDGUploadPrepareContext::GetSlotIndex() const noexcept { return _slot.GetIndex(); }

RDGUploadSource::GeneratedSource::GeneratedSource(GeneratedSource&& other) noexcept {
    if (other.Move) {
        other.Move(Storage, other.Storage);
        Layout = std::exchange(other.Layout, nullptr);
        Write = std::exchange(other.Write, nullptr);
        Move = std::exchange(other.Move, nullptr);
        Destroy = std::exchange(other.Destroy, nullptr);
    }
}

RDGUploadSource::GeneratedSource& RDGUploadSource::GeneratedSource::operator=(GeneratedSource&& other) noexcept {
    if (this != &other) {
        Reset();
        if (other.Move) {
            other.Move(Storage, other.Storage);
            Layout = std::exchange(other.Layout, nullptr);
            Write = std::exchange(other.Write, nullptr);
            Move = std::exchange(other.Move, nullptr);
            Destroy = std::exchange(other.Destroy, nullptr);
        }
    }
    return *this;
}

RDGUploadSource::GeneratedSource::~GeneratedSource() noexcept { Reset(); }

void RDGUploadSource::GeneratedSource::Reset() noexcept {
    if (Destroy) Destroy(Storage);
    Layout = nullptr;
    Write = nullptr;
    Move = nullptr;
    Destroy = nullptr;
}

RDGUploadSource RDGUploadSource::BorrowedBytes(std::span<const byte> bytes) noexcept {
    RDGUploadSource result;
    result._storage = bytes;
    result._maxSize = bytes.size();
    return result;
}
RDGUploadSource RDGUploadSource::OwnedBytes(vector<byte>&& bytes) noexcept {
    RDGUploadSource result;
    result._maxSize = bytes.size();
    result._storage = std::move(bytes);
    return result;
}
RDGUploadSource RDGUploadSource::SnapshotBytes(std::span<const byte> bytes) { return OwnedBytes(vector<byte>{bytes.begin(), bytes.end()}); }
uint64_t RDGUploadSource::GetMaxSize() const noexcept { return _maxSize; }
uint64_t RDGUploadSource::PrepareLayout(RDGUploadPrepareContext& context) noexcept {
    auto* generated = std::get_if<GeneratedSource>(&_storage);
    return generated && generated->Layout ? generated->Layout.Get()(generated->Storage, context, _maxSize) : _maxSize;
}
bool RDGUploadSource::Write(RDGUploadWriteContext& context, uint64_t sourceRowPitch, uint32_t sourceRowsPerImage) const noexcept {
    if (const auto* generated = std::get_if<GeneratedSource>(&_storage)) return generated->Write.Get()(generated->Storage, context);
    const auto* owned = std::get_if<vector<byte>>(&_storage);
    const auto bytes = owned ? std::span<const byte>{*owned} : std::get<std::span<const byte>>(_storage);
    if (context.RowBytes) {
        const auto pitch = sourceRowPitch ? sourceRowPitch : context.RowBytes;
        const auto sourceRows = sourceRowsPerImage ? sourceRowsPerImage : context.Rows;
        if (!context.RowPitch || context.RowBytes > bytes.size() || context.RowBytes > context.Bytes.size()) return false;
        for (uint32_t slice = 0; slice < context.Depth; ++slice) {
            for (uint32_t row = 0; row < context.Rows; ++row) {
                const uint64_t sourceRow = uint64_t{slice} * sourceRows + row;
                const uint64_t targetRow = uint64_t{slice} * context.Rows + row;
                if (sourceRow > (bytes.size() - context.RowBytes) / pitch || targetRow > (context.Bytes.size() - context.RowBytes) / context.RowPitch) return false;
                std::memcpy(context.Bytes.data() + targetRow * context.RowPitch, bytes.data() + sourceRow * pitch, static_cast<size_t>(context.RowBytes));
            }
        }
    } else {
        if (context.Size > bytes.size() || context.Size > context.Bytes.size()) return false;
        if (context.Size) std::memcpy(context.Bytes.data(), bytes.data(), static_cast<size_t>(context.Size));
    }
    return true;
}
void RDGUploadSource::ReleaseSource() noexcept {
    _storage.emplace<std::span<const byte>>();
}
RDGTextureUploadSource RDGTextureUploadSource::BorrowedBytes(std::span<const byte> bytes, uint64_t pitch, uint32_t rowsPerImage) noexcept { return {RDGUploadSource::BorrowedBytes(bytes), pitch, rowsPerImage}; }
RDGTextureUploadSource RDGTextureUploadSource::OwnedBytes(vector<byte>&& bytes, uint64_t pitch, uint32_t rowsPerImage) noexcept { return {RDGUploadSource::OwnedBytes(std::move(bytes)), pitch, rowsPerImage}; }
RDGTextureUploadSource RDGTextureUploadSource::SnapshotBytes(std::span<const byte> bytes, uint64_t pitch, uint32_t rowsPerImage) { return {RDGUploadSource::SnapshotBytes(bytes), pitch, rowsPerImage}; }

RDGTextureHandle RDGBuilder::CreateTexture(const gpu::TextureDescriptor& desc) {
    RequireOpen(_data);
    const auto index = static_cast<uint32_t>(_data.Textures.size());
    _data.Textures.push_back({.Desc = desc, .Readers = std::pmr::vector<uint32_t>{&_data.MemoryResource}});
    return {index};
}

RDGBufferHandle RDGBuilder::CreateBuffer(const gpu::BufferDescriptor& desc) {
    RequireOpen(_data);
    const auto index = static_cast<uint32_t>(_data.Buffers.size());
    _data.Buffers.push_back({.Desc = desc, .Readers = std::pmr::vector<uint32_t>{&_data.MemoryResource}});
    return {index};
}

RDGBufferHandle RDGBuilder::CreateBuffer(const RDGDynamicBufferDescriptor& desc, RDGUploadSource source) {
    const auto handle = CreateBuffer(gpu::BufferDescriptor{.Size = desc.MaxSize, .Memory = gpu::MemoryType::Upload, .Usage = desc.Usage | gpu::BufferUse::MapWrite});
    auto& resource = _data.Buffers[handle.Index];
    resource.Dynamic = true;
    resource.Alignment = desc.Alignment;
    UploadBuffer(handle, 0, std::move(source));
    return handle;
}

RDGPassHandle RDGBuilder::UploadBuffer(RDGBufferHandle dst, uint64_t offset, RDGUploadSource source) {
    RequireOpen(_data);
    if (dst.Index >= _data.Buffers.size()) {
        Fail(_data, "invalid upload destination");
        return {};
    }
    const auto& resource = _data.Buffers[dst.Index];
    if (resource.OwnerWrites) {
        Fail(_data, "persistent upload buffer is written by its owner");
        return {};
    }
    const auto& desc = resource.Desc;
    if (offset > desc.Size || source.GetMaxSize() > desc.Size - offset || desc.Memory == gpu::MemoryType::ReadBack ||
        (desc.Memory == gpu::MemoryType::Device && (!desc.Usage.HasFlag(gpu::BufferUse::CopyDestination) || offset % std::max(uint64_t{1}, _data.Constraints.BufferCopyAlignment))) ||
        (desc.Memory == gpu::MemoryType::Upload && !desc.Usage.HasFlag(gpu::BufferUse::MapWrite))) {
        Fail(_data, "invalid upload range, memory or usage");
        return {};
    }
    if (!source.GetMaxSize() && !resource.Dynamic) return {};
    const bool host = desc.Memory == gpu::MemoryType::Upload;
    const auto payload = static_cast<uint32_t>(_data.Uploads.size());
    _data.Uploads.push_back({std::move(source), {false, dst.Index, static_cast<uint32_t>(host ? gpu::BufferState::HostWrite : gpu::BufferState::CopyDestination), false, true, true}, offset});
    return {AddNode(_data, host ? RDGPassKind::HostWrite : RDGPassKind::Upload, payload)};
}

RDGPassHandle RDGBuilder::UploadTexture(RDGTextureHandle dst, const gpu::TextureRegion2D& region, RDGTextureUploadSource source) {
    RequireOpen(_data);
    if (dst.Index >= _data.Textures.size()) {
        Fail(_data, "invalid texture upload destination");
        return {};
    }
    const auto& desc = _data.Textures[dst.Index].Desc;
    if (!gpu::ValidateTextureUploadRegion(desc, region).first) {
        Fail(_data, "invalid texture upload region or format");
        return {};
    }
    const auto block = gpu::GetTextureFormatBlockInfo(desc.Format);
    const uint64_t rowBytes = uint64_t{block.Columns(region.Width)} * block.BytesPerBlock;
    const auto rows = block.Rows(region.Height);
    const auto imageRows = source.RowsPerImage ? source.RowsPerImage : rows;
    const uint64_t lastRow = uint64_t{region.Depth - 1} * imageRows + rows - 1;
    const auto pitch = source.SourceRowPitch ? source.SourceRowPitch : rowBytes;
    if (imageRows < rows || pitch < rowBytes || source.Source.GetMaxSize() < rowBytes ||
        lastRow > (source.Source.GetMaxSize() - rowBytes) / pitch) {
        Fail(_data, "texture source does not cover the region");
        return {};
    }
    const auto payload = static_cast<uint32_t>(_data.Uploads.size());
    _data.Uploads.push_back({std::move(source.Source), {true, dst.Index, static_cast<uint32_t>(gpu::TextureState::CopyDestination), false, true, true}, 0, pitch, region, imageRows});
    return {AddNode(_data, RDGPassKind::Upload, payload)};
}

void RDGBuilder::MarkOutput(RDGTextureHandle texture) {
    RequireOpen(_data);
    if (texture.Index >= _data.Textures.size())
        Fail(_data, "invalid texture output");
    else
        _data.Textures[texture.Index].Output = true;
}

void RDGBuilder::MarkOutput(RDGBufferHandle buffer) {
    RequireOpen(_data);
    if (buffer.Index >= _data.Buffers.size())
        Fail(_data, "invalid buffer output");
    else
        _data.Buffers[buffer.Index].Output = true;
}

uint32_t RDGBuilder::AddRasterPassImpl(std::string_view name, detail::ErasedPassCallback callback, bool (*execute)(const void*, RDGRasterPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept) {
    const auto index = AddPass(_data, name, RDGPassKind::Raster);
    User(_data, _data.Passes[index]).Callback = std::move(callback);
    User(_data, _data.Passes[index]).RasterExecute = execute;
    User(_data, _data.Passes[index]).Prepare = prepare;
    return index;
}

uint32_t RDGBuilder::AddComputePassImpl(std::string_view name, detail::ErasedPassCallback callback, bool (*execute)(const void*, RDGComputePassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept) {
    const auto index = AddPass(_data, name, RDGPassKind::Compute);
    User(_data, _data.Passes[index]).Callback = std::move(callback);
    User(_data, _data.Passes[index]).ComputeExecute = execute;
    User(_data, _data.Passes[index]).Prepare = prepare;
    return index;
}

uint32_t RDGBuilder::AddCopyPassImpl(std::string_view name, detail::ErasedPassCallback callback, bool (*execute)(const void*, RDGCopyPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept) {
    const auto index = AddPass(_data, name, RDGPassKind::Copy);
    User(_data, _data.Passes[index]).Callback = std::move(callback);
    User(_data, _data.Passes[index]).CopyExecute = execute;
    User(_data, _data.Passes[index]).Prepare = prepare;
    return index;
}

RDGRasterPassBuilder::RDGRasterPassBuilder(detail::RDGData& data, uint32_t pass) noexcept : _data(data), _pass(pass) {}
RDGComputePassBuilder::RDGComputePassBuilder(detail::RDGData& data, uint32_t pass) noexcept : _data(data), _pass(pass) {}
RDGCopyPassBuilder::RDGCopyPassBuilder(detail::RDGData& data, uint32_t pass) noexcept : _data(data), _pass(pass) {}

RDGPassHandle RDGRasterPassBuilder::GetHandle() const noexcept { return {_pass}; }
RDGPassHandle RDGComputePassBuilder::GetHandle() const noexcept { return {_pass}; }
RDGPassHandle RDGCopyPassBuilder::GetHandle() const noexcept { return {_pass}; }

RDGRasterPassBuilder& RDGRasterPassBuilder::SetRenderArea(Rect area) {
    RequireOpen(_data);
    User(_data, _data.Passes[_pass]).RenderArea = area;
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseMeshPass(MeshPassId pass) {
    RequireOpen(_data);
    if (pass.Value == std::numeric_limits<uint32_t>::max()) {
        Fail(_data, "invalid MeshPassId");
        return *this;
    }
    User(_data, _data.Passes[_pass]).MeshPasses.push_back(pass);
    return *this;
}

namespace {

void AddColorAttachment(RDGData& data, uint32_t pass, const RDGColorAttachment& attachment) {
    RequireOpen(data);
    auto& colors = User(data, data.Passes[pass]).Colors;
    for (const auto& color : colors) {
        if (color.Slot == attachment.Slot || color.Texture == attachment.Texture) {
            Fail(data, "duplicate color attachment");
            return;
        }
    }
    colors.push_back(attachment);
    const bool load = attachment.Load == gpu::LoadAction::Load;
    AddAccess(data, pass, {true, attachment.Texture.Index, static_cast<uint32_t>(gpu::TextureState::RenderTarget), load, true, load});
}

void AddDepthAttachment(RDGData& data, uint32_t pass, const RDGDepthAttachment& attachment) {
    RequireOpen(data);
    auto& raster = User(data, data.Passes[pass]);
    if (raster.Depth || DepthUsage(attachment.State) == gpu::TextureViewUsage::UNKNOWN) {
        Fail(data, "invalid or duplicate depth/stencil attachment");
        return;
    }
    raster.Depth = attachment;
    const bool load = attachment.DepthLoad == gpu::LoadAction::Load || attachment.StencilLoad == gpu::LoadAction::Load;
    const bool write = attachment.State != gpu::TextureState::DepthRead;
    AddAccess(data, pass, {true, attachment.Texture.Index, static_cast<uint32_t>(attachment.State), load || !write, write, load});
}

}  // namespace

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteColor(uint32_t slot, RDGTextureHandle texture, gpu::LoadAction load, gpu::StoreAction store,
                                                       gpu::ColorClearValue clear, Nullable<gpu::TextureView*> explicitView) {
    AddColorAttachment(_data, _pass, {slot, texture, load, store, clear, explicitView});
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteColor(uint32_t slot, RDGTextureHandle texture, gpu::LoadAction load, gpu::StoreAction store,
                                                       gpu::ColorClearValue clear, gpu::SubresourceRange range) {
    AddColorAttachment(_data, _pass, {slot, texture, load, store, clear, nullptr, range});
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteColor(uint32_t slot, RDGTextureHandle texture, gpu::LoadAction load, gpu::StoreAction store,
                                                       gpu::ColorClearValue clear, RDGAttachmentSlice slice) {
    return WriteColor(slot, texture, load, store, clear, slice.ToSubresourceRange());
}

RDGRasterPassBuilder& RDGRasterPassBuilder::SetDepthStencil(RDGTextureHandle texture, gpu::TextureState state,
                                                            gpu::LoadAction depthLoad, gpu::StoreAction depthStore,
                                                            gpu::LoadAction stencilLoad, gpu::StoreAction stencilStore,
                                                            gpu::DepthStencilClearValue clear, Nullable<gpu::TextureView*> explicitView) {
    AddDepthAttachment(_data, _pass, {texture, state, depthLoad, depthStore, stencilLoad, stencilStore, clear, explicitView});
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::SetDepthStencil(RDGTextureHandle texture, gpu::TextureState state,
                                                            gpu::LoadAction depthLoad, gpu::StoreAction depthStore,
                                                            gpu::LoadAction stencilLoad, gpu::StoreAction stencilStore,
                                                            gpu::DepthStencilClearValue clear, gpu::SubresourceRange range) {
    AddDepthAttachment(_data, _pass, {texture, state, depthLoad, depthStore, stencilLoad, stencilStore, clear, nullptr, range});
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::SetDepthStencil(RDGTextureHandle texture, gpu::TextureState state,
                                                            gpu::LoadAction depthLoad, gpu::StoreAction depthStore,
                                                            gpu::LoadAction stencilLoad, gpu::StoreAction stencilStore,
                                                            gpu::DepthStencilClearValue clear, RDGAttachmentSlice slice) {
    return SetDepthStencil(texture, state, depthLoad, depthStore, stencilLoad, stencilStore, clear, slice.ToSubresourceRange());
}

void RDGBuilder::AddOrderDependency(RDGPassHandle before, RDGPassHandle after) {
    RequireOpen(_data);
    if (before.Index >= after.Index || after.Index >= _data.Passes.size()) {
        Fail(_data, "order dependency must connect two declared passes in forward order");
        return;
    }
    auto& deps = _data.Passes[after.Index].OrderDependencies;
    if (std::find(deps.begin(), deps.end(), before.Index) == deps.end()) deps.push_back(before.Index);
}

void RDGBuilder::AddOrderBarrier(RDGPassHandle barrier) {
    RequireOpen(_data);
    if (!barrier.IsValid() || barrier.Index >= _data.Passes.size() || barrier.Index != _data.Passes.size() - 1) {
        Fail(_data, "order barrier must be the latest declared pass");
        return;
    }
    if (_data.Passes[barrier.Index].OrderBarrier) {
        Fail(_data, "duplicate order barrier");
        return;
    }
    _data.Passes[barrier.Index].OrderBarrier = true;
}

Nullable<CompiledRDG*> RDGBuilder::Compile() {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile");
    RADRAY_PROFILE_PLOT("RadRay.RDG.DeclaredPasses", static_cast<int64_t>(_data.Passes.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.DeclaredTextures", static_cast<int64_t>(_data.Textures.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.DeclaredBuffers", static_cast<int64_t>(_data.Buffers.size()));
    RequireOpen(_data);
    _data.Sealed = true;
    if (_data.Failed) return nullptr;
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile.ValidateResources");
        for (const auto& resource : _data.Textures) {
            const auto& d = resource.Desc;
            if (!d.Width || !d.Height || !d.DepthOrArraySize || !d.MipLevels || !d.SampleCount || d.Dim == gpu::TextureDimension::UNKNOWN || d.Format == gpu::TextureFormat::UNKNOWN ||
                (resource.Imported && (!ValidTextureState(d, resource.InitialState) || (resource.FinalState && !ValidTextureState(d, *resource.FinalState))))) Fail(_data, "invalid texture descriptor or boundary state");
        }
        for (const auto& resource : _data.Buffers) {
            if ((!resource.Desc.Size && !resource.Dynamic) || (_data.Constraints.MaxBufferSize && resource.Desc.Size > _data.Constraints.MaxBufferSize && !resource.Dynamic) || (resource.Imported && (!ValidBufferState(resource.Desc, resource.InitialState) || (resource.FinalState && !ValidBufferState(resource.Desc, *resource.FinalState))))) Fail(_data, "invalid buffer descriptor or boundary state");
        }
    }
    std::pmr::vector<bool> gpuAccessed(_data.Buffers.size(), false, &_data.ScratchResource);
    std::pmr::vector<std::pmr::vector<std::pair<uint64_t, uint64_t>>> hostRanges(_data.Buffers.size(), &_data.ScratchResource);
    std::pmr::vector<uint32_t> seenData(_data.Passes.size(), std::numeric_limits<uint32_t>::max(), &_data.ScratchResource);
    std::pmr::vector<uint32_t> seenOrder(_data.Passes.size(), std::numeric_limits<uint32_t>::max(), &_data.ScratchResource);
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile.Dependencies");
        for (uint32_t index = 0; index < _data.Passes.size(); ++index) {
            auto& pass = _data.Passes[index];
            const auto addEdge = [index](auto& dependencies, auto& seen, uint32_t predecessor) {
                if (seen[predecessor] == index) return;
                seen[predecessor] = index;
                dependencies.push_back(predecessor);
            };
            if (pass.Kind == RDGPassKind::Raster) std::sort(User(_data, pass).Colors.begin(), User(_data, pass).Colors.end(), [](const auto& a, const auto& b) { return a.Slot < b.Slot; });
            ValidateAttachments(_data, pass);
            for (const auto& access : Accesses(_data, pass)) {
                if (_data.Options.FullValidation && !ValidAccess(_data, pass, access)) Fail(_data, radray::format("pass '{}': {}#{} state {} is incompatible with usage, access mode or pass kind", PassName(_data, pass), access.Texture ? "texture" : "buffer", access.Index, access.State));
                if (!access.Texture && _data.Buffers[access.Index].Desc.Memory == gpu::MemoryType::Upload &&
                    access.Write && pass.Kind != RDGPassKind::HostWrite) Fail(_data, "Upload buffers only accept Prepare writes");
                auto& lastWriter = access.Texture ? _data.Textures[access.Index].LastWriter : _data.Buffers[access.Index].LastWriter;
                const bool imported = access.Texture ? bool(_data.Textures[access.Index].Imported) : bool(_data.Buffers[access.Index].Imported);
                const bool undefined = access.Texture ? _data.Textures[access.Index].InitialState == gpu::TextureState::Undefined : _data.Buffers[access.Index].InitialState == gpu::BufferState::Undefined;
                if (access.Read && !lastWriter && (!imported || undefined)) Fail(_data, "resource read before initialization");
                auto& readers = access.Texture ? _data.Textures[access.Index].Readers : _data.Buffers[access.Index].Readers;
                if ((access.Read || access.Preserve) && lastWriter) addEdge(pass.DataDependencies, seenData, *lastWriter);
                if (access.Write) {
                    if (!access.Read && !access.Preserve && lastWriter) addEdge(pass.OrderDependencies, seenOrder, *lastWriter);
                    for (const auto reader : readers) addEdge(pass.OrderDependencies, seenOrder, reader);
                    lastWriter = index;
                    readers.clear();
                } else if (access.Read) {
                    readers.push_back(index);
                }
                if (!access.Texture && _data.Buffers[access.Index].Desc.Memory == gpu::MemoryType::Upload) {
                    if (pass.Kind == RDGPassKind::HostWrite) {
                        if (_data.Buffers[access.Index].OwnerWrites) Fail(_data, "persistent upload buffer is written by its owner");
                        if (gpuAccessed[access.Index]) Fail(_data, "direct upload must precede GPU access");
                        hostRanges[access.Index].push_back({Upload(_data, pass).UploadOffset, Upload(_data, pass).UploadOffset + Upload(_data, pass).Source.GetMaxSize()});
                    } else {
                        gpuAccessed[access.Index] = true;
                    }
                }
            }
        }
    }
    for (auto& ranges : hostRanges) {
        std::sort(ranges.begin(), ranges.end());
        for (size_t i = 1; i < ranges.size(); ++i) {
            if (ranges[i].first < ranges[i - 1].second) Fail(_data, "overlapping direct upload ranges");
        }
    }
    if (_data.Failed) return nullptr;
    std::pmr::vector<uint32_t> pending{&_data.ScratchResource};
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile.Cull");
        for (uint32_t i = 0; i < _data.Passes.size(); ++i) {
            if (_data.Passes[i].NeverCull) pending.push_back(i);
        }
        const auto roots = [&](auto& resources) {
            for (auto& resource : resources) {
                if (!resource.Output) continue;
                resource.Alive = true;
                if (resource.LastWriter)
                    pending.push_back(*resource.LastWriter);
                else if (!resource.Imported)
                    Fail(_data, "transient output has no writer");
            }
        };
        roots(_data.Textures);
        roots(_data.Buffers);
        if (_data.Failed) return nullptr;
        while (!pending.empty()) {
            const auto index = pending.back();
            pending.pop_back();
            auto& pass = _data.Passes[index];
            if (pass.Alive) continue;
            pass.Alive = true;
            pending.insert(pending.end(), pass.DataDependencies.begin(), pass.DataDependencies.end());
        }
        for (const auto& pass : _data.Passes) {
            if (!pass.Alive) continue;
            for (const auto& access : Accesses(_data, pass)) {
                if (access.Texture)
                    _data.Textures[access.Index].Alive = true;
                else
                    _data.Buffers[access.Index].Alive = true;
            }
            if (IsUpload(pass)) continue;
            for (const auto meshPass : User(_data, pass).MeshPasses) {
                if (std::find(_data.AliveMeshPasses.begin(), _data.AliveMeshPasses.end(), meshPass) == _data.AliveMeshPasses.end()) _data.AliveMeshPasses.push_back(meshPass);
            }
        }
    }
    std::optional<uint32_t> lastBarrier;
    std::pmr::vector<uint32_t> interval{&_data.ScratchResource};
    for (uint32_t i = 0; i < _data.Passes.size(); ++i) {
        auto& pass = _data.Passes[i];
        if (!pass.Alive) continue;
        if (pass.OrderBarrier) {
            if (interval.empty() && lastBarrier) pass.OrderDependencies.push_back(*lastBarrier);
            pass.OrderDependencies.insert(pass.OrderDependencies.end(), interval.begin(), interval.end());
            interval.clear();
            lastBarrier = i;
        } else {
            if (lastBarrier) pass.OrderDependencies.push_back(*lastBarrier);
            interval.push_back(i);
        }
    }
    if (!SchedulePasses(_data)) return nullptr;
    RADRAY_PROFILE_PLOT("RadRay.RDG.AlivePasses", static_cast<int64_t>(_data.ScheduledPasses.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.ExecutionGroups", static_cast<int64_t>(_data.Groups.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.AliveMeshPasses", static_cast<int64_t>(_data.AliveMeshPasses.size()));
    return _slot.Emplace<CompiledRDG>(_data);
}

CompiledRDG::CompiledRDG(detail::RDGData& data) noexcept : _data(data) {}
void CompiledRDG::ExecutionDeleter::operator()(detail::RDGExecution* execution) const noexcept {
    std::pmr::polymorphic_allocator<>{&execution->Data.MemoryResource}.delete_object(execution);
}
CompiledRDG::~CompiledRDG() noexcept {
    // 参数组先于帧内视图解除引用；底层 owner 由 RDGData 最后释放。
    for (auto& pass : _data.UserPasses) pass.Callback.Reset();
}
std::span<const MeshPassId> CompiledRDG::GetAliveMeshPasses() const noexcept { return _data.AliveMeshPasses; }

bool CompiledRDG::Prepare(FrameSlot& slot, RDGTransientResourcePool& pool, UploadArena& uploads) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Prepare");
    if (_prepareAttempted) return false;
    _prepareAttempted = true;
    if (&slot.GetMemoryResource() != &_data.MemoryResource) return false;
    _execution.reset(std::pmr::polymorphic_allocator<>{&_data.MemoryResource}.new_object<RDGExecution>(RDGExecution{_data, slot, pool, uploads}));
    auto& execution = *_execution;
    const auto& constraints = _data.Constraints;
    RDGUploadPrepareContext layout{slot, _data.AliveMeshPasses};
    size_t aliveUploads = 0;
    for (const auto index : _data.ScheduledPasses) aliveUploads += IsUpload(_data.Passes[index]);
    execution.PreparedUploads.reserve(aliveUploads);
    std::pmr::vector<uint64_t> sizes{&_data.ScratchResource};
    sizes.reserve(_data.Buffers.size());
    for (const auto& buffer : _data.Buffers) sizes.push_back(buffer.Desc.Size);
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.PrepareLayout");
        for (const auto index : _data.ScheduledPasses) {
            auto& pass = _data.Passes[index];
            if (pass.Kind != RDGPassKind::Upload && pass.Kind != RDGPassKind::HostWrite) continue;
            pass.PreparedUpload = static_cast<uint32_t>(execution.PreparedUploads.size());
            execution.PreparedUploads.push_back({.Barriers = std::pmr::vector<gpu::ResourceBarrierDescriptor>{&_data.MemoryResource}});
            Prepared(execution, pass).ActualSize = Upload(_data, pass).Source.PrepareLayout(layout);
            if (Prepared(execution, pass).ActualSize > Upload(_data, pass).Source.GetMaxSize()) return false;
            const auto& access = Accesses(_data, pass).front();
            if (access.Texture) {
                if (Prepared(execution, pass).ActualSize != Upload(_data, pass).Source.GetMaxSize()) return false;
                // 来源覆盖量包含行 padding；实际有效量只计算像素。
                const auto& region = Upload(_data, pass).Region;
                const auto block = gpu::GetTextureFormatBlockInfo(_data.Textures[access.Index].Desc.Format);
                Prepared(execution, pass).ActualSize = uint64_t{block.Columns(region.Width)} * block.Rows(region.Height) * region.Depth * block.BytesPerBlock;
            }
            if (!access.Texture) {
                const auto& resource = _data.Buffers[access.Index];
                if (resource.Dynamic)
                    sizes[access.Index] = Prepared(execution, pass).ActualSize;
                else if (!Prepared(execution, pass).ActualSize && (!resource.Imported || resource.InitialState == gpu::BufferState::Undefined))
                    return false;
                if (Upload(_data, pass).UploadOffset > sizes[access.Index] || Prepared(execution, pass).ActualSize > sizes[access.Index] - Upload(_data, pass).UploadOffset) return false;
                if (pass.Kind == RDGPassKind::Upload && Upload(_data, pass).UploadOffset % std::max(uint64_t{1}, constraints.BufferCopyAlignment)) return false;
            }
        }
    }
    execution.States.Initialize();
    execution.Slices.resize(_data.Buffers.size());
    std::pmr::vector<UploadAllocation> direct(_data.Buffers.size(), &_data.ScratchResource);
    std::pmr::vector<gpu::BufferStates> reads(_data.Buffers.size(), &_data.ScratchResource);
    std::pmr::vector<uint32_t> uploadCounts(_data.Textures.size() + _data.Buffers.size(), 0, &_data.ScratchResource);
    for (const auto index : _data.ScheduledPasses) {
        for (const auto& access : Accesses(_data, _data.Passes[index])) {
            if (!access.Texture && !access.Write) reads[access.Index] |= gpu::BufferStates{access.State};
            if (_data.Passes[index].Kind == RDGPassKind::Upload) ++uploadCounts[AccessIndex(_data, access)];
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.AcquireResources");
        for (size_t i = 0; i < _data.Textures.size(); ++i) {
            const auto& resource = _data.Textures[i];
            if (!resource.Alive) continue;
            const auto allocation = resource.Imported ? RDGTransientTextureAllocation{resource.Imported, resource.InitialState} : pool.AcquireTexture(resource.Desc);
            if (!allocation.Texture) return false;
            execution.States.BindTexture(static_cast<uint32_t>(i), allocation.Texture.Get(), allocation.LastState);
        }
        for (size_t i = 0; i < _data.Buffers.size(); ++i) {
            const auto& resource = _data.Buffers[i];
            if (!resource.Alive || !sizes[i]) continue;
            if (!resource.Imported && resource.Desc.Memory == gpu::MemoryType::Upload) {
                const auto usage = resource.Desc.Usage;
                const auto resolvedReads = reads[i] ? reads[i] : BufferReads(usage);
                const bool stagingOnly = (usage & ~gpu::BufferUses{gpu::BufferUse::MapWrite}) == gpu::BufferUse::CopySource;
                const auto allocation = stagingOnly
                                            ? AllocateStagingBuffer(uploads, constraints, sizes[i], resource.Alignment)
                                            : AllocateBuffer(uploads, constraints, sizes[i], usage, resource.Alignment, resolvedReads);
                if (!allocation) return false;
                direct[i] = *allocation;
                execution.Slices[i] = {allocation->Buffer, allocation->Offset, sizes[i]};
                execution.States.BindBuffer(static_cast<uint32_t>(i), allocation->Buffer.Get(), resolvedReads);
            } else {
                const auto allocation = resource.Imported ? RDGTransientBufferAllocation{resource.Imported, resource.InitialState} : pool.AcquireBuffer(resource.Desc);
                if (!allocation.Buffer) return false;
                execution.States.BindBuffer(static_cast<uint32_t>(i), allocation.Buffer.Get(), allocation.LastState);
                execution.Slices[i] = {allocation.Buffer, 0, sizes[i]};
            }
        }
    }
    // 所有视图在用户 Prepare 之前物化，Record 只查准备表。
    for (const auto index : _data.ScheduledPasses) {
        auto& pass = _data.Passes[index];
        execution.Pass = &pass;
        for (const auto& access : Accesses(_data, pass)) {
            if (!access.Texture) continue;
            const auto state = static_cast<gpu::TextureState>(access.State);
            if (state != gpu::TextureState::ShaderRead && state != gpu::TextureState::UnorderedAccess) continue;
            const auto usage = state == gpu::TextureState::ShaderRead ? gpu::TextureViewUsage::Resource : gpu::TextureViewUsage::UnorderedAccess;
            if (!ResolveTextureView(execution, {access.Index}, DefaultViewRequest(usage), true)) return false;
        }
        if (pass.Kind == RDGPassKind::Raster) {
            const auto& raster = User(_data, pass);
            for (const auto& color : raster.Colors) {
                if (!ResolveAttachmentView(execution, color, gpu::TextureViewUsage::RenderTarget, true)) return false;
            }
            if (raster.Depth && !ResolveAttachmentView(execution, *raster.Depth, DepthUsage(raster.Depth->State), true)) return false;
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.FillUploads");
        for (const auto index : _data.ScheduledPasses) {
            auto& pass = _data.Passes[index];
            if (pass.Kind != RDGPassKind::Upload && pass.Kind != RDGPassKind::HostWrite) continue;
            const auto& access = Accesses(_data, pass).front();
            RADRAY_PROFILE_PLOT("RadRay.RDG.UploadBytes", static_cast<int64_t>(Prepared(execution, pass).ActualSize));
            if (!Prepared(execution, pass).ActualSize) {
                Upload(_data, pass).Source.ReleaseSource();
                continue;
            }
            uint64_t mappedSize = Prepared(execution, pass).ActualSize, rowBytes = 0;
            uint32_t rows = 1, depth = 1;
            if (access.Texture) {
                const auto block = gpu::GetTextureFormatBlockInfo(_data.Textures[access.Index].Desc.Format);
                const auto& region = Upload(_data, pass).Region;
                rowBytes = uint64_t{block.Columns(region.Width)} * block.BytesPerBlock;
                rows = block.Rows(region.Height);
                depth = region.Depth;
                const auto pitchAlignment = MergeAlignment(constraints.TexturePitchAlignment, block.BytesPerBlock);
                const auto placement = MergeAlignment(constraints.TexturePlacementAlignment, block.BytesPerBlock);
                if (!pitchAlignment || !placement) return false;
                const auto pitch = AlignBytes(rowBytes, *pitchAlignment);
                const uint64_t lastRow = uint64_t{rows} * depth - 1;
                if (!pitch || *pitch > std::numeric_limits<uint32_t>::max() || lastRow > (std::numeric_limits<uint64_t>::max() - rowBytes) / *pitch) return false;
                Prepared(execution, pass).StagingPitch = static_cast<uint32_t>(*pitch);
                mappedSize = *pitch * lastRow + rowBytes;
                if (constraints.MaxBufferSize && mappedSize > constraints.MaxBufferSize) return false;
                const auto allocation = uploads.AllocateStaging(mappedSize, *placement);
                if (!allocation) return false;
                Prepared(execution, pass).Staging = *allocation;
            } else if (pass.Kind == RDGPassKind::Upload) {
                const auto allocation = AllocateStagingBuffer(uploads, constraints, mappedSize, 1);
                if (!allocation) return false;
                Prepared(execution, pass).Staging = *allocation;
            } else if (direct[access.Index].Buffer) {
                Prepared(execution, pass).Staging = direct[access.Index];
                Prepared(execution, pass).Staging.Offset += Upload(_data, pass).UploadOffset;
                Prepared(execution, pass).Staging.Data = Prepared(execution, pass).Staging.Data.Get() + Upload(_data, pass).UploadOffset;
                Prepared(execution, pass).Staging.Size = mappedSize;
            } else {
                auto& physical = execution.States.GetBuffer(access.Index);
                auto* mapped = static_cast<byte*>(physical.Resource->GetMappedData().Get());
                if (!mapped) return false;
                Prepared(execution, pass).Staging = {physical.Resource, mapped + Upload(_data, pass).UploadOffset, Upload(_data, pass).UploadOffset, mappedSize, mappedSize};
            }
            RDGUploadWriteContext write{{Prepared(execution, pass).Staging.Data.Get(), static_cast<size_t>(mappedSize)}, Prepared(execution, pass).ActualSize, Prepared(execution, pass).StagingPitch, rowBytes, rows, depth};
            const bool written = Upload(_data, pass).Source.Write(write, Upload(_data, pass).SourceRowPitch, Upload(_data, pass).SourceRowsPerImage);
            if (pass.Kind == RDGPassKind::HostWrite && _data.Buffers[access.Index].Imported) {
                auto& physical = execution.States.GetBuffer(access.Index);
                if (written && !physical.Resource->FlushMappedRange({Upload(_data, pass).UploadOffset, mappedSize})) return false;
                execution.States.PrepareHostWrite(access.Index);
            }
            Prepared(execution, pass).Staging.Data = nullptr;
            Upload(_data, pass).Source.ReleaseSource();
            if (!written) return false;
        }
    }
    for (const auto index : _data.ScheduledPasses) {
        auto& pass = _data.Passes[index];
        execution.Pass = &pass;
        if (!IsUpload(pass) && User(_data, pass).Prepare) {
            RADRAY_PROFILE_SCOPE_N("RadRay.RDG.PrepareBindings");
            RDGPrepareContext context{execution};
            if (!User(_data, pass).Prepare.Get()(User(_data, pass).Callback.Get(), context)) return false;
        }
    }
    if (!uploads.Flush()) return false;
    execution.States.PrepareBarriers(uploads, execution.PreparedUploads, uploadCounts);
    for (auto& group : _data.Groups) {
        if (group.Kind == RDGPassKind::Raster) {
            execution.Pass = &_data.Passes[group.Passes.front()];
            const auto& first = _data.Passes[group.Passes.front()];
            const auto& last = _data.Passes[group.Passes.back()];
            for (size_t i = 0; i < User(_data, first).Colors.size(); ++i) {
                const auto& a = User(_data, first).Colors[i];
                const auto view = ResolveAttachmentView(execution, a, gpu::TextureViewUsage::RenderTarget, false);
                if (!view) return false;
                group.Colors.push_back({view.Get(), a.Load, User(_data, last).Colors[i].Store, a.Clear});
            }
            if (User(_data, first).Depth) {
                const auto& a = *User(_data, first).Depth;
                const auto view = ResolveAttachmentView(execution, a, DepthUsage(a.State), false);
                if (!view) return false;
                group.Depth = {view.Get(), a.DepthLoad, User(_data, last).Depth->DepthStore, a.StencilLoad, User(_data, last).Depth->StencilStore, a.Clear};
            }
        }
    }
    execution.Pass = nullptr;
    _prepared = true;
    return true;
}

vector<string> CompiledRDG::GetRecordedPassNames() const {
    vector<string> result;
    for (const auto& group : _data.Groups)
        for (const auto index : group.Passes) result.emplace_back(PassName(_data, _data.Passes[index]));
    return result;
}

bool CompiledRDG::Record(gpu::CommandBuffer& command, Nullable<gpu::QueryPool*> passTimestamps) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Record");
    if (!_prepared || _recordAttempted) return false;
    if (passTimestamps) {
        uint64_t required = 2;
        for (const auto& group : _data.Groups) required += 2ull * group.Passes.size();
        if (required > passTimestamps->GetCount()) return false;
    }
    _recordAttempted = true;
    auto& execution = *_execution;
    execution.Command = &command;
    uint32_t nextQuery = 2;
    const auto timestamp = [&](gpu::QueryPipelineStage stage) {
        if (passTimestamps) command.WriteTimestamp({passTimestamps.Get(), stage, nextQuery++});
    };
    if (!execution.States.GetBeginBarriers().empty()) command.ResourceBarrier(execution.States.GetBeginBarriers());
    for (const auto& group : _data.Groups) {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.RecordGroup");
        RADRAY_PROFILE_PLOT("RadRay.RDG.GroupBarriers", static_cast<int64_t>(group.Barriers.size()));
        timestamp(gpu::QueryPipelineStage::Top);
        const auto beginPass = [&](uint32_t index) {
            if (index != group.Passes.front()) timestamp(gpu::QueryPipelineStage::Top);
        };
        const auto endPass = [&](uint32_t index) {
            if (index != group.Passes.back()) timestamp(gpu::QueryPipelineStage::Bottom);
        };
        if (!group.Barriers.empty()) command.ResourceBarrier(group.Barriers);
        const auto& first = _data.Passes[group.Passes.front()];
        bool result = true;
        if (group.Kind == RDGPassKind::Upload || group.Kind == RDGPassKind::Copy) {
            command.PushDebugGroup(group.Kind == RDGPassKind::Upload ? "RDG UploadBatch" : PassName(_data, first));
            for (const auto index : group.Passes) {
                const auto& pass = _data.Passes[index];
                execution.Pass = &pass;
                RADRAY_PROFILE_SCOPE_DYN(PassName(_data, pass));
                beginPass(index);
                if (group.Kind == RDGPassKind::Copy) {
                    RDGCopyPassContext context{execution};
                    result = User(_data, pass).CopyExecute.Get()(User(_data, pass).Callback.Get(), context);
                } else if (Prepared(execution, pass).ActualSize) {
                    if (!Prepared(execution, pass).Barriers.empty()) command.ResourceBarrier(Prepared(execution, pass).Barriers);
                    const auto& access = Accesses(_data, pass).front();
                    if (access.Texture) {
                        const auto& r = Upload(_data, pass).Region;
                        result = command.CopyBufferToTextureRegion({Prepared(execution, pass).Staging.Buffer.Get(), execution.States.GetTexture(access.Index).Resource.Get(), {Prepared(execution, pass).Staging.Offset, Prepared(execution, pass).StagingPitch, r}});
                    } else {
                        const auto target = execution.Slices[access.Index];
                        command.CopyBufferToBuffer(target.Buffer.Get(), target.Offset + Upload(_data, pass).UploadOffset, Prepared(execution, pass).Staging.Buffer.Get(), Prepared(execution, pass).Staging.Offset, Prepared(execution, pass).ActualSize);
                    }
                }
                endPass(index);
                if (!result) break;
            }
            command.PopDebugGroup();
        } else if (group.Kind == RDGPassKind::Compute) {
            auto encoder = command.BeginComputePass();
            if (!encoder) return false;
            RDGComputePassContext context{execution, *encoder};
            for (const auto index : group.Passes) {
                const auto& pass = _data.Passes[index];
                execution.Pass = &pass;
                RADRAY_PROFILE_SCOPE_DYN(PassName(_data, pass));
                command.PushDebugGroup(PassName(_data, pass));
                beginPass(index);
                result = User(_data, pass).ComputeExecute.Get()(User(_data, pass).Callback.Get(), context);
                endPass(index);
                command.PopDebugGroup();
                if (!result) break;
            }
            command.EndComputePass(encoder.Release());
        } else {
            const auto& raster = User(_data, first);
            auto encoder = command.BeginRenderPass({.Colors = group.Colors, .DepthStencil = group.Depth, .Width = raster.Width, .Height = raster.Height, .Layers = raster.Layers,
                                                    .RenderArea = raster.RenderArea, .Name = PassName(_data, first), .AllowUavWrites = group.Uav});
            if (!encoder) return false;
            RDGRasterPassContext context{execution, *encoder};
            for (const auto index : group.Passes) {
                const auto& pass = _data.Passes[index];
                execution.Pass = &pass;
                RADRAY_PROFILE_SCOPE_DYN(PassName(_data, pass));
                command.PushDebugGroup(PassName(_data, pass));
                beginPass(index);
                encoder->SetStencilReference(0);
                result = User(_data, pass).RasterExecute.Get()(User(_data, pass).Callback.Get(), context);
                endPass(index);
                command.PopDebugGroup();
                if (!result) break;
            }
            command.EndRenderPass(encoder.Release());
        }
        timestamp(gpu::QueryPipelineStage::Bottom);
        if (!result) return false;
    }
    RADRAY_PROFILE_PLOT("RadRay.RDG.FinalBarriers", static_cast<int64_t>(execution.States.GetFinalBarriers().size() + execution.States.GetReleaseBarriers().size()));
    if (!execution.States.GetFinalBarriers().empty()) command.ResourceBarrier(execution.States.GetFinalBarriers());
    if (!execution.States.GetReleaseBarriers().empty()) command.ResourceBarrier(execution.States.GetReleaseBarriers());
    _recorded = true;
    return true;
}

void CompiledRDG::CommitSubmittedStates() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.CommitSubmittedStates");
    if (!_recorded || _committed) RADRAY_ABORT("RDG: commit requires successful Record and exactly one submission");
    _committed = true;
    auto& execution = *_execution;
    for (size_t i = 0; i < _data.Textures.size(); ++i) {
        auto& resource = _data.Textures[i];
        if (!resource.Alive) continue;
        const auto& physical = execution.States.GetTexture(static_cast<uint32_t >(i));
        if (resource.Persistent) resource.Persistent->CommitSubmittedState(physical.CurrentState);
        if (!resource.Imported) execution.Pool.CommitTextureState(physical.Resource.Get(), physical.CurrentState);
    }
    for (size_t i = 0; i < _data.Buffers.size(); ++i) {
        auto& resource = _data.Buffers[i];
        if (!resource.Alive) continue;
        const auto& physical = execution.States.GetBuffer(static_cast<uint32_t >(i));
        if (resource.Persistent) resource.Persistent->CommitSubmittedState(physical.CurrentState);
        if (!resource.Imported && resource.Desc.Memory != gpu::MemoryType::Upload) execution.Pool.CommitBufferState(physical.Resource.Get(), physical.CurrentState);
    }
    execution.Uploads.CommitSubmittedStates();
}

RDGPrepareContext::RDGPrepareContext(detail::RDGExecution& execution) noexcept : _execution(execution) {}
FrameSlot& RDGPrepareContext::GetSlot() noexcept { return _execution.Slot; }
RDGBufferSlice RDGPrepareContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }
gpu::Texture& RDGPrepareContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
Nullable<gpu::TextureView*> RDGPrepareContext::ResolveView(RDGTextureHandle handle, gpu::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }
Nullable<gpu::TextureView*> RDGPrepareContext::ResolveView(RDGTextureHandle handle, const RDGTextureViewDescriptor& view) const noexcept { return ResolveTextureView(_execution, handle, view, true); }

RDGRasterPassContext::RDGRasterPassContext(detail::RDGExecution& execution, gpu::GraphicsCommandEncoder& encoder) noexcept : _execution(execution), _encoder(encoder) {}
gpu::GraphicsCommandEncoder& RDGRasterPassContext::GetEncoder() noexcept { return _encoder; }
RDGComputePassContext::RDGComputePassContext(detail::RDGExecution& execution, gpu::ComputeCommandEncoder& encoder) noexcept : _execution(execution), _encoder(encoder) {}
gpu::ComputeCommandEncoder& RDGComputePassContext::GetEncoder() noexcept { return _encoder; }
RDGCopyPassContext::RDGCopyPassContext(detail::RDGExecution& execution) noexcept : _execution(execution) {}
gpu::CommandBuffer& RDGCopyPassContext::GetCommandBuffer() noexcept { return *_execution.Command; }
uint64_t RDGRasterPassContext::GetFrameId() const noexcept { return _execution.Slot.GetFrameId(); }
uint32_t RDGRasterPassContext::GetSlotIndex() const noexcept { return _execution.Slot.GetIndex(); }
std::span<const VisibleMeshDrawCommand> RDGRasterPassContext::GetVisibleCommands(MeshPassId pass) const noexcept { return std::as_const(_execution.Slot).GetPassDrawList(pass).VisibleCommands; }
gpu::Texture& RDGRasterPassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
RDGBufferSlice RDGRasterPassContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }
Nullable<gpu::TextureView*> RDGRasterPassContext::ResolveView(RDGTextureHandle handle, gpu::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }
Nullable<gpu::TextureView*> RDGRasterPassContext::ResolveView(RDGTextureHandle handle, const RDGTextureViewDescriptor& view) const noexcept { return ResolveTextureView(_execution, handle, view, false); }
uint64_t RDGComputePassContext::GetFrameId() const noexcept { return _execution.Slot.GetFrameId(); }
uint32_t RDGComputePassContext::GetSlotIndex() const noexcept { return _execution.Slot.GetIndex(); }
std::span<const VisibleMeshDrawCommand> RDGComputePassContext::GetVisibleCommands(MeshPassId pass) const noexcept { return std::as_const(_execution.Slot).GetPassDrawList(pass).VisibleCommands; }
gpu::Texture& RDGComputePassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
RDGBufferSlice RDGComputePassContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }
Nullable<gpu::TextureView*> RDGComputePassContext::ResolveView(RDGTextureHandle handle, gpu::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }
Nullable<gpu::TextureView*> RDGComputePassContext::ResolveView(RDGTextureHandle handle, const RDGTextureViewDescriptor& view) const noexcept { return ResolveTextureView(_execution, handle, view, false); }
uint64_t RDGCopyPassContext::GetFrameId() const noexcept { return _execution.Slot.GetFrameId(); }
uint32_t RDGCopyPassContext::GetSlotIndex() const noexcept { return _execution.Slot.GetIndex(); }
std::span<const VisibleMeshDrawCommand> RDGCopyPassContext::GetVisibleCommands(MeshPassId pass) const noexcept { return std::as_const(_execution.Slot).GetPassDrawList(pass).VisibleCommands; }
gpu::Texture& RDGCopyPassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
RDGBufferSlice RDGCopyPassContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadBuffer(RDGBufferHandle handle, gpu::BufferStates state) {
    BufferAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteBuffer(RDGBufferHandle handle, gpu::BufferState state) {
    BufferAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadWriteBuffer(RDGBufferHandle handle, gpu::BufferState state) {
    BufferAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadTexture(RDGTextureHandle handle, gpu::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteTexture(RDGTextureHandle handle, gpu::TextureState state) {
    TextureAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadWriteTexture(RDGTextureHandle handle, gpu::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::NeverCull() noexcept {
    RequireOpen(_data);
    _data.Passes[_pass].NeverCull = true;
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadBuffer(RDGBufferHandle handle, gpu::BufferStates state) {
    BufferAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::WriteBuffer(RDGBufferHandle handle, gpu::BufferState state) {
    BufferAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadWriteBuffer(RDGBufferHandle handle, gpu::BufferState state) {
    BufferAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadTexture(RDGTextureHandle handle, gpu::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::WriteTexture(RDGTextureHandle handle, gpu::TextureState state) {
    TextureAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadWriteTexture(RDGTextureHandle handle, gpu::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::NeverCull() noexcept {
    RequireOpen(_data);
    _data.Passes[_pass].NeverCull = true;
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::ReadBuffer(RDGBufferHandle handle) {
    BufferAccess(_data, _pass, handle, gpu::BufferState::CopySource, true, false);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::WriteBuffer(RDGBufferHandle handle) {
    BufferAccess(_data, _pass, handle, gpu::BufferState::CopyDestination, false, true);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::ReadTexture(RDGTextureHandle handle) {
    TextureAccess(_data, _pass, handle, gpu::TextureState::CopySource, true, false);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::WriteTexture(RDGTextureHandle handle) {
    TextureAccess(_data, _pass, handle, gpu::TextureState::CopyDestination, false, true);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::NeverCull() noexcept {
    RequireOpen(_data);
    _data.Passes[_pass].NeverCull = true;
    return *this;
}

}  // namespace radray
