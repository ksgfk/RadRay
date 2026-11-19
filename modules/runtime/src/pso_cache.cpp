#include <radray/runtime/pso_cache.h>

#include <algorithm>
#include <utility>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/pipeline_layout_registry.h>
#include <radray/runtime/shader_asset.h>

namespace radray {
namespace {
struct PipelineOwner {
    shared_ptr<GpuPipelineLayout> Layout;
    unique_ptr<gpu::PipelineState> State;
};

template <class Key>
struct CacheEntry {
    Key Description;
    uint32_t Revision;
    PipelineOwner Owner;
};
}

class PsoCache::Impl {
public:
    gpu::Device& Device;
    const AssetManager& Assets;
    PipelineLayoutRegistry& Layouts;
    Nullable<gpu::PipelineCache*> Native;
    map<UntypedAssetHandle, vector<CacheEntry<GraphicsPsoKey>>> Graphics;
    map<UntypedAssetHandle, vector<CacheEntry<ComputePsoKey>>> Compute;

    template <class State, class Key, class Create>
    Nullable<State*> Acquire(map<UntypedAssetHandle, vector<CacheEntry<Key>>>& entries, const Key& key, Create create) {
        const auto shader = Assets.Get(key.Shader);
        if (!shader || shader->GetBackend() != Device.GetBackend()) return nullptr;
        const auto id = key.Shader.Untyped();
        const auto revision = Assets.GetRevision(id);
        if (const auto bucket = entries.find(id); bucket != entries.end()) {
            for (auto& entry : bucket->second) {
                if (entry.Revision == revision && entry.Description == key) return static_cast<State*>(entry.Owner.State.get());
            }
        }
        auto layout = key.ExplicitLayout;
        if (!layout) layout = Layouts.Acquire(shader->GetLayoutDesc()).Release();
        if (!layout) return nullptr;
        auto state = create(*shader, layout->GetLayout());
        if (!state) return nullptr;
        auto* result = state.Get();
        entries[id].push_back({key, revision, {std::move(layout), state.Release()}});
        return result;
    }

    template <class Key>
    void Retire(map<UntypedAssetHandle, vector<CacheEntry<Key>>>& entries, FrameSlot& slot) {
        for (auto it = entries.begin(); it != entries.end();) {
            const auto revision = Assets.GetRevision(it->first);
            std::erase_if(it->second, [&](auto& entry) {
                if (entry.Revision == revision) return false;
                slot.Emplace<PipelineOwner>(std::move(entry.Owner));
                return true;
            });
            if (it->second.empty()) it = entries.erase(it);
            else ++it;
        }
    }
};

PsoCache::PsoCache(gpu::Device& device, const AssetManager& assets, PipelineLayoutRegistry& layouts, Nullable<gpu::PipelineCache*> nativeCache)
    : _impl(new Impl{device, assets, layouts, nativeCache, {}, {}}) {}
PsoCache::~PsoCache() noexcept = default;

Nullable<gpu::GraphicsPipelineState*> PsoCache::AcquireGraphics(const GraphicsPsoKey& key) {
    return _impl->Acquire<gpu::GraphicsPipelineState>(_impl->Graphics, key, [&](const ShaderAsset& shader, gpu::PipelineLayout& layout) -> Nullable<unique_ptr<gpu::GraphicsPipelineState>> {
        auto interface = shader.GetVariantInterface(key.Variant);
        auto vs = shader.GetStage(key.Variant, gpu::ShaderStage::Vertex);
        if (!interface || !vs) return nullptr;
        ResolvedVertexInputState vertices;
        const VertexInput source{key.VertexLayouts, key.VertexAttributes, {}};
        if (!ResolveVertexInputState(source, interface->VertexInputs, vertices)) return nullptr;
        return _impl->Device.CreateGraphicsPipelineState({.PipelineLayout = &layout, .VS = vs,
            .PS = key.EnablePixelStage ? shader.GetStage(key.Variant, gpu::ShaderStage::Pixel) : std::nullopt,
            .VertexInput = {vertices.Layouts, vertices.Attributes}, .Primitive = key.Primitive,
            .DepthStencil = key.DepthStencil, .MultiSample = key.MultiSample, .ColorTargets = key.ColorTargets, .Cache = _impl->Native});
    });
}

Nullable<gpu::ComputePipelineState*> PsoCache::AcquireCompute(const ComputePsoKey& key) {
    return _impl->Acquire<gpu::ComputePipelineState>(_impl->Compute, key, [&](const ShaderAsset& shader, gpu::PipelineLayout& layout) -> Nullable<unique_ptr<gpu::ComputePipelineState>> {
        const auto cs = shader.GetStage(key.Variant, gpu::ShaderStage::Compute);
        if (!cs) return nullptr;
        return _impl->Device.CreateComputePipelineState({&layout, *cs, _impl->Native});
    });
}

void PsoCache::RetireInvalidated(FrameSlot& slot) {
    _impl->Retire(_impl->Graphics, slot);
    _impl->Retire(_impl->Compute, slot);
}

}  // namespace radray
