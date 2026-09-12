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
    uint64_t SharedBufferUploads{0}, SharedBufferHits{0};
    uint64_t SharedGroupHits{0};
};

struct FrameShaderGroupId {
    uint32_t Value{UINT32_MAX};
    bool IsValid() const noexcept { return Value != UINT32_MAX; }
    friend bool operator==(const FrameShaderGroupId&, const FrameShaderGroupId&) = default;
};
struct FrameDrawBindingId {
    uint32_t Value{UINT32_MAX};
    bool IsValid() const noexcept { return Value != UINT32_MAX; }
    friend bool operator==(const FrameDrawBindingId&, const FrameDrawBindingId&) = default;
};

/// Values identified by (source, wire ABI, context, row) must be immutable until BeginFrame.
/// Different temporal views use different contexts; a byte count alone never identifies a wire ABI.
struct FrameCBufferIdentity {
    Nullable<const void*> Source{nullptr}, WireType{nullptr};
    uint64_t Context{0};
    uint64_t Generation{0}, Revision{0}, HistoryRevision{0};
    Nullable<const void*> HistoryOwner{nullptr};
    friend bool operator==(const FrameCBufferIdentity&, const FrameCBufferIdentity&) = default;
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
    uint64_t GetEpoch() const noexcept { return _bufferEpoch; }
    size_t GetGroupCount() const noexcept { return _readyGroups.size(); }
    std::span<const PreparedShaderGroup> GetGroups() const noexcept { return _readyGroups; }
    size_t GetBindingCount() const noexcept { return _drawBindings.size(); }
    /// CPU cache storage high-water mark; excludes driver-owned descriptor allocations and the upload arena.
    size_t GetCacheCapacityBytes() const noexcept;
    bool IsValid(FrameDrawBindingId id) const noexcept { return id.Value < _drawBindings.size(); }
    /// Immutable group tuples are shared across lists and state-only policies within this epoch.
    FrameDrawBindingId InternBinding(std::span<const FrameShaderGroupId> groups);
    std::span<const FrameShaderGroupId> GetBinding(FrameDrawBindingId id) const noexcept { return _drawBindings[id.Value]; }
    /// Intern a small set of packed view values once at each view boundary, with exact byte equality.
    FrameCBufferIdentity InternValues(const void* wireType, std::span<const byte> bytes);
    bool HasCBufferValues(FrameCBufferIdentity identity, uint32_t row) const noexcept;
    /// Same single-cbuffer sharing contract, with immutable material textures/samplers included in identity.
    /// Empty bytes may reuse an already uploaded row. Failed native preparation never publishes an ID.
    FrameShaderGroupId PrepareGroupId(
        ShaderProgram& program, uint32_t group, FrameCBufferIdentity identity, uint32_t row, std::span<const byte> bytes,
        std::span<const MaterialTextureFrameData> textures = {}, std::span<const MaterialSamplerFrameData> samplers = {});
    void FailNextGroupForTesting() noexcept { _failNextGroup = true; }

    std::optional<PreparedShaderGroup> PrepareGroup(
        ShaderProgram& program, uint32_t group, const ShaderParameterStorage& parameters,
        std::span<const MaterialTextureFrameData> textures = {}, std::span<const MaterialSamplerFrameData> samplers = {});

    std::optional<PreparedShaderGroup> PrepareGroup(
        ShaderProgram& program, uint32_t group, std::span<const byte> bufferBytes,
        std::span<const MaterialTextureFrameData> textures = {}, std::span<const MaterialSamplerFrameData> samplers = {});

    /// Shares an upload slice across compatible wire consumers while keeping native sets layout-specific.
    /// The group must contain exactly one cbuffer and no texture or sampler bindings.
    std::optional<PreparedShaderGroup> PrepareSharedCBufferGroup(
        ShaderProgram& program, uint32_t group, FrameCBufferIdentity identity, uint32_t row, std::span<const byte> bytes);

    /// Indexed form for visible draws. IDs survive table growth until BeginFrame/ClearSets;
    /// references returned by GetGroup are only stable after all preparation has finished.
    FrameShaderGroupId PrepareSharedCBufferGroupId(
        ShaderProgram& program, uint32_t group, FrameCBufferIdentity identity, uint32_t row, std::span<const byte> bytes);
    /// The caller supplies a successful ID from this flight's current preparation.
    const PreparedShaderGroup& GetGroup(FrameShaderGroupId id) const noexcept { return _readyGroups[id.Value]; }

