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
/// Interned backend compatibility ids (`_compatibilities`) are never reclaimed: their count is bounded by
/// the distinct group layouts observed on the owner, and a JIT reload that re-registers layouts can add
/// more. This is deliberate — layouts are treated as immutable for the owner's lifetime.
class BindingRegistry {
public:
    BindingRegistry(render::Device* device, Guid owner);
    ~BindingRegistry() noexcept;
    BindingRegistry(const BindingRegistry&) = delete;
    BindingRegistry& operator=(const BindingRegistry&) = delete;
    std::optional<DescriptorBlockId> Resolve(render::PipelineLayout* layout, uint32_t group,
                                             std::span<const ParameterWrite> writes, std::span<const ResourceId> dependencies,
                                             std::span<const uint32_t> elementIndices = {}, uint32_t compatibility = UINT32_MAX);
    /// Interns the group's backend compatibility into a small owner-local id. Returns UINT32_MAX when unavailable.
    uint32_t InternCompatibility(render::PipelineLayout* layout, uint32_t group);
    bool Retain(DescriptorBlockId id);
    /// Retains one frame use; `newly` reports whether this stamp was not already pinned. False when unavailable.
    bool Retain(DescriptorBlockId id, uint64_t stamp, bool& newly);
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
        uint32_t Compatibility{UINT32_MAX};
        /// Flattened identity words with `ValueOffsets` marking each element's half-open range.
        vector<uint64_t> Values;
        vector<uint32_t> ValueOffsets;
        vector<ResourceId> Dependencies;
        unique_ptr<render::ShaderParameterSet> Parameters;
        size_t Hash{0};
        uint64_t LastUse{0}, LastStamp{0};
        uint32_t Pins{0};
        uint32_t Resources{0}, Samplers{0};
        bool Retiring{false};
    };
    struct Compatibility {
        render::ParameterGroupCompatibility Value;
        size_t Hash{0};
    };
    struct Slot {
        uint32_t Generation{1};
        unique_ptr<Entry> Value;
    };
    uint32_t Intern(const render::ParameterGroupCompatibility& compatibility);
    Nullable<Entry*> Find(DescriptorBlockId id) const noexcept;
    void Destroy(uint32_t index);
    render::Device* _device;
    Guid _owner;
    BindingCacheBudget _budget;
    vector<Slot> _slots;
    vector<uint32_t> _free;
    vector<Compatibility> _compatibilities;
    unordered_map<size_t, vector<uint32_t>> _compatLookup;
    unordered_map<size_t, vector<uint32_t>> _lookup;
    /// Resource to number of live blocks depending on it; makes HasResource O(1).
    unordered_map<ResourceId, uint32_t, RendererHandleHash<RendererResourceTag>> _dependents;
    unordered_set<ResourceId, RendererHandleHash<RendererResourceTag>> _retiringResources;
    uint64_t _clock{0}, _idleResources{0}, _idleSamplers{0}, _idleGroups{0};
};
}  // namespace radray
