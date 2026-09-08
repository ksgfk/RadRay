#pragma once

#include <array>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>

namespace radray {

struct ShaderParameterGroupRecipe;

struct FrameBufferBinding {
    uint32_t BufferIndex{0};
    render::ShaderBufferBinding Value;
    friend bool operator==(const FrameBufferBinding&, const FrameBufferBinding&) = default;
};
struct FrameDrawResourceStats {
    // RecipeBuilds counts only recipes first materialized in their owning ShaderProgram by this frame.
    uint64_t GroupPreparations{0}, RecipeBuilds{0}, SetCacheHits{0}, SetCreations{0}, BufferBytesCopied{0};
};

/// Render-thread, per-flight arena and immutable descriptor sets. Clear list references before BeginFrame.
class FrameDrawResources {
public:
    explicit FrameDrawResources(render::Device* device, DynamicCBufferArena::Descriptor descriptor = {});
    ~FrameDrawResources() noexcept;
    FrameDrawResources(const FrameDrawResources&) = delete;
    FrameDrawResources& operator=(const FrameDrawResources&) = delete;

    bool BeginFrame(HostWriteBatch& hostWrites) noexcept;
    void ClearSets() noexcept;
    size_t GetSetCount() const noexcept { return _sets.size(); }
    const FrameDrawResourceStats& GetStats() const noexcept { return _stats; }

    std::optional<PreparedShaderGroup> PrepareGroup(
        ShaderProgram& program, uint32_t group, const ShaderParameterStorage& parameters,
        std::span<const MaterialTextureFrameData> textures = {}, std::span<const MaterialSamplerFrameData> samplers = {});

    /// Pre-uploaded bindings use the same exact-tuple cache; dynamic offsets are supplied at bind time.
    Nullable<render::ShaderParameterSet*> PrepareSet(
        ShaderProgram& program, uint32_t group, std::span<const FrameBufferBinding> buffers,
        std::span<const MaterialTextureFrameData> textures = {}, std::span<const MaterialSamplerFrameData> samplers = {});

private:
    struct TextureBinding {
        uint32_t Parameter, Element;
        render::TextureView* View;
        friend bool operator==(const TextureBinding&, const TextureBinding&) = default;
    };
    struct SamplerBinding {
        uint32_t Parameter, Element;
        render::Sampler* Sampler;
        friend bool operator==(const SamplerBinding&, const SamplerBinding&) = default;
    };
    struct FrameSetKey {
        render::PipelineLayout* Layout;
        uint32_t Group;
        vector<FrameBufferBinding> Buffers;
        vector<TextureBinding> Textures;
        vector<SamplerBinding> Samplers;
        friend bool operator==(const FrameSetKey&, const FrameSetKey&) = default;
    };
    struct FrameSetKeyHash {
        size_t operator()(const FrameSetKey& key) const noexcept;
    };
    const ShaderParameterGroupRecipe& GetRecipe(ShaderProgram& program, uint32_t group);
    Nullable<render::ShaderParameterSet*> PrepareSetForGroup(
        ShaderProgram& program, uint32_t group, const ShaderParameterGroupRecipe& recipe, std::span<const FrameBufferBinding> buffers,
        std::span<const MaterialTextureFrameData> textures, std::span<const MaterialSamplerFrameData> samplers);

    // Sets for groups made only of dynamic constant buffers, keyed by (layout, group, arena block per buffer).
    // Such groups are re-prepared per object each frame; this is a linear scan over a few entries and avoids
    // building and hashing the generic FrameSetKey for every call.
    static constexpr uint32_t kDynamicOnlyTargets = 4;
    struct DynamicOnlySet {
        render::PipelineLayout* Layout;
        uint32_t Group, Count;
        std::array<render::Buffer*, kDynamicOnlyTargets> Targets;
        std::array<uint32_t, kDynamicOnlyTargets> Indices;
        render::ShaderParameterSet* Set;
    };

    render::Device* _device;
    DynamicCBufferArena::Descriptor _descriptor;
    Nullable<HostWriteBatch*> _hostWrites{nullptr};
    unique_ptr<DynamicCBufferArena> _arena;
    // Sets must be destroyed before their arena buffers.
    vector<unique_ptr<render::ShaderParameterSet>> _sets;
    unordered_map<FrameSetKey, render::ShaderParameterSet*, FrameSetKeyHash> _setCache;
    vector<DynamicOnlySet> _dynamicOnlySets;
    vector<FrameBufferBinding> _bindingScratch;
    FrameSetKey _keyScratch{};
    FrameDrawResourceStats _stats;
};

}  // namespace radray