    /// Pre-uploaded bindings use the same exact-tuple cache; dynamic offsets are supplied at bind time.
    Nullable<render::ShaderParameterSet*> PrepareSet(
        ShaderProgram& program, uint32_t group, std::span<const FrameBufferBinding> buffers,
        std::span<const MaterialTextureFrameData> textures = {}, std::span<const MaterialSamplerFrameData> samplers = {});

private:
    friend std::optional<PreparedRendererList> PrepareRendererList(const RendererList&, RenderGraphPrepareContext&, Nullable<const RendererListPassSets*>);
    PreparedRendererList::Workspace& GetPreparationWorkspace() const;
    /// Open-addressed indices retain bucket storage across epochs; keys live in reusable owner tables.
    class Lookup {
    public:
        void Reset() noexcept;
        template <class Predicate>
        uint32_t Find(size_t hash, Predicate&& matches) const noexcept {
            if (_slots.empty()) return UINT32_MAX;
            size_t slot = hash & (_slots.size() - 1);
            while (_slots[slot].Epoch == _epoch) {
                if (_slots[slot].Hash == hash && matches(_slots[slot].Index)) return _slots[slot].Index;
                slot = (slot + 1) & (_slots.size() - 1);
            }
            return UINT32_MAX;
        }
        void Insert(size_t hash, uint32_t index);
        size_t CapacityBytes() const noexcept { return _slots.capacity() * sizeof(Slot); }

    private:
        struct Slot {
            uint64_t Epoch{0};
            size_t Hash{0};
            uint32_t Index{0};
        };
        vector<Slot> _slots;
        uint64_t _epoch{1};
        size_t _count{0};
    };
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
    bool UploadBuffer(
        const ShaderParameterBufferLayout& buffer, uint32_t index, bool dynamic, std::span<const byte> bytes,
        vector<FrameBufferBinding>& bindings, PreparedShaderGroup& out);
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
    struct SharedBufferSlice {
        uint64_t Epoch{0};
        Nullable<render::Buffer*> Target{nullptr};
        uint32_t Offset{0}, Size{0};
    };
    struct SharedBufferDomain {
        FrameCBufferIdentity Identity;
        vector<SharedBufferSlice> Rows;
    };
    struct NativeGroup {
        uint64_t ProgramGeneration{0};
        uint32_t Group{0}, Domain{0}, Row{0};
        FrameShaderGroupId Id;
        size_t TextureCount{0}, SamplerCount{0};
    };
    struct StoredValues {
        Nullable<const void*> Wire{nullptr};
        vector<byte> Bytes;
    };

    render::Device* _device;
    DynamicCBufferArena::Descriptor _descriptor;
    Nullable<HostWriteBatch*> _hostWrites{nullptr};
    unique_ptr<DynamicCBufferArena> _arena;
    // Sets must be destroyed before their arena buffers.
    vector<unique_ptr<render::ShaderParameterSet>> _sets;
    struct CachedSet {
        FrameSetKey Key{};
        Nullable<render::ShaderParameterSet*> Set{nullptr};
    };
    vector<CachedSet> _setCache;
    size_t _activeSets{0};
    Lookup _setLookup, _domainLookup, _nativeLookup, _bindingLookup, _valueLookup;
    vector<DynamicOnlySet> _dynamicOnlySets;
    size_t _lastDynamicOnlySet{0};
    vector<FrameBufferBinding> _bindingScratch;
    FrameSetKey _keyScratch{};
    vector<SharedBufferDomain> _sharedBufferDomains;
    size_t _activeBufferDomains{0};
    uint64_t _bufferEpoch{0};
    vector<PreparedShaderGroup> _readyGroups;
    vector<NativeGroup> _nativeGroups;
    vector<InlineVector<FrameShaderGroupId, 3>> _drawBindings;
    vector<StoredValues> _values;
    size_t _activeValues{0};
    bool _failNextGroup{false};
    FrameDrawResourceStats _stats;
    mutable unique_ptr<PreparedRendererList::Workspace> _preparation;
};

}  // namespace radray
