#include <radray/runtime/rdg.h>

#include <algorithm>
#include <cstring>
#include <limits>

#include <radray/logger.h>
#include <radray/profiler.h>

namespace radray {
namespace detail {

enum class RDGPassKind { Raster,
                         Compute,
                         Copy,
                         Upload };

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

struct RDGPass {
    RDGPassKind Kind;
    string Name;
    vector<RDGAccess> Accesses{};
    vector<MeshPassId> MeshPasses{};
    vector<uint32_t> DataDependencies{};
    vector<uint32_t> OrderDependencies{};
    vector<RDGColorAttachment> Colors{};
    std::optional<RDGDepthAttachment> Depth{};
    Nullable<void*> Callback{nullptr};
    Nullable<bool (*)(void*, RDGRasterPassContext&)> RasterExecute{nullptr};
    Nullable<bool (*)(void*, RDGComputePassContext&)> ComputeExecute{nullptr};
    Nullable<bool (*)(void*, RDGCopyPassContext&)> CopyExecute{nullptr};
    std::span<const byte> UploadBytes{};
    uint64_t UploadOffset{0};
    uint32_t Width{1}, Height{1}, Layers{1};
    bool NeverCull{false};
    bool Alive{false};
};

struct RDGTexture {
    render::TextureDescriptor Desc;
    Nullable<render::Texture*> Imported{nullptr};
    render::TextureState InitialState{render::TextureState::Undefined};
    std::optional<render::TextureState> FinalState{};
    std::optional<uint32_t> LastWriter{};
    vector<uint32_t> Readers{};
    bool Output{false};
    bool Alive{false};
};

struct RDGBuffer {
    render::BufferDescriptor Desc;
    Nullable<render::Buffer*> Imported{nullptr};
    render::BufferState InitialState{render::BufferState::Undefined};
    std::optional<render::BufferState> FinalState{};
    std::optional<uint32_t> LastWriter{};
    vector<uint32_t> Readers{};
    bool Output{false};
    bool Alive{false};
};

struct RDGExecutionGroup {
    RDGPassKind Kind;
    vector<uint32_t> Passes;
};

struct RDGData {
    vector<RDGTexture> Textures;
    vector<RDGBuffer> Buffers;
    vector<RDGPass> Passes;
    vector<uint32_t> ScheduledPasses;
    vector<RDGExecutionGroup> Groups;
    vector<MeshPassId> AliveMeshPasses;
    bool Failed{false};
    bool Sealed{false};
};

template <class T, class State>
struct RDGPhysicalResource {
    Nullable<T*> Resource{nullptr};
    State CurrentState;
    bool PendingWrite{false};
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
    render::CommandBuffer& Command;
    vector<RDGPhysicalResource<render::Texture, render::TextureState>> Textures;
    vector<RDGPhysicalResource<render::Buffer, render::BufferState>> Buffers;
    Nullable<const RDGPass*> Pass{nullptr};
    vector<FrameView> ImportedViews{};
};

}  // namespace detail

namespace {

using namespace detail;

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

bool IsWriteState(render::BufferState state) noexcept {
    return state == render::BufferState::HostWrite || state == render::BufferState::UnorderedAccess || state == render::BufferState::CopyDestination;
}

bool ValidBufferState(const render::BufferDescriptor& desc, render::BufferState state) noexcept {
    using enum render::BufferState;
    switch (state) {
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

void AddAccess(RDGData& data, uint32_t passIndex, RDGAccess access) {
    RequireOpen(data);
    if ((access.Texture && access.Index >= data.Textures.size()) || (!access.Texture && access.Index >= data.Buffers.size())) {
        Fail(data, "invalid resource handle");
        return;
    }
    auto& pass = data.Passes[passIndex];
    for (auto& previous : pass.Accesses) {
        if (previous.Texture != access.Texture || previous.Index != access.Index) continue;
        if (previous.State != access.State) {
            Fail(data, "conflicting whole-resource states in a pass");
            return;
        }
        previous.Read |= access.Read;
        previous.Write |= access.Write;
        previous.Preserve |= access.Preserve;
        return;
    }
    pass.Accesses.push_back(access);
}

void BufferAccess(RDGData& data, uint32_t pass, RDGBufferHandle buffer, render::BufferState state, bool read, bool write) {
    AddAccess(data, pass, {false, buffer.Index, static_cast<uint32_t>(state), read, write, write});
}

void TextureAccess(RDGData& data, uint32_t pass, RDGTextureHandle texture, render::TextureState state, bool read, bool write) {
    AddAccess(data, pass, {true, texture.Index, static_cast<uint32_t>(state), read, write, write});
}

uint32_t AddPass(RDGData& data, std::string_view name, RDGPassKind kind) {
    RequireOpen(data);
    if (data.Passes.size() >= std::numeric_limits<uint32_t>::max()) RADRAY_ABORT("RDG: too many passes");
    const auto index = static_cast<uint32_t>(data.Passes.size());
    data.Passes.push_back(RDGPass{.Kind = kind, .Name = string{name}});
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
    if (pass.Colors.empty() && !pass.Depth) Fail(data, "raster pass requires an attachment");
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
    for (uint32_t slot = 0; slot < pass.Colors.size(); ++slot) {
        const auto& color = pass.Colors[slot];
        if (color.Slot != slot) Fail(data, "color attachment slots must be contiguous from zero");
        if (!ValidLoad(color.Load) || !ValidStore(color.Store)) Fail(data, "invalid color attachment load/store");
        validate(color.Texture, color.View, render::TextureViewUsage::RenderTarget);
    }
    if (pass.Depth) {
        const auto& depth = *pass.Depth;
        const auto usage = DepthUsage(depth.State);
        const bool stencil = render::GetTextureFormatAspects(data.Textures[depth.Texture.Index].Desc.Format).HasFlag(render::TextureAspect::Stencil);
        if (!ValidLoad(depth.DepthLoad) || !ValidStore(depth.DepthStore) || !ValidLoad(depth.StencilLoad) || !ValidStore(depth.StencilStore)) Fail(data, "invalid depth/stencil load/store");
        if ((render::IsDepthReadOnly(usage) && (depth.DepthLoad != render::LoadAction::Load || depth.DepthStore != render::StoreAction::Store)) ||
            (stencil && render::IsStencilReadOnly(usage) && (depth.StencilLoad != render::LoadAction::Load || depth.StencilStore != render::StoreAction::Store))) Fail(data, "read-only depth/stencil aspects require Load and Store");
        validate(depth.Texture, depth.View, usage);
    }
    if (shape) {
        pass.Width = (*shape)[0];
        pass.Height = (*shape)[1];
        pass.Layers = (*shape)[2];
    }
}

bool ValidAccess(const RDGData& data, const RDGPass& pass, const RDGAccess& access) noexcept {
    if (access.Texture) {
        const auto state = static_cast<render::TextureState>(access.State);
        if (!ValidTextureState(data.Textures[access.Index].Desc, state)) return false;
        if (pass.Kind == RDGPassKind::Compute && state != render::TextureState::ShaderRead && state != render::TextureState::UnorderedAccess) return false;
        if (pass.Kind == RDGPassKind::Raster && state != render::TextureState::ShaderRead && state != render::TextureState::UnorderedAccess) {
            const bool color = state == render::TextureState::RenderTarget && std::any_of(pass.Colors.begin(), pass.Colors.end(), [&](const auto& attachment) { return attachment.Texture.Index == access.Index; });
            const bool depth = pass.Depth && pass.Depth->Texture.Index == access.Index && pass.Depth->State == state;
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
    const auto state = static_cast<render::BufferState>(access.State);
    if (!ValidBufferState(data.Buffers[access.Index].Desc, state)) return false;
    if (pass.Kind == RDGPassKind::Raster || pass.Kind == RDGPassKind::Compute) {
        if (access.Write && state != render::BufferState::UnorderedAccess) return false;
        const bool shader = state == render::BufferState::ShaderRead || state == render::BufferState::CBuffer || state == render::BufferState::UnorderedAccess || state == render::BufferState::Indirect;
        const bool geometry = pass.Kind == RDGPassKind::Raster && (state == render::BufferState::Vertex || state == render::BufferState::Index);
        if (!shader && !geometry) return false;
    }
    if (access.Write) return state == render::BufferState::UnorderedAccess || state == render::BufferState::CopyDestination ||
                             (pass.Kind == RDGPassKind::Upload && state == render::BufferState::HostWrite);
    return state == render::BufferState::ShaderRead || state == render::BufferState::CopySource || state == render::BufferState::Vertex ||
           state == render::BufferState::Index || state == render::BufferState::CBuffer || state == render::BufferState::Indirect || state == render::BufferState::UnorderedAccess;
}

render::TextureViewDescriptor ViewDescriptor(render::Texture& texture, render::TextureViewUsage usage) noexcept {
    const auto desc = texture.GetDesc();
    const uint32_t layers = desc.Dim == render::TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize;
    return {&texture, desc.Dim, desc.Format, {0, layers, 0, 1}, usage};
}

bool Declared(const RDGExecution& execution, bool texture, uint32_t index) noexcept {
    if (!execution.Pass) return false;
    for (const auto& access : execution.Pass->Accesses) {
        if (access.Texture == texture && access.Index == index) return true;
    }
    return false;
}

render::Texture& ResolveTexture(const RDGExecution& execution, RDGTextureHandle handle) noexcept {
    if (!Declared(execution, true, handle.Index) || !execution.Textures[handle.Index].Resource) RADRAY_ABORT("RDG: resolving undeclared texture");
    return *execution.Textures[handle.Index].Resource;
}

render::Buffer& ResolveBuffer(const RDGExecution& execution, RDGBufferHandle handle) noexcept {
    if (!Declared(execution, false, handle.Index) || !execution.Buffers[handle.Index].Resource) RADRAY_ABORT("RDG: resolving undeclared buffer");
    return *execution.Buffers[handle.Index].Resource;
}

Nullable<render::TextureView*> ResolveTextureView(RDGExecution& execution, RDGTextureHandle handle, render::TextureViewUsage usage) noexcept {
    if (!Declared(execution, true, handle.Index)) return nullptr;
    const auto state = execution.Textures[handle.Index].CurrentState;
    const bool compatible = (usage == render::TextureViewUsage::Resource && state == render::TextureState::ShaderRead) ||
                            (usage == render::TextureViewUsage::UnorderedAccess && state == render::TextureState::UnorderedAccess) ||
                            (usage == render::TextureViewUsage::RenderTarget && state == render::TextureState::RenderTarget) ||
                            (usage != render::TextureViewUsage::UNKNOWN && usage == DepthUsage(state));
    if (!compatible) return nullptr;
    const auto descriptor = ViewDescriptor(ResolveTexture(execution, handle), usage);
    if (!execution.Data.Textures[handle.Index].Imported) return execution.Pool.AcquireTextureView(descriptor);
    for (const auto& entry : execution.ImportedViews) {
        if (entry.Texture == handle && entry.Usage == usage) return entry.View;
    }
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.CreateImportedView");
    auto view = execution.Slot.GetDevice().CreateTextureView(descriptor);
    if (!view) return nullptr;
    auto* owner = execution.Slot.GetArena().Emplace<unique_ptr<render::TextureView>>(view.Release());
    execution.ImportedViews.push_back({handle, usage, owner->get()});
    return owner->get();
}

struct GroupAccess {
    size_t Group{std::numeric_limits<size_t>::max()};
    RDGAccess Access{};
};

size_t AccessIndex(const RDGData& data, const RDGAccess& access) noexcept {
    return access.Texture ? access.Index : data.Textures.size() + access.Index;
}

bool CanMerge(const RDGData& data, const RDGPass& candidate, const vector<GroupAccess>& accesses) noexcept {
    if (data.Groups.empty()) return false;
    const auto& group = data.Groups.back();
    if (group.Kind != candidate.Kind || (group.Kind != RDGPassKind::Raster && group.Kind != RDGPassKind::Compute)) return false;
    const auto& previous = data.Passes[group.Passes.back()];
    if (group.Kind == RDGPassKind::Raster) {
        if (previous.Colors.size() != candidate.Colors.size() || bool(previous.Depth) != bool(candidate.Depth)) return false;
        for (size_t i = 0; i < previous.Colors.size(); ++i) {
            const auto& a = previous.Colors[i];
            const auto& b = candidate.Colors[i];
            if (a.Slot != b.Slot || a.Texture != b.Texture || a.View != b.View || a.Store != render::StoreAction::Store || b.Load != render::LoadAction::Load) return false;
        }
        if (previous.Depth) {
            const auto& a = *previous.Depth;
            const auto& b = *candidate.Depth;
            if (a.Texture != b.Texture || a.View != b.View || a.State != b.State || a.DepthStore != render::StoreAction::Store || b.DepthLoad != render::LoadAction::Load) return false;
            const bool stencil = render::GetTextureFormatAspects(data.Textures[a.Texture.Index].Desc.Format).HasFlag(render::TextureAspect::Stencil);
            if (stencil && (a.StencilStore != render::StoreAction::Store || b.StencilLoad != render::LoadAction::Load)) return false;
        }
    }
    for (const auto& access : candidate.Accesses) {
        const auto& entry = accesses[AccessIndex(data, access)];
        if (entry.Group != data.Groups.size() - 1) continue;
        const auto& old = entry.Access;
        if (old.State != access.State) return false;
        if (candidate.Kind == RDGPassKind::Raster && access.Texture) {
            if (candidate.Depth && candidate.Depth->Texture.Index == access.Index && access.State == static_cast<uint32_t>(candidate.Depth->State)) continue;
            if (access.State == static_cast<uint32_t>(render::TextureState::RenderTarget) &&
                std::any_of(candidate.Colors.begin(), candidate.Colors.end(), [&](const auto& color) { return color.Texture.Index == access.Index; })) continue;
        }
        if (old.Write || access.Write) return false;
    }
    return true;
}

bool SchedulePasses(RDGData& data) {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile.Schedule");
    const auto count = data.Passes.size();
    vector<vector<uint32_t>> dependents(count);
    vector<uint32_t> inDegree(count, 0);
    vector<uint32_t> seen(count, std::numeric_limits<uint32_t>::max());
    vector<uint32_t> ready;
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
        if (!inDegree[index]) ready.push_back(index);
    }
    vector<GroupAccess> accesses(data.Textures.size() + data.Buffers.size());
    while (!ready.empty()) {
        size_t best = 0;
        uint32_t bestPriority = 3;
        for (size_t i = 0; i < ready.size(); ++i) {
            const auto& pass = data.Passes[ready[i]];
            const uint32_t priority = CanMerge(data, pass, accesses) ? 0 : (pass.Kind == RDGPassKind::Upload || pass.Kind == RDGPassKind::Copy ? 1 : 2);
            if (priority < bestPriority || (priority == bestPriority && ready[i] < ready[best])) {
                best = i;
                bestPriority = priority;
            }
        }
        const auto index = ready[best];
        ready[best] = ready.back();
        ready.pop_back();
        const auto& pass = data.Passes[index];
        if (bestPriority != 0) data.Groups.push_back({pass.Kind, {}});
        data.Groups.back().Passes.push_back(index);
        data.ScheduledPasses.push_back(index);
        for (const auto& access : pass.Accesses) {
            auto& entry = accesses[AccessIndex(data, access)];
            if (entry.Group != data.Groups.size() - 1)
                entry = {data.Groups.size() - 1, access};
            else
                entry.Access.Write |= access.Write;
        }
        for (const auto next : dependents[index]) {
            if (--inDegree[next] == 0) ready.push_back(next);
        }
    }
    if (data.ScheduledPasses.size() != aliveCount) Fail(data, "cannot schedule all live passes");
    return !data.Failed;
}

void TransitionTexture(RDGExecution& execution, uint32_t index, render::TextureState next, bool write, vector<render::ResourceBarrierDescriptor>& barriers) noexcept {
    auto& current = execution.Textures[index];
    const auto before = current.CurrentState;
    if (before == render::TextureState::UnorderedAccess && next == before && (current.PendingWrite || write)) {
        barriers.emplace_back(render::BarrierUavDescriptor{current.Resource.Get()});
    } else if (before != next || current.PendingWrite) {
        barriers.emplace_back(render::BarrierTextureDescriptor{current.Resource.Get(), before, next});
    }
    current.CurrentState = next;
    current.PendingWrite = write;
}

void TransitionBuffer(RDGExecution& execution, uint32_t index, render::BufferState next, bool write, vector<render::ResourceBarrierDescriptor>& barriers) noexcept {
    auto& current = execution.Buffers[index];
    const auto before = current.CurrentState;
    if (before == render::BufferState::UnorderedAccess && next == before && (current.PendingWrite || write)) {
        barriers.emplace_back(render::BarrierUavDescriptor{current.Resource.Get()});
    } else if (before != next || current.PendingWrite) {
        barriers.emplace_back(render::BarrierBufferDescriptor{current.Resource.Get(), before, next});
    }
    current.CurrentState = next;
    current.PendingWrite = write;
}

bool MapUpload(render::Buffer& buffer, uint64_t offset, std::span<const byte> bytes) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.MapUpload");
    RADRAY_PROFILE_PLOT("RadRay.RDG.UploadBytes", static_cast<int64_t>(bytes.size()));
    Nullable<void*> mapped = buffer.Map(offset, bytes.size());
    if (!mapped) {
        RADRAY_ERR_LOG("RDG: cannot map upload buffer");
        return false;
    }
    std::memcpy(mapped.Get(), bytes.data(), bytes.size());
    buffer.FlushMappedRange({offset, bytes.size()});
    buffer.Unmap();
    return true;
}

bool ExecuteUpload(RDGExecution& execution, const RDGPass& pass) noexcept {
    const auto index = pass.Accesses.front().Index;
    auto& target = *execution.Buffers[index].Resource;
    if (execution.Data.Buffers[index].Desc.Memory == render::MemoryType::Upload) {
        return MapUpload(target, pass.UploadOffset, pass.UploadBytes);
    }
    const auto allocation = execution.Pool.AcquireBuffer({.Size = pass.UploadBytes.size(), .Memory = render::MemoryType::Upload, .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite});
    if (!allocation.Buffer) return false;
    auto& staging = *allocation.Buffer;
    // This staging object is unique for this execution and its prior epoch completed.
    const render::ResourceBarrierDescriptor toHost = render::BarrierBufferDescriptor{&staging, allocation.LastState, render::BufferState::HostWrite};
    if (allocation.LastState != render::BufferState::HostWrite) execution.Command.ResourceBarrier(std::span{&toHost, 1});
    if (!MapUpload(staging, 0, pass.UploadBytes)) return false;
    const render::ResourceBarrierDescriptor toCopy = render::BarrierBufferDescriptor{&staging, render::BufferState::HostWrite, render::BufferState::CopySource};
    execution.Command.ResourceBarrier(std::span{&toCopy, 1});
    execution.Command.CopyBufferToBuffer(&target, pass.UploadOffset, &staging, 0, pass.UploadBytes.size());
    execution.Pool.CommitBufferState(&staging, render::BufferState::CopySource);
    return true;
}

}  // namespace

struct RDGTransientResourcePool::Impl {
    struct TextureEntry {
        render::TextureDescriptor Desc;
        unique_ptr<render::Texture> Resource;
        render::TextureState State{render::TextureState::Undefined};
        bool Used{true};
    };
    struct BufferEntry {
        render::BufferDescriptor Desc;
        unique_ptr<render::Buffer> Resource;
        render::BufferState State;
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

void RDGTransientResourcePool::CommitBufferState(render::Buffer* buffer, render::BufferState state) noexcept {
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

RDGBuilder::RDGBuilder(ObjectArena& arena) noexcept : _arena(arena), _data(*arena.Emplace<detail::RDGData>()) {}

RDGTextureHandle RDGBuilder::ImportTexture(render::Texture* texture, render::TextureState initialState, std::optional<render::TextureState> finalState) {
    RequireOpen(_data);
    for (uint32_t i = 0; i < _data.Textures.size(); ++i) {
        auto& previous = _data.Textures[i];
        if (previous.Imported.Get() != texture) continue;
        if (previous.InitialState != initialState || (previous.FinalState && finalState && previous.FinalState != finalState)) Fail(_data, "inconsistent duplicate texture import");
        if (finalState) previous.FinalState = finalState;
        return {i};
    }
    const auto index = static_cast<uint32_t>(_data.Textures.size());
    _data.Textures.push_back({.Desc = texture->GetDesc(), .Imported = texture, .InitialState = initialState, .FinalState = finalState});
    return {index};
}

RDGBufferHandle RDGBuilder::ImportBuffer(render::Buffer* buffer, render::BufferState initialState, std::optional<render::BufferState> finalState) {
    RequireOpen(_data);
    for (uint32_t i = 0; i < _data.Buffers.size(); ++i) {
        auto& previous = _data.Buffers[i];
        if (previous.Imported.Get() != buffer) continue;
        if (previous.InitialState != initialState || (previous.FinalState && finalState && previous.FinalState != finalState)) Fail(_data, "inconsistent duplicate buffer import");
        if (finalState) previous.FinalState = finalState;
        return {i};
    }
    const auto index = static_cast<uint32_t>(_data.Buffers.size());
    _data.Buffers.push_back({.Desc = buffer->GetDesc(), .Imported = buffer, .InitialState = initialState, .FinalState = finalState});
    return {index};
}

RDGTextureHandle RDGBuilder::CreateTexture(const render::TextureDescriptor& desc) {
    RequireOpen(_data);
    const auto index = static_cast<uint32_t>(_data.Textures.size());
    _data.Textures.push_back({.Desc = desc});
    return {index};
}

RDGBufferHandle RDGBuilder::CreateBuffer(const render::BufferDescriptor& desc) {
    RequireOpen(_data);
    const auto index = static_cast<uint32_t>(_data.Buffers.size());
    _data.Buffers.push_back({.Desc = desc});
    return {index};
}

void RDGBuilder::UploadBuffer(RDGBufferHandle dst, uint64_t offset, std::span<const byte> bytes) {
    RequireOpen(_data);
    if (dst.Index >= _data.Buffers.size()) {
        Fail(_data, "invalid upload destination");
        return;
    }
    const auto desc = _data.Buffers[dst.Index].Desc;
    if (offset > desc.Size || bytes.size() > desc.Size - offset || desc.Memory == render::MemoryType::ReadBack ||
        (desc.Memory == render::MemoryType::Device && !desc.Usage.HasFlag(render::BufferUse::CopyDestination)) ||
        (desc.Memory == render::MemoryType::Upload && !desc.Usage.HasFlag(render::BufferUse::MapWrite))) {
        Fail(_data, "invalid upload range, memory or usage");
        return;
    }
    if (bytes.empty()) return;
    auto* copy = static_cast<byte*>(_arena.AllocateBytes(bytes.size(), alignof(byte)));
    std::memcpy(copy, bytes.data(), bytes.size());
    const auto index = AddPass(_data, "UploadBuffer", RDGPassKind::Upload);
    auto& pass = _data.Passes[index];
    pass.UploadBytes = {copy, bytes.size()};
    pass.UploadOffset = offset;
    BufferAccess(_data, index, dst, desc.Memory == render::MemoryType::Upload ? render::BufferState::HostWrite : render::BufferState::CopyDestination, false, true);
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

uint32_t RDGBuilder::AddRasterPassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGRasterPassContext&)) {
    const auto index = AddPass(_data, name, RDGPassKind::Raster);
    _data.Passes[index].Callback = callback;
    _data.Passes[index].RasterExecute = execute;
    return index;
}

uint32_t RDGBuilder::AddComputePassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGComputePassContext&)) {
    const auto index = AddPass(_data, name, RDGPassKind::Compute);
    _data.Passes[index].Callback = callback;
    _data.Passes[index].ComputeExecute = execute;
    return index;
}

uint32_t RDGBuilder::AddCopyPassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGCopyPassContext&)) {
    const auto index = AddPass(_data, name, RDGPassKind::Copy);
    _data.Passes[index].Callback = callback;
    _data.Passes[index].CopyExecute = execute;
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
    _data.Passes[_pass].MeshPasses.push_back(pass);
    return *this;
}

RDGRasterPassBuilder& RDGRasterPassBuilder::WriteColor(uint32_t slot, RDGTextureHandle texture, render::LoadAction load, render::StoreAction store,
                                                       render::ColorClearValue clear, Nullable<render::TextureView*> explicitView) {
    RequireOpen(_data);
    auto& colors = _data.Passes[_pass].Colors;
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
    if (pass.Depth || DepthUsage(state) == render::TextureViewUsage::UNKNOWN) {
        Fail(_data, "invalid or duplicate depth/stencil attachment");
        return *this;
    }
    pass.Depth = {texture, state, depthLoad, depthStore, stencilLoad, stencilStore, clear, explicitView};
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
            if (!resource.Desc.Size || (resource.Imported && (!ValidBufferState(resource.Desc, resource.InitialState) ||
                                                              (resource.FinalState && !ValidBufferState(resource.Desc, *resource.FinalState))))) Fail(_data, "invalid buffer descriptor or boundary state");
        }
    }
    vector<uint32_t> seenData(_data.Passes.size(), std::numeric_limits<uint32_t>::max());
    vector<uint32_t> seenOrder(_data.Passes.size(), std::numeric_limits<uint32_t>::max());
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Compile.Dependencies");
        for (uint32_t index = 0; index < _data.Passes.size(); ++index) {
            auto& pass = _data.Passes[index];
            const auto addEdge = [index](auto& dependencies, auto& seen, uint32_t predecessor) {
                if (seen[predecessor] == index) return;
                seen[predecessor] = index;
                dependencies.push_back(predecessor);
            };
            std::sort(pass.Colors.begin(), pass.Colors.end(), [](const auto& a, const auto& b) { return a.Slot < b.Slot; });
            ValidateAttachments(_data, pass);
            for (const auto& access : pass.Accesses) {
                if (!ValidAccess(_data, pass, access)) Fail(_data, fmt::format("pass '{}': {}#{} state {} is incompatible with usage, access mode or pass kind", pass.Name, access.Texture ? "texture" : "buffer", access.Index, access.State));
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
                if (!access.Texture && _data.Buffers[access.Index].Desc.Memory == render::MemoryType::Upload && pass.Kind == RDGPassKind::Upload) {
                    for (uint32_t earlier = 0; earlier < index; ++earlier) {
                        const auto& previous = _data.Passes[earlier];
                        for (const auto& oldAccess : previous.Accesses) {
                            if (oldAccess.Texture || oldAccess.Index != access.Index) continue;
                            if (previous.Kind != RDGPassKind::Upload)
                                Fail(_data, "direct upload must precede GPU access");
                            else if (pass.UploadOffset < previous.UploadOffset + previous.UploadBytes.size() && previous.UploadOffset < pass.UploadOffset + pass.UploadBytes.size())
                                Fail(_data, "overlapping direct upload ranges");
                        }
                    }
                }
            }
        }
    }
    if (_data.Failed) return nullptr;
    vector<uint32_t> pending;
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
            for (const auto& access : pass.Accesses) {
                if (access.Texture)
                    _data.Textures[access.Index].Alive = true;
                else
                    _data.Buffers[access.Index].Alive = true;
            }
            for (const auto meshPass : pass.MeshPasses) {
                if (std::find(_data.AliveMeshPasses.begin(), _data.AliveMeshPasses.end(), meshPass) == _data.AliveMeshPasses.end()) _data.AliveMeshPasses.push_back(meshPass);
            }
        }
    }
    if (!SchedulePasses(_data)) return nullptr;
    RADRAY_PROFILE_PLOT("RadRay.RDG.AlivePasses", static_cast<int64_t>(_data.ScheduledPasses.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.ExecutionGroups", static_cast<int64_t>(_data.Groups.size()));
    RADRAY_PROFILE_PLOT("RadRay.RDG.AliveMeshPasses", static_cast<int64_t>(_data.AliveMeshPasses.size()));
    return _arena.Emplace<CompiledRDG>(_data);
}

CompiledRDG::CompiledRDG(detail::RDGData& data) noexcept : _data(data) {}
std::span<const MeshPassId> CompiledRDG::GetAliveMeshPasses() const noexcept { return _data.AliveMeshPasses; }

bool CompiledRDG::Execute(FrameSlot& slot, RDGTransientResourcePool& pool, render::CommandBuffer& command) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.RDG.Execute");
    if (_executed) {
        RADRAY_ERR_LOG("RDG: graph already executed");
        return false;
    }
    _executed = true;
    RDGExecution execution{_data, slot, pool, command, {}, {}};
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.AcquireResources");
        execution.Textures.resize(_data.Textures.size());
        execution.Buffers.resize(_data.Buffers.size());
        for (size_t i = 0; i < _data.Textures.size(); ++i) {
            const auto& resource = _data.Textures[i];
            if (!resource.Alive) continue;
            const auto allocation = resource.Imported ? RDGTransientTextureAllocation{resource.Imported, resource.InitialState} : pool.AcquireTexture(resource.Desc);
            if (!allocation.Texture) return false;
            execution.Textures[i] = {allocation.Texture, allocation.LastState, IsWriteState(allocation.LastState)};
        }
        for (size_t i = 0; i < _data.Buffers.size(); ++i) {
            const auto& resource = _data.Buffers[i];
            if (!resource.Alive) continue;
            const auto allocation = resource.Imported ? RDGTransientBufferAllocation{resource.Imported, resource.InitialState} : pool.AcquireBuffer(resource.Desc);
            if (!allocation.Buffer) return false;
            execution.Buffers[i] = {allocation.Buffer, allocation.LastState, IsWriteState(allocation.LastState)};
        }
    }
    vector<render::ResourceBarrierDescriptor> barriers;
    vector<size_t> seen(_data.Textures.size() + _data.Buffers.size(), std::numeric_limits<size_t>::max());
    for (size_t groupIndex = 0; groupIndex < _data.Groups.size(); ++groupIndex) {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.ExecuteGroup");
        const auto& group = _data.Groups[groupIndex];
        bool uav = false;
        {
            RADRAY_PROFILE_SCOPE_N("RadRay.RDG.GroupBarriers");
            barriers.clear();
            for (const auto passIndex : group.Passes) {
                for (const auto& access : _data.Passes[passIndex].Accesses) {
                    uav |= access.Write && access.State == (access.Texture ? static_cast<uint32_t>(render::TextureState::UnorderedAccess) : static_cast<uint32_t>(render::BufferState::UnorderedAccess));
                    auto& stamp = seen[AccessIndex(_data, access)];
                    if (stamp == groupIndex) {
                        if (access.Texture)
                            execution.Textures[access.Index].PendingWrite |= access.Write;
                        else
                            execution.Buffers[access.Index].PendingWrite |= access.Write;
                        continue;
                    }
                    stamp = groupIndex;
                    if (access.Texture)
                        TransitionTexture(execution, access.Index, static_cast<render::TextureState>(access.State), access.Write, barriers);
                    else
                        TransitionBuffer(execution, access.Index, static_cast<render::BufferState>(access.State), access.Write, barriers);
                }
            }
            if (!barriers.empty()) command.ResourceBarrier(barriers);
            RADRAY_PROFILE_PLOT("RadRay.RDG.GroupBarriers", static_cast<int64_t>(barriers.size()));
        }
        const auto& first = _data.Passes[group.Passes.front()];
        const auto& last = _data.Passes[group.Passes.back()];
        execution.Pass = &first;
        bool result = false;
        if (group.Kind == RDGPassKind::Upload || group.Kind == RDGPassKind::Copy) {
            RADRAY_PROFILE_SCOPE_N("RadRay.RDG.RecordCopyOrUpload");
            {
                RADRAY_PROFILE_SCOPE_DYN(first.Name);
                command.PushDebugGroup(first.Name);
                if (group.Kind == RDGPassKind::Upload)
                    result = ExecuteUpload(execution, first);
                else {
                    RDGCopyPassContext ctx{execution};
                    result = first.CopyExecute.Get()(first.Callback.Get(), ctx);
                }
                command.PopDebugGroup();
            }
        } else if (group.Kind == RDGPassKind::Compute) {
            RADRAY_PROFILE_SCOPE_N("RadRay.RDG.RecordCompute");
            auto encoder = command.BeginComputePass();
            if (encoder) {
                RDGComputePassContext ctx{execution, *encoder};
                for (const auto passIndex : group.Passes) {
                    const auto& pass = _data.Passes[passIndex];
                    RADRAY_PROFILE_SCOPE_DYN(pass.Name);
                    execution.Pass = &pass;
                    command.PushDebugGroup(pass.Name);
                    result = pass.ComputeExecute.Get()(pass.Callback.Get(), ctx);
                    command.PopDebugGroup();
                    if (!result) break;
                }
                command.EndComputePass(encoder.Release());
            }
        } else {
            RADRAY_PROFILE_SCOPE_N("RadRay.RDG.RecordRaster");
            vector<render::RenderPassColorAttachment> colors;
            std::optional<render::RenderPassDepthStencilAttachment> depth;
            bool valid = true;
            {
                RADRAY_PROFILE_SCOPE_N("RadRay.RDG.ResolveAttachments");
                for (size_t i = 0; i < first.Colors.size(); ++i) {
                    const auto& attachment = first.Colors[i];
                    const auto view = attachment.View ? attachment.View : ResolveTextureView(execution, attachment.Texture, render::TextureViewUsage::RenderTarget);
                    if (!view) {
                        valid = false;
                        break;
                    }
                    colors.push_back({view.Get(), attachment.Load, last.Colors[i].Store, attachment.Clear});
                }
                if (first.Depth) {
                    const auto& attachment = *first.Depth;
                    const auto view = attachment.View ? attachment.View : ResolveTextureView(execution, attachment.Texture, DepthUsage(attachment.State));
                    if (!view)
                        valid = false;
                    else
                        depth = {view.Get(), attachment.DepthLoad, last.Depth->DepthStore, attachment.StencilLoad, last.Depth->StencilStore, attachment.Clear};
                }
            }
            if (valid) {
                auto encoder = command.BeginRenderPass({.Colors = colors, .DepthStencil = depth, .Width = first.Width, .Height = first.Height, .Layers = first.Layers, .Name = first.Name, .AllowUavWrites = uav});
                if (encoder) {
                    RDGRasterPassContext ctx{execution, *encoder};
                    for (const auto passIndex : group.Passes) {
                        const auto& pass = _data.Passes[passIndex];
                        RADRAY_PROFILE_SCOPE_DYN(pass.Name);
                        execution.Pass = &pass;
                        command.PushDebugGroup(pass.Name);
                        encoder->SetStencilReference(0);
                        result = pass.RasterExecute.Get()(pass.Callback.Get(), ctx);
                        command.PopDebugGroup();
                        if (!result) break;
                    }
                    command.EndRenderPass(encoder.Release());
                }
            }
        }
        if (!result) {
            RADRAY_ERR_LOG("RDG: execution failed in pass '{}'", execution.Pass->Name);
            return false;
        }
    }
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.RDG.FinalizeStates");
        barriers.clear();
        for (uint32_t i = 0; i < _data.Textures.size(); ++i) {
            const auto& resource = _data.Textures[i];
            if (!resource.Alive) continue;
            if (resource.FinalState) TransitionTexture(execution, i, *resource.FinalState, false, barriers);
            if (!resource.Imported) pool.CommitTextureState(execution.Textures[i].Resource.Get(), execution.Textures[i].CurrentState);
        }
        for (uint32_t i = 0; i < _data.Buffers.size(); ++i) {
            const auto& resource = _data.Buffers[i];
            if (!resource.Alive) continue;
            if (resource.FinalState) TransitionBuffer(execution, i, *resource.FinalState, false, barriers);
            if (!resource.Imported) pool.CommitBufferState(execution.Buffers[i].Resource.Get(), execution.Buffers[i].CurrentState);
        }
        if (!barriers.empty()) command.ResourceBarrier(barriers);
        RADRAY_PROFILE_PLOT("RadRay.RDG.FinalBarriers", static_cast<int64_t>(barriers.size()));
    }
    return true;
}

