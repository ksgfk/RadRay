#include <radray/runtime/rdg.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>
#include <map>
#include <memory_resource>
#include <set>
#include <unordered_map>
#include <vector>

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/gpu_device.h>

#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray {
namespace detail {

enum class RDGPassKind { Raster,
                         Compute,
                         Copy,
                         Upload,
                         HostWrite };

struct RDGAccess {
    bool Texture;
    uint32_t Index;
    uint32_t State;
    bool Read;
    bool Write;
    bool Preserve;
};

struct RDGColorAttachment {
    uint32_t Slot;
    RDGTextureHandle Texture;
    render::LoadAction Load;
    render::StoreAction Store;
    render::ColorClearValue Clear;
    Nullable<render::TextureView*> View;
};

struct RDGDepthAttachment {
    RDGTextureHandle Texture;
    render::TextureState State;
    render::LoadAction DepthLoad;
    render::StoreAction DepthStore;
    render::LoadAction StencilLoad;
    render::StoreAction StencilStore;
    render::DepthStencilClearValue Clear;
    Nullable<render::TextureView*> View;
};

struct RDGUserPass {
    string Name;
    std::pmr::vector<RDGAccess> Accesses;
    std::pmr::vector<MeshPassId> MeshPasses;
    std::pmr::vector<RDGColorAttachment> Colors;
    std::optional<RDGDepthAttachment> Depth{};
    shared_ptr<void> Callback{};
    Nullable<bool (*)(const void*, RDGRasterPassContext&) noexcept> RasterExecute{nullptr};
    Nullable<bool (*)(const void*, RDGComputePassContext&) noexcept> ComputeExecute{nullptr};
    Nullable<bool (*)(const void*, RDGCopyPassContext&) noexcept> CopyExecute{nullptr};
    Nullable<bool (*)(void*, RDGPrepareContext&) noexcept> Prepare{nullptr};
    uint32_t Width{1}, Height{1}, Layers{1};
};

struct RDGUpload {
    RDGUploadSource Source;
    RDGAccess Access;
    uint64_t UploadOffset{0};
    uint64_t SourceRowPitch{0};
    render::TextureRegion2D Region{};
};

struct RDGPass {
    RDGPassKind Kind;
    uint32_t Payload;
    std::pmr::vector<uint32_t> DataDependencies;
    std::pmr::vector<uint32_t> OrderDependencies;
    uint32_t PreparedUpload{std::numeric_limits<uint32_t>::max()};
    bool NeverCull{false};
    bool Alive{false};
};

struct RDGPreparedUpload {
    uint64_t ActualSize{0};
    uint32_t StagingPitch{0};
    UploadAllocation Staging{};
    std::pmr::vector<render::ResourceBarrierDescriptor> Barriers;
};

struct RDGTexture {
    render::TextureDescriptor Desc;
    Nullable<render::Texture*> Imported{nullptr};
    render::TextureState InitialState{render::TextureState::Undefined};
    std::optional<render::TextureState> FinalState{};
    std::optional<uint32_t> LastWriter{};
    std::pmr::vector<uint32_t> Readers;
    RDGExternalOwnership Ownership{};
    Nullable<GpuTexture*> Persistent{nullptr};
    IntrusivePtr<GpuTexture> Owner{};
    bool Output{false};
    bool Alive{false};
};

struct RDGBuffer {
    render::BufferDescriptor Desc;
    Nullable<render::Buffer*> Imported{nullptr};
    render::BufferStates InitialState{render::BufferState::Undefined};
    std::optional<render::BufferStates> FinalState{};
    std::optional<uint32_t> LastWriter{};
    std::pmr::vector<uint32_t> Readers;
    RDGExternalOwnership Ownership{};
    Nullable<GpuBuffer*> Persistent{nullptr};
    IntrusivePtr<GpuBuffer> Owner{};
    bool Dynamic{false};
    uint64_t Alignment{1};
    bool Output{false};
    bool Alive{false};
};

struct RDGExecutionGroup {
    RDGPassKind Kind;
    std::pmr::vector<uint32_t> Passes;
    std::pmr::vector<render::ResourceBarrierDescriptor> Barriers;
    std::pmr::vector<render::RenderPassColorAttachment> Colors;
    std::optional<render::RenderPassDepthStencilAttachment> Depth{};
    bool Uav{false};
};

struct RDGData {
    std::pmr::memory_resource& MemoryResource;
    std::pmr::memory_resource& ScratchResource;
    RDGDeviceConstraints Constraints;
    RDGCompileOptions Options;
    std::pmr::vector<RDGTexture> Textures{&MemoryResource};
    std::pmr::vector<RDGBuffer> Buffers{&MemoryResource};
    std::pmr::unordered_map<render::Texture*, uint32_t> TextureImports{&MemoryResource};
    std::pmr::unordered_map<render::Buffer*, uint32_t> BufferImports{&MemoryResource};
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
    void BindTexture(uint32_t index, render::Texture* texture, render::TextureState initialState) noexcept;
    void BindBuffer(uint32_t index, render::Buffer* buffer, render::BufferStates initialState) noexcept;
    const RDGPhysicalResource<render::Texture, render::TextureState>& GetTexture(uint32_t index) const noexcept { return _textures[index]; }
    const RDGPhysicalResource<render::Buffer, render::BufferStates>& GetBuffer(uint32_t index) const noexcept { return _buffers[index]; }
    void PrepareHostWrite(uint32_t index) noexcept;
    void PrepareBarriers(UploadArena& uploads, std::span<RDGPreparedUpload> preparedUploads, std::span<const uint32_t> uploadCounts);
    std::span<const render::ResourceBarrierDescriptor> GetBeginBarriers() const noexcept { return _beginBarriers; }
    std::span<const render::ResourceBarrierDescriptor> GetFinalBarriers() const noexcept { return _finalBarriers; }
    std::span<const render::ResourceBarrierDescriptor> GetReleaseBarriers() const noexcept { return _releaseBarriers; }

private:
    void TransitionTexture(uint32_t index, render::TextureState next, bool write, std::pmr::vector<render::ResourceBarrierDescriptor>& barriers) noexcept;
    void TransitionBuffer(uint32_t index, render::BufferStates next, bool write, std::pmr::vector<render::ResourceBarrierDescriptor>& barriers) noexcept;
    RDGData& _data;
    std::pmr::vector<RDGPhysicalResource<render::Texture, render::TextureState>> _textures{&_data.MemoryResource};
    std::pmr::vector<RDGPhysicalResource<render::Buffer, render::BufferStates>> _buffers{&_data.MemoryResource};
    std::pmr::vector<render::ResourceBarrierDescriptor> _beginBarriers{&_data.MemoryResource};
    std::pmr::vector<render::ResourceBarrierDescriptor> _finalBarriers{&_data.MemoryResource};
    std::pmr::vector<render::ResourceBarrierDescriptor> _releaseBarriers{&_data.MemoryResource};
};

struct RDGExecution {
    struct FrameView {
        RDGTextureHandle Texture;
        render::TextureViewUsage Usage;
        render::TextureView* View;
    };
    RDGData& Data;
    FrameSlot& Slot;
    RDGTransientResourcePool& Pool;
    UploadArena& Uploads;
    Nullable<render::CommandBuffer*> Command{nullptr};
    RDGStateTracker States{Data};
    Nullable<const RDGPass*> Pass{nullptr};
    std::pmr::vector<FrameView> Views{&Data.MemoryResource};
    std::pmr::vector<unique_ptr<render::TextureView>> ViewOwners{&Data.MemoryResource};
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
    return User(data, pass).Accesses;
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

bool SameDescriptor(const render::TextureDescriptor& a, const render::TextureDescriptor& b) noexcept {
    return a.Dim == b.Dim && a.Width == b.Width && a.Height == b.Height && a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels && a.SampleCount == b.SampleCount && a.Format == b.Format && a.Memory == b.Memory && a.Usage == b.Usage && a.Hints == b.Hints;
}

bool SameDescriptor(const render::BufferDescriptor& a, const render::BufferDescriptor& b) noexcept {
    return a.Size == b.Size && a.Memory == b.Memory && a.Usage == b.Usage && a.Hints == b.Hints;
}

bool SameDescriptor(const render::TextureViewDescriptor& a, const render::TextureViewDescriptor& b) noexcept {
    return a.Target == b.Target && a.Dim == b.Dim && a.Format == b.Format && a.Range == b.Range && a.Usage == b.Usage;
}

render::TextureViewUsage DepthUsage(render::TextureState state) noexcept {
    switch (state) {
        case render::TextureState::DepthRead: return render::TextureViewUsage::DepthRead;
        case render::TextureState::DepthWrite: return render::TextureViewUsage::DepthWrite;
        case render::TextureState::DepthReadStencilWrite: return render::TextureViewUsage::DepthReadStencilWrite;
        case render::TextureState::DepthWriteStencilRead: return render::TextureViewUsage::DepthWriteStencilRead;
        default: return render::TextureViewUsage::UNKNOWN;
    }
}

bool IsWriteState(render::TextureState state) noexcept {
    return state == render::TextureState::RenderTarget || state == render::TextureState::DepthWrite ||
           state == render::TextureState::DepthReadStencilWrite || state == render::TextureState::DepthWriteStencilRead ||
           state == render::TextureState::UnorderedAccess || state == render::TextureState::CopyDestination;
}

bool IsWriteState(render::BufferStates state) noexcept {
    return state == render::BufferState::HostWrite || state == render::BufferState::UnorderedAccess || state == render::BufferState::CopyDestination;
}

bool IsReadOnlyBufferState(render::BufferStates state) noexcept {
    constexpr auto kReadOnlyBufferStates = render::BufferState::CopySource | render::BufferState::Vertex | render::BufferState::Index |
                                           render::BufferState::CBuffer | render::BufferState::ShaderRead | render::BufferState::Indirect;
    return state && !(state & ~kReadOnlyBufferStates);
}

bool ValidBufferState(const render::BufferDescriptor& desc, render::BufferStates state) noexcept {
    if (IsReadOnlyBufferState(state)) {
        const auto compatible = [&](render::BufferState bit, render::BufferUse usage) {
            return !state.HasFlag(bit) || desc.Usage.HasFlag(usage);
        };
        return compatible(render::BufferState::CopySource, render::BufferUse::CopySource) &&
               compatible(render::BufferState::Vertex, render::BufferUse::Vertex) &&
               compatible(render::BufferState::Index, render::BufferUse::Index) &&
               compatible(render::BufferState::CBuffer, render::BufferUse::CBuffer) &&
               compatible(render::BufferState::ShaderRead, render::BufferUse::Resource) &&
               compatible(render::BufferState::Indirect, render::BufferUse::Indirect);
    }
    using enum render::BufferState;
    switch (static_cast<render::BufferState>(state)) {
        case Undefined:
        case Common: return true;
        case render::BufferState::CopySource: return desc.Usage.HasFlag(render::BufferUse::CopySource);
        case render::BufferState::CopyDestination: return desc.Usage.HasFlag(render::BufferUse::CopyDestination);
        case render::BufferState::Vertex: return desc.Usage.HasFlag(render::BufferUse::Vertex);
        case render::BufferState::Index: return desc.Usage.HasFlag(render::BufferUse::Index);
        case render::BufferState::CBuffer: return desc.Usage.HasFlag(render::BufferUse::CBuffer);
        case ShaderRead: return desc.Usage.HasFlag(render::BufferUse::Resource);
        case render::BufferState::UnorderedAccess: return desc.Usage.HasFlag(render::BufferUse::UnorderedAccess);
        case render::BufferState::Indirect: return desc.Usage.HasFlag(render::BufferUse::Indirect);
        case HostRead: return desc.Memory == render::MemoryType::ReadBack && desc.Usage.HasFlag(render::BufferUse::MapRead);
        case HostWrite: return desc.Memory == render::MemoryType::Upload && desc.Usage.HasFlag(render::BufferUse::MapWrite);
        default: return false;
    }
}

bool ValidTextureState(const render::TextureDescriptor& desc, render::TextureState state) noexcept {
    using enum render::TextureState;
    switch (state) {
        case Undefined:
        case Common:
        case Present: return true;
        case CopySource: return desc.Usage.HasFlag(render::TextureUse::CopySource);
        case CopyDestination: return desc.Usage.HasFlag(render::TextureUse::CopyDestination);
        case ShaderRead: return desc.Usage.HasFlag(render::TextureUse::Resource);
        case RenderTarget: return desc.Usage.HasFlag(render::TextureUse::RenderTarget) && !render::IsDepthStencilFormat(desc.Format);
        case DepthRead: return desc.Usage.HasFlag(render::TextureUse::DepthStencilRead) && render::IsDepthStencilFormat(desc.Format);
        case DepthWrite: return desc.Usage.HasFlag(render::TextureUse::DepthStencilWrite) && render::IsDepthStencilFormat(desc.Format);
        case DepthReadStencilWrite:
        case DepthWriteStencilRead: return desc.Usage.HasFlag(render::TextureUse::DepthStencilRead | render::TextureUse::DepthStencilWrite) && render::IsDepthStencilFormat(desc.Format);
        case UnorderedAccess: return desc.Usage.HasFlag(render::TextureUse::UnorderedAccess);
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
    for (auto& previous : User(data, pass).Accesses) {
        if (previous.Texture != access.Texture || previous.Index != access.Index) continue;
        if (previous.State != access.State) {
            if (!access.Texture && !previous.Write && !access.Write &&
                IsReadOnlyBufferState(render::BufferStates{previous.State}) && IsReadOnlyBufferState(render::BufferStates{access.State})) {
                access.State |= previous.State;
                if (ValidAccess(data, pass, access)) {
                    previous.State = access.State;
                    return;
                }
            }
            Fail(data, "conflicting whole-resource states in a pass");
            return;
        }
        previous.Read |= access.Read;
        previous.Write |= access.Write;
        previous.Preserve |= access.Preserve;
        return;
    }
    User(data, pass).Accesses.push_back(access);
}

void BufferAccess(RDGData& data, uint32_t pass, RDGBufferHandle buffer, render::BufferStates state, bool read, bool write) {
    AddAccess(data, pass, {false, buffer.Index, state.value(), read, write, write});
}

void TextureAccess(RDGData& data, uint32_t pass, RDGTextureHandle texture, render::TextureState state, bool read, bool write) {
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
    data.UserPasses.push_back(RDGUserPass{.Name = string{name},
                                          .Accesses = std::pmr::vector<RDGAccess>{&data.MemoryResource},
                                          .MeshPasses = std::pmr::vector<MeshPassId>{&data.MemoryResource},
                                          .Colors = std::pmr::vector<RDGColorAttachment>{&data.MemoryResource}});
    AddNode(data, kind, payload);
    return index;
}

bool ValidLoad(render::LoadAction action) noexcept {
    return action == render::LoadAction::Clear || action == render::LoadAction::Load || action == render::LoadAction::DontCare;
}

bool ValidStore(render::StoreAction action) noexcept {
    return action == render::StoreAction::Store || action == render::StoreAction::Discard;
}

void ValidateAttachments(RDGData& data, RDGPass& pass) {
    if (pass.Kind != RDGPassKind::Raster) return;
    if (User(data, pass).Colors.empty() && !User(data, pass).Depth) Fail(data, "raster pass requires an attachment");
    std::optional<array<uint32_t, 4>> shape;
    const auto validate = [&](RDGTextureHandle handle, Nullable<render::TextureView*> view, render::TextureViewUsage usage) {
        const auto& resource = data.Textures[handle.Index];
        const auto& desc = resource.Desc;
        render::SubresourceRange range{0, desc.DepthOrArraySize, 0, 1};
        if (view) {
            const auto v = view->GetDesc();
            if (!resource.Imported || v.Target != resource.Imported.Get() || !view->IsValid() || v.Usage != usage ||
                render::IsDepthStencilFormat(v.Format) != render::IsDepthStencilFormat(desc.Format)) {
                Fail(data, "explicit attachment view does not match the imported resource or usage");
                return;
            }
            range = v.Range;
        }
        const auto normalized = render::NormalizeSubresourceRange(desc, range);
        if (!normalized || normalized->MipLevelCount != 1 || normalized->BaseMipLevel >= 32) {
            Fail(data, "invalid attachment subresource range");
            return;
        }
        const array current{std::max(1u, desc.Width >> normalized->BaseMipLevel), std::max(1u, desc.Height >> normalized->BaseMipLevel), normalized->ArrayLayerCount, desc.SampleCount};
        if (shape && *shape != current) Fail(data, "attachment dimensions, layers and samples must match");
        shape = current;
    };
    for (uint32_t slot = 0; slot < User(data, pass).Colors.size(); ++slot) {
        const auto& color = User(data, pass).Colors[slot];
        if (color.Slot != slot) Fail(data, "color attachment slots must be contiguous from zero");
        if (!ValidLoad(color.Load) || !ValidStore(color.Store)) Fail(data, "invalid color attachment load/store");
        validate(color.Texture, color.View, render::TextureViewUsage::RenderTarget);
    }
    if (User(data, pass).Depth) {
        const auto& depth = *User(data, pass).Depth;
        const auto usage = DepthUsage(depth.State);
        const bool stencil = render::GetTextureFormatAspects(data.Textures[depth.Texture.Index].Desc.Format).HasFlag(render::TextureAspect::Stencil);
        if (!ValidLoad(depth.DepthLoad) || !ValidStore(depth.DepthStore) || !ValidLoad(depth.StencilLoad) || !ValidStore(depth.StencilStore)) Fail(data, "invalid depth/stencil load/store");
        if ((render::IsDepthReadOnly(usage) && (depth.DepthLoad != render::LoadAction::Load || depth.DepthStore != render::StoreAction::Store)) ||
            (stencil && render::IsStencilReadOnly(usage) && (depth.StencilLoad != render::LoadAction::Load || depth.StencilStore != render::StoreAction::Store))) Fail(data, "read-only depth/stencil aspects require Load and Store");
        validate(depth.Texture, depth.View, usage);
    }
    if (shape) {
        User(data, pass).Width = (*shape)[0];
        User(data, pass).Height = (*shape)[1];
        User(data, pass).Layers = (*shape)[2];
    }
}

bool ValidAccess(const RDGData& data, const RDGPass& pass, const RDGAccess& access) noexcept {
    if (access.Texture) {
        const auto state = static_cast<render::TextureState>(access.State);
        if (!ValidTextureState(data.Textures[access.Index].Desc, state)) return false;
        if (pass.Kind == RDGPassKind::Compute && state != render::TextureState::ShaderRead && state != render::TextureState::UnorderedAccess) return false;
        if (pass.Kind == RDGPassKind::Raster && state != render::TextureState::ShaderRead && state != render::TextureState::UnorderedAccess) {
            const bool color = state == render::TextureState::RenderTarget && std::any_of(User(data, pass).Colors.begin(), User(data, pass).Colors.end(), [&](const auto& attachment) { return attachment.Texture.Index == access.Index; });
            const bool depth = User(data, pass).Depth && User(data, pass).Depth->Texture.Index == access.Index && User(data, pass).Depth->State == state;
            if (!color && !depth) return false;
        }
        if (access.Write) {
            return state == render::TextureState::UnorderedAccess || state == render::TextureState::CopyDestination ||
                   state == render::TextureState::RenderTarget || DepthUsage(state) == render::TextureViewUsage::DepthWrite ||
                   state == render::TextureState::DepthReadStencilWrite || state == render::TextureState::DepthWriteStencilRead;
        }
        return state == render::TextureState::ShaderRead || state == render::TextureState::CopySource ||
               state == render::TextureState::DepthRead || state == render::TextureState::UnorderedAccess;
    }
    const auto state = render::BufferStates{access.State};
    if (!ValidBufferState(data.Buffers[access.Index].Desc, state)) return false;
    if (pass.Kind == RDGPassKind::Raster || pass.Kind == RDGPassKind::Compute) {
        if (state == render::BufferState::UnorderedAccess) return true;
        auto allowed = render::BufferState::CBuffer | render::BufferState::ShaderRead | render::BufferState::Indirect;
        if (pass.Kind == RDGPassKind::Raster) allowed |= render::BufferState::Vertex | render::BufferState::Index;
        return !access.Write && state && !(state & ~allowed);
    }
    if (access.Write) return state == render::BufferState::CopyDestination ||
                             (pass.Kind == RDGPassKind::HostWrite && state == render::BufferState::HostWrite);
    return state == render::BufferState::CopySource;
}

render::TextureViewDescriptor ViewDescriptor(render::Texture& texture, render::TextureViewUsage usage) noexcept {
    const auto desc = texture.GetDesc();
    const uint32_t layers = desc.Dim == render::TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize;
    return {&texture, desc.Dim, desc.Format, {0, layers, 0, 1}, usage};
}

render::Texture& ResolveTexture(const RDGExecution& execution, RDGTextureHandle handle) noexcept {
    if (handle.Index >= execution.Data.Textures.size() || !execution.States.GetTexture(handle.Index).Resource) RADRAY_ABORT("RDG: resolving undeclared texture");
    return *execution.States.GetTexture(handle.Index).Resource;
}

RDGBufferSlice ResolveBuffer(const RDGExecution& execution, RDGBufferHandle handle) noexcept {
    if (handle.Index >= execution.Slices.size()) RADRAY_ABORT("RDG: invalid buffer handle");
    return execution.Slices[handle.Index];
}

Nullable<render::TextureView*> ResolveTextureView(const RDGExecution& execution, RDGTextureHandle handle, render::TextureViewUsage usage) noexcept {
    if (execution.Pass) {
        const auto found = std::find_if(Accesses(execution.Data, *execution.Pass).begin(), Accesses(execution.Data, *execution.Pass).end(), [&](const auto& access) {
            if (!access.Texture || access.Index != handle.Index) return false;
            const auto state = static_cast<render::TextureState>(access.State);
            return (usage == render::TextureViewUsage::Resource && state == render::TextureState::ShaderRead) ||
                   (usage == render::TextureViewUsage::UnorderedAccess && state == render::TextureState::UnorderedAccess) ||
                   (usage == render::TextureViewUsage::RenderTarget && state == render::TextureState::RenderTarget) ||
                   (usage != render::TextureViewUsage::UNKNOWN && usage == DepthUsage(state));
        });
        if (found == Accesses(execution.Data, *execution.Pass).end()) return nullptr;
    }
    for (const auto& entry : execution.Views) {
        if (entry.Texture == handle && entry.Usage == usage) return entry.View;
    }
    return nullptr;
}

bool PrepareView(RDGExecution& execution, RDGTextureHandle handle, render::TextureViewUsage usage) {
    if (ResolveTextureView(execution, handle, usage)) return true;
    const auto descriptor = ViewDescriptor(*execution.States.GetTexture(handle.Index).Resource, usage);
    Nullable<render::TextureView*> view;
    if (execution.Data.Textures[handle.Index].Imported) {
        auto owner = execution.Slot.GetDevice().CreateTextureView(descriptor);
        if (!owner) return false;
        view = owner.Get();
        execution.ViewOwners.push_back(owner.Release());
    } else {
        view = execution.Pool.AcquireTextureView(descriptor);
    }
    if (!view) return false;
    execution.Views.push_back({handle, usage, view.Get()});
    return true;
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
        if (User(data, previous).Colors.size() != User(data, candidate).Colors.size() || bool(User(data, previous).Depth) != bool(User(data, candidate).Depth)) return false;
        for (size_t i = 0; i < User(data, previous).Colors.size(); ++i) {
            const auto& a = User(data, previous).Colors[i];
            const auto& b = User(data, candidate).Colors[i];
            if (a.Slot != b.Slot || a.Texture != b.Texture || a.View != b.View || a.Store != render::StoreAction::Store || b.Load != render::LoadAction::Load) return false;
        }
        if (User(data, previous).Depth) {
            const auto& a = *User(data, previous).Depth;
            const auto& b = *User(data, candidate).Depth;
            if (a.Texture != b.Texture || a.View != b.View || a.State != b.State || a.DepthStore != render::StoreAction::Store || b.DepthLoad != render::LoadAction::Load) return false;
            const bool stencil = render::GetTextureFormatAspects(data.Textures[a.Texture.Index].Desc.Format).HasFlag(render::TextureAspect::Stencil);
            if (stencil && (a.StencilStore != render::StoreAction::Store || b.StencilLoad != render::LoadAction::Load)) return false;
        }
    }
    for (const auto& access : Accesses(data, candidate)) {
        const auto& entry = accesses[AccessIndex(data, access)];
        if (entry.Group != data.Groups.size() - 1) continue;
        const auto& old = entry.Access;
        if (old.State != access.State) return false;
        if (candidate.Kind == RDGPassKind::Raster && access.Texture) {
            if (User(data, candidate).Depth && User(data, candidate).Depth->Texture.Index == access.Index && access.State == static_cast<uint32_t>(User(data, candidate).Depth->State)) continue;
            if (access.State == static_cast<uint32_t>(render::TextureState::RenderTarget) &&
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
                                                                        std::pmr::vector<render::ResourceBarrierDescriptor>{&data.MemoryResource},
                                                                        std::pmr::vector<render::RenderPassColorAttachment>{&data.MemoryResource}});
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

render::BufferStates BufferReads(render::BufferUses usage) noexcept {
    render::BufferStates result{};
    if (usage.HasFlag(render::BufferUse::CopySource)) result |= render::BufferState::CopySource;
    if (usage.HasFlag(render::BufferUse::Vertex)) result |= render::BufferState::Vertex;
    if (usage.HasFlag(render::BufferUse::Index)) result |= render::BufferState::Index;
    if (usage.HasFlag(render::BufferUse::CBuffer)) result |= render::BufferState::CBuffer;
    if (usage.HasFlag(render::BufferUse::Resource)) result |= render::BufferState::ShaderRead;
    if (usage.HasFlag(render::BufferUse::Indirect)) result |= render::BufferState::Indirect;
    return result;
}

std::optional<UploadAllocation> AllocateBuffer(UploadArena& arena, const RDGDeviceConstraints& c, uint64_t size,
                                               render::BufferUses usage, uint64_t alignment, render::BufferStates reads) {
    const auto merge = [&](uint64_t value) {
        const auto result = MergeAlignment(alignment, value);
        if (!result) return false;
        alignment = *result;
        return true;
    };
    if (!merge(c.BufferCopyAlignment)) return std::nullopt;
    if (usage.HasFlag(render::BufferUse::Index) && !merge(4)) return std::nullopt;
    if (usage.HasFlag(render::BufferUse::CBuffer) && !merge(c.CBufferOffsetAlignment)) return std::nullopt;
    if (usage.HasFlag(render::BufferUse::Resource) && (!merge(c.StorageOffsetAlignment) || !merge(c.TexelOffsetAlignment))) return std::nullopt;
    if ((c.MaxBufferSize && size > c.MaxBufferSize) ||
        (usage.HasFlag(render::BufferUse::CBuffer) && c.MaxCBufferRange && size > c.MaxCBufferRange) ||
        (usage.HasFlag(render::BufferUse::Resource) && c.MaxStorageRange && size > c.MaxStorageRange)) return std::nullopt;
    const auto reserved = AlignBytes(size, usage.HasFlag(render::BufferUse::CBuffer) ? c.CBufferSizeAlignment : 1);
    if (!reserved) return std::nullopt;
    return arena.Allocate(size, alignment, *reserved, reads ? reads : BufferReads(usage));
}

}  // namespace

namespace detail {

void RDGStateTracker::Initialize() {
    _textures.resize(_data.Textures.size());
    _buffers.resize(_data.Buffers.size());
}

void RDGStateTracker::BindTexture(uint32_t index, render::Texture* texture, render::TextureState initialState) noexcept {
    const auto& ownership = _data.Textures[index].Ownership;
    _textures[index] = {texture, initialState, IsWriteState(initialState), ownership.Acquire, ownership.Release};
}

void RDGStateTracker::BindBuffer(uint32_t index, render::Buffer* buffer, render::BufferStates initialState) noexcept {
    const auto& ownership = _data.Buffers[index].Ownership;
    _buffers[index] = {buffer, initialState, IsWriteState(initialState), ownership.Acquire, ownership.Release};
}

void RDGStateTracker::TransitionTexture(uint32_t index, render::TextureState next, bool write, std::pmr::vector<render::ResourceBarrierDescriptor>& barriers) noexcept {
    auto& current = _textures[index];
    const auto before = current.CurrentState;
    if (current.PendingExternalAcquire) {
        barriers.emplace_back(render::BarrierTextureDescriptor{.Target = current.Resource.Get(), .Before = before, .After = next, .ExternalTransfer = render::ExternalOwnershipTransfer::Acquire});
        current.PendingExternalAcquire = false;
    } else if (before == render::TextureState::UnorderedAccess && next == before && (current.PendingWrite || write)) {
        barriers.emplace_back(render::BarrierUavDescriptor{current.Resource.Get()});
    } else if (before != next || current.PendingWrite) {
        barriers.emplace_back(render::BarrierTextureDescriptor{current.Resource.Get(), before, next});
    }
    current.CurrentState = next;
    current.PendingWrite = write;
}

void RDGStateTracker::TransitionBuffer(uint32_t index, render::BufferStates next, bool write, std::pmr::vector<render::ResourceBarrierDescriptor>& barriers) noexcept {
    auto& current = _buffers[index];
    if (!current.Resource) return;
    const auto& logical = _data.Buffers[index];
    if (!logical.Imported && logical.Desc.Memory == render::MemoryType::Upload) return;
    const auto before = current.CurrentState;
    if (current.PendingExternalAcquire) {
        barriers.emplace_back(render::BarrierBufferDescriptor{.Target = current.Resource.Get(), .Before = before, .After = next, .ExternalTransfer = render::ExternalOwnershipTransfer::Acquire});
        current.PendingExternalAcquire = false;
    } else if (before == render::BufferState::UnorderedAccess && next == before && (current.PendingWrite || write)) {
        barriers.emplace_back(render::BarrierUavDescriptor{current.Resource.Get()});
    } else if (before != next || current.PendingWrite) {
        barriers.emplace_back(render::BarrierBufferDescriptor{current.Resource.Get(), before, next});
    }
    current.CurrentState = next;
    current.PendingWrite = write;
}

void RDGStateTracker::PrepareHostWrite(uint32_t index) noexcept {
    TransitionBuffer(index, render::BufferState::HostWrite, false, _beginBarriers);
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
                group.Uav |= access.Write && access.State == (access.Texture ? static_cast<uint32_t>(render::TextureState::UnorderedAccess) : static_cast<uint32_t>(render::BufferState::UnorderedAccess));
                auto& stamp = seen[AccessIndex(_data, access)];
                if (group.Kind == RDGPassKind::Upload) {
                    if (!preparedUploads[pass.PreparedUpload].ActualSize) continue;
                    // 仅有一个生产者的资源不需要区间索引。
                    if (uploadCounts[AccessIndex(_data, access)] == 1) {
                        if (access.Texture)
                            TransitionTexture(access.Index, static_cast<render::TextureState>(access.State), true, group.Barriers);
                        else
                            TransitionBuffer(access.Index, render::BufferStates{access.State}, true, group.Barriers);
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
                            TransitionTexture(access.Index, static_cast<render::TextureState>(access.State), true, barriers);
                        else
                            TransitionBuffer(access.Index, render::BufferStates{access.State}, true, barriers);
                    }
                    if (overlap) ranges.clear();
                    ranges.emplace(begin, end);
                    stamp = groupIndex;
                } else if (stamp != groupIndex) {
                    stamp = groupIndex;
                    if (access.Texture)
                        TransitionTexture(access.Index, static_cast<render::TextureState>(access.State), access.Write, group.Barriers);
                    else
                        TransitionBuffer(access.Index, render::BufferStates{access.State}, access.Write, group.Barriers);
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
        if (resource.NeedsExternalRelease) _releaseBarriers.emplace_back(render::BarrierTextureDescriptor{.Target = resource.Resource.Get(), .Before = resource.CurrentState, .After = resource.CurrentState, .ExternalTransfer = render::ExternalOwnershipTransfer::Release});
    }
    for (const auto& resource : _buffers) {
        if (resource.NeedsExternalRelease) _releaseBarriers.emplace_back(render::BarrierBufferDescriptor{.Target = resource.Resource.Get(), .Before = resource.CurrentState, .After = resource.CurrentState, .ExternalTransfer = render::ExternalOwnershipTransfer::Release});
    }
}

}  // namespace detail

class RDGTransientResourcePool::Impl {
public:
    struct TextureEntry {
        render::TextureDescriptor Desc;
        unique_ptr<render::Texture> Resource;
        render::TextureState State{render::TextureState::Undefined};
        bool Used{true};
    };
    struct BufferEntry {
        render::BufferDescriptor Desc;
        unique_ptr<render::Buffer> Resource;
        render::BufferStates State;
        bool Used{true};
    };
    struct ViewEntry {
        render::TextureViewDescriptor Desc;
        unique_ptr<render::TextureView> View;
    };
    explicit Impl(render::Device& device) noexcept : Device(device) {}
    render::Device& Device;
    vector<TextureEntry> Textures;
    vector<BufferEntry> Buffers;
    vector<ViewEntry> Views;
};

RDGTransientResourcePool::RDGTransientResourcePool(render::Device& device) noexcept : _impl(make_unique<Impl>(device)) {}
RDGTransientResourcePool::~RDGTransientResourcePool() noexcept = default;

void RDGTransientResourcePool::BeginFrame() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.BeginFrame");
    RADRAY_PROFILE_PLOT("RadRay.RDG.Pool.Textures", static_cast<int64_t>(_impl->Textures.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.Pool.Buffers", static_cast<int64_t>(_impl->Buffers.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.Pool.Views", static_cast<int64_t>(_impl->Views.size()));
    for (auto& entry : _impl->Textures) entry.Used = false;
    for (auto& entry : _impl->Buffers) entry.Used = false;
}

void RDGTransientResourcePool::Clear() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.Clear");
    _impl->Views.clear();
    _impl->Textures.clear();
    _impl->Buffers.clear();
}

RDGTransientTextureAllocation RDGTransientResourcePool::AcquireTexture(const render::TextureDescriptor& desc) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.AcquireTexture");
    for (auto& entry : _impl->Textures) {
        if (!entry.Used && SameDescriptor(entry.Desc, desc)) {
            entry.Used = true;
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

RDGTransientBufferAllocation RDGTransientResourcePool::AcquireBuffer(const render::BufferDescriptor& desc) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.AcquireBuffer");
    for (auto& entry : _impl->Buffers) {
        if (!entry.Used && SameDescriptor(entry.Desc, desc)) {
            entry.Used = true;
            return {entry.Resource.get(), entry.State};
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Pool.CreateBuffer");
        auto resource = _impl->Device.CreateBuffer(desc);
        if (!resource) return {};
        const auto state = desc.Memory == render::MemoryType::Upload ? render::BufferState::HostWrite : render::BufferState::Undefined;
        auto& entry = _impl->Buffers.emplace_back(Impl::BufferEntry{desc, resource.Release(), state});
        return {entry.Resource.get(), entry.State};
    }
}

void RDGTransientResourcePool::CommitTextureState(render::Texture* texture, render::TextureState state) noexcept {
    for (auto& entry : _impl->Textures) {
        if (entry.Resource.get() == texture && entry.Used) {
            entry.State = state;
            return;
        }
    }
    RADRAY_ABORT("RDG: committing texture not borrowed from this pool");
}

void RDGTransientResourcePool::CommitBufferState(render::Buffer* buffer, render::BufferStates state) noexcept {
    for (auto& entry : _impl->Buffers) {
        if (entry.Resource.get() == buffer && entry.Used) {
            entry.State = state;
            return;
        }
    }
    RADRAY_ABORT("RDG: committing buffer not borrowed from this pool");
}

Nullable<render::TextureView*> RDGTransientResourcePool::AcquireTextureView(const render::TextureViewDescriptor& desc) {
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

RDGBuilder::RDGBuilder(FrameSlot& slot, RDGDeviceConstraints constraints, RDGCompileOptions options) noexcept
    : _slot(slot), _data(*slot.Emplace<detail::RDGData>(slot.GetMemoryResource(), slot.GetScratchMemoryResource())) {
    _data.Constraints = constraints;
    _data.Options = options;
}

RDGTextureHandle RDGBuilder::ImportTexture(render::Texture* texture, render::TextureState initialState, std::optional<render::TextureState> finalState, RDGExternalOwnership ownership) {
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

RDGBufferHandle RDGBuilder::ImportBuffer(render::Buffer* buffer, render::BufferStates initialState, std::optional<render::BufferStates> finalState, RDGExternalOwnership ownership) {
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

RDGBufferHandle RDGBuilder::ImportPersistentBorrowed(GpuBuffer& buffer) {
    const auto handle = ImportBuffer(buffer.GetBuffer(), buffer.GetSubmittedState(), render::BufferState::Common);
    auto& resource = _data.Buffers[handle.Index];
    if (resource.Persistent && resource.Persistent.Get() != &buffer) Fail(_data, "duplicate buffer wrapper");
    resource.Persistent = &buffer;
    return handle;
}
RDGTextureHandle RDGBuilder::ImportPersistentBorrowed(GpuTexture& texture) {
    const auto handle = ImportTexture(texture.GetTexture(), texture.GetSubmittedState(), render::TextureState::ShaderRead);
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
    const auto* generated = std::get_if<GeneratedSource>(&_storage);
    return generated && generated->Layout ? generated->Layout.Get()(generated->Context.get(), context, _maxSize) : _maxSize;
}
bool RDGUploadSource::Write(RDGUploadWriteContext& context, uint64_t sourceRowPitch) const noexcept {
    if (const auto* generated = std::get_if<GeneratedSource>(&_storage)) return generated->Write.Get()(generated->Context.get(), context);
    const auto* owned = std::get_if<vector<byte>>(&_storage);
    const auto bytes = owned ? std::span<const byte>{*owned} : std::get<std::span<const byte>>(_storage);
    if (context.RowBytes) {
        const auto pitch = sourceRowPitch ? sourceRowPitch : context.RowBytes;
        for (uint32_t row = 0; row < context.Rows; ++row) {
            const auto sourceOffset = row * pitch, targetOffset = row * context.RowPitch;
            if (sourceOffset > bytes.size() || context.RowBytes > bytes.size() - sourceOffset ||
                targetOffset > context.Bytes.size() || context.RowBytes > context.Bytes.size() - targetOffset) return false;
            std::memcpy(context.Bytes.data() + targetOffset, bytes.data() + sourceOffset, static_cast<size_t>(context.RowBytes));
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
RDGTextureUploadSource RDGTextureUploadSource::BorrowedBytes(std::span<const byte> bytes, uint64_t pitch) noexcept { return {RDGUploadSource::BorrowedBytes(bytes), pitch}; }
RDGTextureUploadSource RDGTextureUploadSource::OwnedBytes(vector<byte>&& bytes, uint64_t pitch) noexcept { return {RDGUploadSource::OwnedBytes(std::move(bytes)), pitch}; }
RDGTextureUploadSource RDGTextureUploadSource::SnapshotBytes(std::span<const byte> bytes, uint64_t pitch) { return {RDGUploadSource::SnapshotBytes(bytes), pitch}; }

RDGTextureHandle RDGBuilder::CreateTexture(const render::TextureDescriptor& desc) {
    RequireOpen(_data);
    const auto index = static_cast<uint32_t>(_data.Textures.size());
    _data.Textures.push_back({.Desc = desc, .Readers = std::pmr::vector<uint32_t>{&_data.MemoryResource}});
    return {index};
}

RDGBufferHandle RDGBuilder::CreateBuffer(const render::BufferDescriptor& desc) {
    RequireOpen(_data);
    const auto index = static_cast<uint32_t>(_data.Buffers.size());
    _data.Buffers.push_back({.Desc = desc, .Readers = std::pmr::vector<uint32_t>{&_data.MemoryResource}});
    return {index};
}

RDGBufferHandle RDGBuilder::CreateBuffer(const RDGDynamicBufferDescriptor& desc, RDGUploadSource source) {
    const auto handle = CreateBuffer(render::BufferDescriptor{.Size = desc.MaxSize, .Memory = render::MemoryType::Upload, .Usage = desc.Usage | render::BufferUse::MapWrite});
    auto& resource = _data.Buffers[handle.Index];
    resource.Dynamic = true;
    resource.Alignment = desc.Alignment;
    UploadBuffer(handle, 0, std::move(source));
    return handle;
}

void RDGBuilder::UploadBuffer(RDGBufferHandle dst, uint64_t offset, RDGUploadSource source) {
    RequireOpen(_data);
    if (dst.Index >= _data.Buffers.size()) {
        Fail(_data, "invalid upload destination");
        return;
    }
    const auto& resource = _data.Buffers[dst.Index];
    const auto& desc = resource.Desc;
    if (offset > desc.Size || source.GetMaxSize() > desc.Size - offset || desc.Memory == render::MemoryType::ReadBack ||
        (desc.Memory == render::MemoryType::Device && (!desc.Usage.HasFlag(render::BufferUse::CopyDestination) || offset % std::max(uint64_t{1}, _data.Constraints.BufferCopyAlignment))) ||
        (desc.Memory == render::MemoryType::Upload && !desc.Usage.HasFlag(render::BufferUse::MapWrite))) {
        Fail(_data, "invalid upload range, memory or usage");
        return;
    }
    if (!source.GetMaxSize() && !resource.Dynamic) return;
    const bool host = desc.Memory == render::MemoryType::Upload;
    const auto payload = static_cast<uint32_t>(_data.Uploads.size());
    _data.Uploads.push_back({std::move(source), {false, dst.Index, static_cast<uint32_t>(host ? render::BufferState::HostWrite : render::BufferState::CopyDestination), false, true, true}, offset});
    AddNode(_data, host ? RDGPassKind::HostWrite : RDGPassKind::Upload, payload);
}

void RDGBuilder::UploadTexture(RDGTextureHandle dst, const render::TextureRegion2D& region, RDGTextureUploadSource source) {
    RequireOpen(_data);
    if (dst.Index >= _data.Textures.size()) {
        Fail(_data, "invalid texture upload destination");
        return;
    }
    const auto& desc = _data.Textures[dst.Index].Desc;
    const auto pixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (!pixel || render::IsDepthStencilFormat(desc.Format) || desc.SampleCount != 1 ||
        (desc.Dim != render::TextureDimension::Dim2D && desc.Dim != render::TextureDimension::Dim2DArray) ||
        region.MipLevel >= desc.MipLevels || region.MipLevel >= 32 || region.ArrayLayer >= desc.DepthOrArraySize || !region.Width || !region.Height ||
        !desc.Usage.HasFlag(render::TextureUse::CopyDestination)) {
        Fail(_data, "invalid texture upload region or format");
        return;
    }
    const auto width = std::max(1u, desc.Width >> region.MipLevel), height = std::max(1u, desc.Height >> region.MipLevel);
    const uint64_t rowBytes = uint64_t{region.Width} * pixel;
    const auto pitch = source.SourceRowPitch ? source.SourceRowPitch : rowBytes;
    if (region.X > width || region.Width > width - region.X || region.Y > height || region.Height > height - region.Y ||
        pitch < rowBytes || (region.Height > 1 && pitch > (std::numeric_limits<uint64_t>::max() - rowBytes) / (region.Height - 1)) ||
        source.Source.GetMaxSize() < pitch * (region.Height - 1) + rowBytes) {
        Fail(_data, "texture source does not cover the region");
        return;
    }
    const auto payload = static_cast<uint32_t>(_data.Uploads.size());
    _data.Uploads.push_back({std::move(source.Source), {true, dst.Index, static_cast<uint32_t>(render::TextureState::CopyDestination), false, true, true}, 0, pitch, region});
    AddNode(_data, RDGPassKind::Upload, payload);
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

uint32_t RDGBuilder::AddRasterPassImpl(std::string_view name, shared_ptr<void> callback, bool (*execute)(const void*, RDGRasterPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept) {
    const auto index = AddPass(_data, name, RDGPassKind::Raster);
    User(_data, _data.Passes[index]).Callback = std::move(callback);
    User(_data, _data.Passes[index]).RasterExecute = execute;
    User(_data, _data.Passes[index]).Prepare = prepare;
    return index;
}

uint32_t RDGBuilder::AddComputePassImpl(std::string_view name, shared_ptr<void> callback, bool (*execute)(const void*, RDGComputePassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept) {
    const auto index = AddPass(_data, name, RDGPassKind::Compute);
    User(_data, _data.Passes[index]).Callback = std::move(callback);
    User(_data, _data.Passes[index]).ComputeExecute = execute;
    User(_data, _data.Passes[index]).Prepare = prepare;
    return index;
}

uint32_t RDGBuilder::AddCopyPassImpl(std::string_view name, shared_ptr<void> callback, bool (*execute)(const void*, RDGCopyPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept) {
    const auto index = AddPass(_data, name, RDGPassKind::Copy);
    User(_data, _data.Passes[index]).Callback = std::move(callback);
    User(_data, _data.Passes[index]).CopyExecute = execute;
    User(_data, _data.Passes[index]).Prepare = prepare;
    return index;
}

RDGRasterPassBuilder::RDGRasterPassBuilder(detail::RDGData& data, uint32_t pass) noexcept : _data(data), _pass(pass) {}
RDGComputePassBuilder::RDGComputePassBuilder(detail::RDGData& data, uint32_t pass) noexcept : _data(data), _pass(pass) {}
RDGCopyPassBuilder::RDGCopyPassBuilder(detail::RDGData& data, uint32_t pass) noexcept : _data(data), _pass(pass) {}

RDGRasterPassBuilder& RDGRasterPassBuilder::UseMeshPass(MeshPassId pass) {
    RequireOpen(_data);
    if (pass.Value == std::numeric_limits<uint32_t>::max()) {
        Fail(_data, "invalid MeshPassId");
        return *this;
    }
    User(_data, _data.Passes[_pass]).MeshPasses.push_back(pass);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteColor(uint32_t slot, RDGTextureHandle texture, render::LoadAction load, render::StoreAction store,
                                                       render::ColorClearValue clear, Nullable<render::TextureView*> explicitView) {
    RequireOpen(_data);
    auto& colors = User(_data, _data.Passes[_pass]).Colors;
    for (const auto& color : colors) {
        if (color.Slot == slot || color.Texture == texture) {
            Fail(_data, "duplicate color attachment");
            return *this;
        }
    }
    colors.push_back({slot, texture, load, store, clear, explicitView});
    AddAccess(_data, _pass, {true, texture.Index, static_cast<uint32_t>(render::TextureState::RenderTarget), load == render::LoadAction::Load, true, load == render::LoadAction::Load});
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::SetDepthStencil(RDGTextureHandle texture, render::TextureState state,
                                                            render::LoadAction depthLoad, render::StoreAction depthStore,
                                                            render::LoadAction stencilLoad, render::StoreAction stencilStore,
                                                            render::DepthStencilClearValue clear, Nullable<render::TextureView*> explicitView) {
    RequireOpen(_data);
    auto& pass = _data.Passes[_pass];
    if (User(_data, pass).Depth || DepthUsage(state) == render::TextureViewUsage::UNKNOWN) {
        Fail(_data, "invalid or duplicate depth/stencil attachment");
        return *this;
    }
    User(_data, pass).Depth = {texture, state, depthLoad, depthStore, stencilLoad, stencilStore, clear, explicitView};
    const bool load = depthLoad == render::LoadAction::Load || stencilLoad == render::LoadAction::Load;
    const bool write = state != render::TextureState::DepthRead;
    AddAccess(_data, _pass, {true, texture.Index, static_cast<uint32_t>(state), load || !write, write, load});
    return *this;
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
            if (!d.Width || !d.Height || !d.DepthOrArraySize || !d.MipLevels || !d.SampleCount || d.Dim == render::TextureDimension::UNKNOWN || d.Format == render::TextureFormat::UNKNOWN ||
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
                if (!access.Texture && _data.Buffers[access.Index].Desc.Memory == render::MemoryType::Upload &&
                    access.Write && pass.Kind != RDGPassKind::HostWrite) Fail(_data, "Upload buffers only accept Prepare writes");
                auto& lastWriter = access.Texture ? _data.Textures[access.Index].LastWriter : _data.Buffers[access.Index].LastWriter;
                const bool imported = access.Texture ? bool(_data.Textures[access.Index].Imported) : bool(_data.Buffers[access.Index].Imported);
                const bool undefined = access.Texture ? _data.Textures[access.Index].InitialState == render::TextureState::Undefined : _data.Buffers[access.Index].InitialState == render::BufferState::Undefined;
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
                if (!access.Texture && _data.Buffers[access.Index].Desc.Memory == render::MemoryType::Upload) {
                    if (pass.Kind == RDGPassKind::HostWrite) {
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
    for (auto& pass : _data.UserPasses) pass.Callback.reset();
}
std::span<const MeshPassId> CompiledRDG::GetAliveMeshPasses() const noexcept { return _data.AliveMeshPasses; }

bool CompiledRDG::Prepare(FrameSlot& slot, RDGTransientResourcePool& pool, UploadArena& uploads) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Prepare");
    if (_prepareAttempted) return false;
    _prepareAttempted = true;
    if (&slot.GetMemoryResource() != &_data.MemoryResource) return false;
    _execution.reset(std::pmr::polymorphic_allocator<>{&_data.MemoryResource}.new_object<RDGExecution>(RDGExecution{_data, slot, pool, uploads}));
    auto& execution = *_execution;
    const auto detail = slot.GetDevice().GetDetail();
    const auto& limits = slot.GetDevice().GetCapabilities().Limits;
    const RDGDeviceConstraints constraints{detail.BufferCopyOffsetAlignment, detail.TextureDataPlacementAlignment, detail.TextureDataPitchAlignment,
                                           detail.CBufferAlignment, limits.CBufferOffsetAlignment, limits.StorageBufferOffsetAlignment, limits.TexelBufferOffsetAlignment,
                                           limits.MaxBufferSize, limits.MaxUniformBufferRange, limits.MaxStorageBufferRange};
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
            execution.PreparedUploads.push_back({.Barriers = std::pmr::vector<render::ResourceBarrierDescriptor>{&_data.MemoryResource}});
            Prepared(execution, pass).ActualSize = Upload(_data, pass).Source.PrepareLayout(layout);
            if (Prepared(execution, pass).ActualSize > Upload(_data, pass).Source.GetMaxSize()) return false;
            const auto& access = Accesses(_data, pass).front();
            if (access.Texture) {
                if (Prepared(execution, pass).ActualSize != Upload(_data, pass).Source.GetMaxSize()) return false;
                // 来源覆盖量包含行 padding；实际有效量只计算像素。
                Prepared(execution, pass).ActualSize = uint64_t{Upload(_data, pass).Region.Width} * Upload(_data, pass).Region.Height * render::GetTextureFormatBytesPerPixel(_data.Textures[access.Index].Desc.Format);
            }
            if (!access.Texture) {
                const auto& resource = _data.Buffers[access.Index];
                if (resource.Dynamic)
                    sizes[access.Index] = Prepared(execution, pass).ActualSize;
                else if (!Prepared(execution, pass).ActualSize && (!resource.Imported || resource.InitialState == render::BufferState::Undefined))
                    return false;
                if (Upload(_data, pass).UploadOffset > sizes[access.Index] || Prepared(execution, pass).ActualSize > sizes[access.Index] - Upload(_data, pass).UploadOffset) return false;
                if (pass.Kind == RDGPassKind::Upload && Upload(_data, pass).UploadOffset % std::max(uint64_t{1}, constraints.BufferCopyAlignment)) return false;
            }
        }
    }
    execution.States.Initialize();
    execution.Slices.resize(_data.Buffers.size());
    std::pmr::vector<UploadAllocation> direct(_data.Buffers.size(), &_data.ScratchResource);
    std::pmr::vector<render::BufferStates> reads(_data.Buffers.size(), &_data.ScratchResource);
    std::pmr::vector<uint32_t> uploadCounts(_data.Textures.size() + _data.Buffers.size(), 0, &_data.ScratchResource);
    for (const auto index : _data.ScheduledPasses) {
        for (const auto& access : Accesses(_data, _data.Passes[index])) {
            if (!access.Texture && !access.Write) reads[access.Index] |= render::BufferStates{access.State};
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
            if (!resource.Imported && resource.Desc.Memory == render::MemoryType::Upload) {
                auto allocation = AllocateBuffer(uploads, constraints, sizes[i], resource.Desc.Usage, resource.Alignment, reads[i]);
                if (!allocation) return false;
                direct[i] = *allocation;
                execution.Slices[i] = {allocation->Buffer, allocation->Offset, sizes[i]};
                execution.States.BindBuffer(static_cast<uint32_t>(i), allocation->Buffer.Get(), reads[i]);
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
            const auto state = static_cast<render::TextureState>(access.State);
            auto usage = state == render::TextureState::ShaderRead ? render::TextureViewUsage::Resource : state == render::TextureState::UnorderedAccess ? render::TextureViewUsage::UnorderedAccess
                                                                                                      : state == render::TextureState::RenderTarget      ? render::TextureViewUsage::RenderTarget
                                                                                                                                                         : DepthUsage(state);
            if (usage == render::TextureViewUsage::UNKNOWN) continue;
            const bool explicitView = pass.Kind == RDGPassKind::Raster && (std::any_of(User(_data, pass).Colors.begin(), User(_data, pass).Colors.end(), [&](const auto& a) { return a.Texture.Index == access.Index && bool(a.View); }) ||
                                                                           (User(_data, pass).Depth && User(_data, pass).Depth->Texture.Index == access.Index && User(_data, pass).Depth->View));
            if (!explicitView && !PrepareView(execution, {access.Index}, usage)) return false;
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
            uint32_t rows = 1;
            if (access.Texture) {
                rowBytes = uint64_t{Upload(_data, pass).Region.Width} * render::GetTextureFormatBytesPerPixel(_data.Textures[access.Index].Desc.Format);
                rows = Upload(_data, pass).Region.Height;
                const auto pixel = render::GetTextureFormatBytesPerPixel(_data.Textures[access.Index].Desc.Format);
                const auto pitchAlignment = MergeAlignment(constraints.TexturePitchAlignment, pixel);
                const auto placement = MergeAlignment(constraints.TexturePlacementAlignment, pixel);
                if (!pitchAlignment || !placement) return false;
                const auto pitch = AlignBytes(rowBytes, *pitchAlignment);
                if (!pitch || *pitch > std::numeric_limits<uint32_t>::max() || (rows > 1 && *pitch > (std::numeric_limits<uint64_t>::max() - rowBytes) / (rows - 1))) return false;
                Prepared(execution, pass).StagingPitch = static_cast<uint32_t>(*pitch);
                mappedSize = *pitch * (rows - 1) + rowBytes;
                const auto allocation = uploads.Allocate(mappedSize, *placement, mappedSize, render::BufferState::CopySource);
                if (!allocation) return false;
                Prepared(execution, pass).Staging = *allocation;
            } else if (pass.Kind == RDGPassKind::Upload) {
                const auto allocation = AllocateBuffer(uploads, constraints, mappedSize, render::BufferUse::CopySource, 1, render::BufferState::CopySource);
                if (!allocation) return false;
                Prepared(execution, pass).Staging = *allocation;
            } else if (direct[access.Index].Buffer) {
                Prepared(execution, pass).Staging = direct[access.Index];
                Prepared(execution, pass).Staging.Offset += Upload(_data, pass).UploadOffset;
                Prepared(execution, pass).Staging.Data = Prepared(execution, pass).Staging.Data.Get() + Upload(_data, pass).UploadOffset;
                Prepared(execution, pass).Staging.Size = mappedSize;
            } else {
                auto& physical = execution.States.GetBuffer(access.Index);
                Nullable<byte*> mapped = static_cast<byte*>(physical.Resource->Map(Upload(_data, pass).UploadOffset, mappedSize));
                if (!mapped) return false;
                Prepared(execution, pass).Staging = {physical.Resource, mapped, Upload(_data, pass).UploadOffset, mappedSize, mappedSize};
            }
            RDGUploadWriteContext write{{Prepared(execution, pass).Staging.Data.Get(), static_cast<size_t>(mappedSize)}, Prepared(execution, pass).ActualSize, Prepared(execution, pass).StagingPitch, rowBytes, rows};
            const bool written = Upload(_data, pass).Source.Write(write, Upload(_data, pass).SourceRowPitch);
            if (pass.Kind == RDGPassKind::HostWrite && _data.Buffers[access.Index].Imported) {
                auto& physical = execution.States.GetBuffer(access.Index);
                if (written) physical.Resource->FlushMappedRange({Upload(_data, pass).UploadOffset, mappedSize});
                physical.Resource->Unmap();
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
            if (!User(_data, pass).Prepare.Get()(User(_data, pass).Callback.get(), context)) return false;
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
                const auto view = a.View ? a.View : ResolveTextureView(execution, a.Texture, render::TextureViewUsage::RenderTarget);
                if (!view) return false;
                group.Colors.push_back({view.Get(), a.Load, User(_data, last).Colors[i].Store, a.Clear});
            }
            if (User(_data, first).Depth) {
                const auto& a = *User(_data, first).Depth;
                const auto view = a.View ? a.View : ResolveTextureView(execution, a.Texture, DepthUsage(a.State));
                if (!view) return false;
                group.Depth = {view.Get(), a.DepthLoad, User(_data, last).Depth->DepthStore, a.StencilLoad, User(_data, last).Depth->StencilStore, a.Clear};
            }
        }
    }
    execution.Pass = nullptr;
    _prepared = true;
    return true;
}

bool CompiledRDG::Record(render::CommandBuffer& command) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Record");
    if (!_prepared || _recordAttempted) return false;
    _recordAttempted = true;
    auto& execution = *_execution;
    execution.Command = &command;
    if (!execution.States.GetBeginBarriers().empty()) command.ResourceBarrier(execution.States.GetBeginBarriers());
    for (const auto& group : _data.Groups) {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.RecordGroup");
        RADRAY_PROFILE_PLOT("RadRay.RDG.GroupBarriers", static_cast<int64_t>(group.Barriers.size()));
        if (!group.Barriers.empty()) command.ResourceBarrier(group.Barriers);
        const auto& first = _data.Passes[group.Passes.front()];
        bool result = true;
        if (group.Kind == RDGPassKind::Upload || group.Kind == RDGPassKind::Copy) {
            command.PushDebugGroup(group.Kind == RDGPassKind::Upload ? "RDG UploadBatch" : PassName(_data, first));
            for (const auto index : group.Passes) {
                const auto& pass = _data.Passes[index];
                execution.Pass = &pass;
                RADRAY_PROFILE_SCOPE_DYN(PassName(_data, pass));
                if (group.Kind == RDGPassKind::Copy) {
                    RDGCopyPassContext context{execution};
                    result = User(_data, pass).CopyExecute.Get()(User(_data, pass).Callback.get(), context);
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
                result = User(_data, pass).ComputeExecute.Get()(User(_data, pass).Callback.get(), context);
                command.PopDebugGroup();
                if (!result) break;
            }
            command.EndComputePass(encoder.Release());
        } else {
            auto encoder = command.BeginRenderPass({.Colors = group.Colors, .DepthStencil = group.Depth, .Width = User(_data, first).Width, .Height = User(_data, first).Height, .Layers = User(_data, first).Layers, .Name = PassName(_data, first), .AllowUavWrites = group.Uav});
            if (!encoder) return false;
            RDGRasterPassContext context{execution, *encoder};
            for (const auto index : group.Passes) {
                const auto& pass = _data.Passes[index];
                execution.Pass = &pass;
                RADRAY_PROFILE_SCOPE_DYN(PassName(_data, pass));
                command.PushDebugGroup(PassName(_data, pass));
                encoder->SetStencilReference(0);
                result = User(_data, pass).RasterExecute.Get()(User(_data, pass).Callback.get(), context);
                command.PopDebugGroup();
                if (!result) break;
            }
            command.EndRenderPass(encoder.Release());
        }
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
        const auto& physical = execution.States.GetTexture(i);
        if (resource.Persistent) resource.Persistent->CommitSubmittedState(physical.CurrentState);
        if (!resource.Imported) execution.Pool.CommitTextureState(physical.Resource.Get(), physical.CurrentState);
    }
    for (size_t i = 0; i < _data.Buffers.size(); ++i) {
        auto& resource = _data.Buffers[i];
        if (!resource.Alive) continue;
        const auto& physical = execution.States.GetBuffer(i);
        if (resource.Persistent) resource.Persistent->CommitSubmittedState(physical.CurrentState);
        if (!resource.Imported && resource.Desc.Memory != render::MemoryType::Upload) execution.Pool.CommitBufferState(physical.Resource.Get(), physical.CurrentState);
    }
    execution.Uploads.CommitSubmittedStates();
}

RDGPrepareContext::RDGPrepareContext(detail::RDGExecution& execution) noexcept : _execution(execution) {}
FrameSlot& RDGPrepareContext::GetSlot() noexcept { return _execution.Slot; }
RDGBufferSlice RDGPrepareContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }
render::Texture& RDGPrepareContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
Nullable<render::TextureView*> RDGPrepareContext::ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }

RDGRasterPassContext::RDGRasterPassContext(detail::RDGExecution& execution, render::GraphicsCommandEncoder& encoder) noexcept : _execution(execution), _encoder(encoder) {}
render::GraphicsCommandEncoder& RDGRasterPassContext::GetEncoder() noexcept { return _encoder; }
RDGComputePassContext::RDGComputePassContext(detail::RDGExecution& execution, render::ComputeCommandEncoder& encoder) noexcept : _execution(execution), _encoder(encoder) {}
render::ComputeCommandEncoder& RDGComputePassContext::GetEncoder() noexcept { return _encoder; }
RDGCopyPassContext::RDGCopyPassContext(detail::RDGExecution& execution) noexcept : _execution(execution) {}
render::CommandBuffer& RDGCopyPassContext::GetCommandBuffer() noexcept { return *_execution.Command; }
uint64_t RDGRasterPassContext::GetFrameId() const noexcept { return _execution.Slot.GetFrameId(); }
uint32_t RDGRasterPassContext::GetSlotIndex() const noexcept { return _execution.Slot.GetIndex(); }
std::span<const VisibleMeshDrawCommand> RDGRasterPassContext::GetVisibleCommands(MeshPassId pass) const noexcept { return std::as_const(_execution.Slot).GetPassDrawList(pass).VisibleCommands; }
render::Texture& RDGRasterPassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
RDGBufferSlice RDGRasterPassContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }
Nullable<render::TextureView*> RDGRasterPassContext::ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }
uint64_t RDGComputePassContext::GetFrameId() const noexcept { return _execution.Slot.GetFrameId(); }
uint32_t RDGComputePassContext::GetSlotIndex() const noexcept { return _execution.Slot.GetIndex(); }
std::span<const VisibleMeshDrawCommand> RDGComputePassContext::GetVisibleCommands(MeshPassId pass) const noexcept { return std::as_const(_execution.Slot).GetPassDrawList(pass).VisibleCommands; }
render::Texture& RDGComputePassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
RDGBufferSlice RDGComputePassContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }
Nullable<render::TextureView*> RDGComputePassContext::ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }
uint64_t RDGCopyPassContext::GetFrameId() const noexcept { return _execution.Slot.GetFrameId(); }
uint32_t RDGCopyPassContext::GetSlotIndex() const noexcept { return _execution.Slot.GetIndex(); }
std::span<const VisibleMeshDrawCommand> RDGCopyPassContext::GetVisibleCommands(MeshPassId pass) const noexcept { return std::as_const(_execution.Slot).GetPassDrawList(pass).VisibleCommands; }
render::Texture& RDGCopyPassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
RDGBufferSlice RDGCopyPassContext::ResolveBuffer(RDGBufferHandle handle) const noexcept { return radray::ResolveBuffer(_execution, handle); }

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadBuffer(RDGBufferHandle handle, render::BufferStates state) {
    BufferAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteBuffer(RDGBufferHandle handle, render::BufferState state) {
    BufferAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadWriteBuffer(RDGBufferHandle handle, render::BufferState state) {
    BufferAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadTexture(RDGTextureHandle handle, render::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteTexture(RDGTextureHandle handle, render::TextureState state) {
    TextureAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadWriteTexture(RDGTextureHandle handle, render::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::NeverCull() noexcept {
    RequireOpen(_data);
    _data.Passes[_pass].NeverCull = true;
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadBuffer(RDGBufferHandle handle, render::BufferStates state) {
    BufferAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::WriteBuffer(RDGBufferHandle handle, render::BufferState state) {
    BufferAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadWriteBuffer(RDGBufferHandle handle, render::BufferState state) {
    BufferAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadTexture(RDGTextureHandle handle, render::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, false);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::WriteTexture(RDGTextureHandle handle, render::TextureState state) {
    TextureAccess(_data, _pass, handle, state, false, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::ReadWriteTexture(RDGTextureHandle handle, render::TextureState state) {
    TextureAccess(_data, _pass, handle, state, true, true);
    return *this;
}

RDGComputePassBuilder& RDGComputePassBuilder::NeverCull() noexcept {
    RequireOpen(_data);
    _data.Passes[_pass].NeverCull = true;
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::ReadBuffer(RDGBufferHandle handle) {
    BufferAccess(_data, _pass, handle, render::BufferState::CopySource, true, false);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::WriteBuffer(RDGBufferHandle handle) {
    BufferAccess(_data, _pass, handle, render::BufferState::CopyDestination, false, true);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::ReadTexture(RDGTextureHandle handle) {
    TextureAccess(_data, _pass, handle, render::TextureState::CopySource, true, false);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::WriteTexture(RDGTextureHandle handle) {
    TextureAccess(_data, _pass, handle, render::TextureState::CopyDestination, false, true);
    return *this;
}

RDGCopyPassBuilder& RDGCopyPassBuilder::NeverCull() noexcept {
    RequireOpen(_data);
    _data.Passes[_pass].NeverCull = true;
    return *this;
}

}  // namespace radray
