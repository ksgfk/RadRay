#pragma once

#include <radray/runtime/render_framework/renderer_id.h>
#include <radray/render/rhi.h>

namespace radray {
struct ParameterWrite {
    render::BindingHandle Binding;
    uint32_t Element;
    render::ShaderParameterValue Value;
};
struct BindingCacheBudget {
    uint32_t ResourceDescriptors{4096}, SamplerDescriptors{128}, Groups{4096};
};

/// Owner-thread-only immutable complete parameter groups. Frame use lists pin each block once.
class BindingRegistry {
public:
    BindingRegistry(render::Device* device, Guid owner);
    ~BindingRegistry() noexcept;
    BindingRegistry(const BindingRegistry&) = delete;
    BindingRegistry& operator=(const BindingRegistry&) = delete;
    std::optional<DescriptorBlockId> Resolve(render::PipelineLayout* layout, uint32_t group,
                                             std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies);
    bool Retain(DescriptorBlockId id);
    bool Release(DescriptorBlockId id);
    bool Bind(render::GraphicsCommandEncoder* encoder, uint32_t destinationGroup, DescriptorBlockId id,
              std::span<const render::ShaderParameterDynamicOffset> offsets) const;
    /// Marks every dependent block for destruction when its admitted/submitted uses finish.
    void RetireResource(ResourceId id);
    void ForgetResource(ResourceId id);
    bool HasResource(ResourceId id) const;
    void SetBudget(BindingCacheBudget budget);
    void TrimIdle(bool all = false);
    bool IsValid(DescriptorBlockId id) const noexcept;
    bool HasActiveUses() const noexcept;

private:
    struct Entry {
        render::ParameterGroupCompatibility Layout;
        vector<vector<uint64_t>> Values;
        vector<ResourceId> Dependencies;
        unique_ptr<render::ShaderParameterSet> Parameters;
        size_t Hash{0};
        uint64_t LastUse{0};
        uint32_t Pins{0};
        bool Retiring{false};
    };
    struct Slot {
        uint32_t Generation{1};
        unique_ptr<Entry> Value;
    };
    Nullable<Entry*> Find(DescriptorBlockId id) const noexcept;
    void Destroy(uint32_t index);
    render::Device* _device;
    Guid _owner;
    BindingCacheBudget _budget;
    vector<Slot> _slots;
    vector<uint32_t> _free;
    unordered_map<size_t, vector<uint32_t>> _lookup;
    unordered_set<ResourceId, RendererHandleHash<RendererResourceTag>> _retiringResources;
    uint64_t _clock{0}, _idleResources{0}, _idleSamplers{0}, _idleGroups{0};
};
}  // namespace radray