RDGRasterPassContext::RDGRasterPassContext(detail::RDGExecution& execution, render::GraphicsCommandEncoder& encoder) noexcept : _execution(execution), _encoder(encoder) {}
render::GraphicsCommandEncoder& RDGRasterPassContext::GetEncoder() noexcept { return _encoder; }
FrameSlot& RDGRasterPassContext::GetSlot() noexcept { return _execution.Slot; }
render::Texture& RDGRasterPassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
render::Buffer& RDGRasterPassContext::Resolve(RDGBufferHandle handle) const noexcept { return ResolveBuffer(_execution, handle); }
Nullable<render::TextureView*> RDGRasterPassContext::ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }

RDGComputePassContext::RDGComputePassContext(detail::RDGExecution& execution, render::ComputeCommandEncoder& encoder) noexcept : _execution(execution), _encoder(encoder) {}
render::ComputeCommandEncoder& RDGComputePassContext::GetEncoder() noexcept { return _encoder; }
FrameSlot& RDGComputePassContext::GetSlot() noexcept { return _execution.Slot; }
render::Texture& RDGComputePassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
render::Buffer& RDGComputePassContext::Resolve(RDGBufferHandle handle) const noexcept { return ResolveBuffer(_execution, handle); }
Nullable<render::TextureView*> RDGComputePassContext::ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept { return ResolveTextureView(_execution, handle, usage); }

RDGCopyPassContext::RDGCopyPassContext(detail::RDGExecution& execution) noexcept : _execution(execution) {}
render::CommandBuffer& RDGCopyPassContext::GetCommandBuffer() noexcept { return _execution.Command; }
FrameSlot& RDGCopyPassContext::GetSlot() noexcept { return _execution.Slot; }
render::Texture& RDGCopyPassContext::Resolve(RDGTextureHandle handle) const noexcept { return ResolveTexture(_execution, handle); }
render::Buffer& RDGCopyPassContext::Resolve(RDGBufferHandle handle) const noexcept { return ResolveBuffer(_execution, handle); }

RDGRasterPassBuilder& RDGRasterPassBuilder::ReadBuffer(RDGBufferHandle handle, render::BufferState state) {
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

RDGComputePassBuilder& RDGComputePassBuilder::ReadBuffer(RDGBufferHandle handle, render::BufferState state) {
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
