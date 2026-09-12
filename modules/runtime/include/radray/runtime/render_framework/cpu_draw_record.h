#pragma once

#include <radray/basic_math.h>
#include <radray/hash.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>
#include <radray/types.h>

namespace radray {

class ShaderProgram;
struct RenderSceneSnapshot;
struct MaterialPassRenderData;
struct MaterialRenderData;

struct PassPolicyId {
    uint64_t Value{0};
    bool IsValid() const noexcept { return Value != 0; }
    friend bool operator==(const PassPolicyId&, const PassPolicyId&) = default;
};

enum class StaticBindingRole : uint8_t { View,
                                         Material,
                                         Object,
                                         Pass };
/// Policy-owned interpretation of immutable shader groups; no frame bindings or uploaded offsets.
struct StaticBindingRecipe {
    array<uint32_t, 4> Buffers{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    array<uint32_t, 4> Groups{UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
    array<uint8_t, 4> GroupOrder{0, 1, 2, 3};
    uint8_t GroupCount{0};
    bool Valid{false};
};
struct StaticPassCompileInput {
    const MaterialPassRenderData& Pass;
    const GpuMesh::DrawData& Geometry;
    RenderQueue Queue;
    Nullable<const StaticBindingRecipe*> Bindings{nullptr};
};
struct StaticPassCompileResult {
    MaterialPipelineState NormalState, MirroredState;
    StaticBindingRecipe Bindings;
};
/// Pure CPU callback. Bindings depend only on ProgramGeneration, vertex LayoutId, PolicyId and PolicyRevision.
/// Material identity/values/state/queue and geometry identity/address/range must not affect Bindings.
/// Normal/MirroredState may additionally use static pass state and queue. Never retain input references or read authoring/frame state.
struct PassPolicy {
    PassPolicyId Id;
    uint64_t Revision{0};
    string PassName;
    bool (*CompileStatic)(const StaticPassCompileInput&, StaticPassCompileResult&){nullptr};
    friend bool operator==(const PassPolicy&, const PassPolicy&) = default;
};
struct CpuDrawRange {
    uint32_t First{0}, Count{0};
};
/// Consecutive binding numbers, addressed as a subspan of Geometry.VertexBuffers.
struct CpuVertexBindingRun {
    uint32_t First{0}, Count{0};
    friend bool operator==(const CpuVertexBindingRun&, const CpuVertexBindingRun&) = default;
};
struct CpuGeometryBindingPlan {
    InlineVector<CpuVertexBindingRun, 4> Runs;
};

/// Sparse identity for a live Scene slot. Slot may be reused; Generation must match.
struct SceneObjectId {
    uint32_t Slot{0};
    uint32_t Generation{0};
    bool IsValid() const noexcept { return Generation != 0; }
    friend bool operator==(const SceneObjectId&, const SceneObjectId&) = default;
};

enum class DrawRecordStatus : uint8_t { Ready,
                                        MissingPass,
                                        InvalidBindings,
                                        InvalidGeometry };

/// Long-lived CPU draw description. No frame CB offsets, view pointers, or graph handles.
struct DrawRecord {
    uint32_t Id{0};
    uint32_t Generation{0};
    uint64_t RecipeRevision{0};
    SceneObjectId PrimitiveId{};
    RenderPrimitiveIndex Primitive{0};
    MeshBatchIndex Batch{0};
    RenderMaterialIndex Material{0};
    uint32_t SectionIndex{0};
    uint32_t PassIndex{0};
    uint32_t PassNameHash{0};
    uint32_t ProgramFrameId{0};
    RenderQueue Queue{RenderQueue::Geometry};
    uint32_t LayerMask{0xffffffffu};
    DrawRecordStatus Status{DrawRecordStatus::InvalidGeometry};
    bool Mirrored{false};
    PassPolicyId Policy;
    uint64_t PolicyRevision{0};
    uint32_t BindingRecipe{UINT32_MAX};
    uint32_t GeometryBindingPlan{UINT32_MAX};
    uint64_t NormalStateId{0}, MirroredStateId{0};
    MaterialPipelineState MirroredState;
    MeshDrawDescription Description{};
};

struct CpuDrawStoreStats {
    uint64_t DrawRecordBuilds{0};
    uint64_t DrawRecordsReused{0};
    uint64_t DrawRecordStateSelects{0};
    uint64_t DrawRecordBytes{0};
    uint64_t DrawRecordFullSyncs{0}, DrawRecordPrimitivesVisited{0}, DrawRecordCopies{0};
    uint64_t StaticRecipeCompiles{0}, BindingRecipeCompiles{0};
};

uint32_t HashPassName(std::string_view name) noexcept;
bool IsMirroredAffine(const Eigen::Matrix4f& localToWorld) noexcept;
render::FrontFace OppositeFrontFace(render::FrontFace face) noexcept;

/// Game-thread cache of stable draw records. Sync writes snapshot-local copies; camera-only frames reuse.
class CpuDrawStore {
public:
    /// Complete active set, collected before the Scene cutoff. Repeating the same epoch must supply the identical set.
    bool SetActivePolicies(uint64_t serial, std::span<const PassPolicy> policies);
    bool Sync(RenderSceneSnapshot& scene, RenderValidationMode validation = RenderValidationMode::Full);
    /// GT canonical update. Indices must be unique; set layoutChanged after membership/range moves.
    /// Record count changes also force a layout rebuild, reported by DrawRecordFullSyncs.
    bool SyncChanged(RenderSceneSnapshot& scene, std::span<const uint32_t> changedPrimitives, bool layoutChanged,
                     RenderValidationMode validation = RenderValidationMode::Full);
    const CpuDrawStoreStats& GetStats() const noexcept { return _stats; }
    void ResetCounters() noexcept { _stats = {}; }
    std::span<const CpuDrawRange> ChangedDrawRanges() const noexcept { return _changedDrawRanges; }
    std::span<const CpuDrawRange> ChangedBindingRanges() const noexcept { return _changedBindingRanges; }
    std::span<const CpuDrawRange> ChangedGeometryRanges() const noexcept { return _changedGeometryRanges; }
    size_t GetActiveStateCount() const noexcept { return _states.size(); }
    size_t GetActiveGeometryPlanCount() const noexcept { return _geometryIndices.size(); }
    size_t GetActiveLayoutCount() const noexcept { return _layouts.Size(); }
    RenderMemoryStats GetMemoryStats() const noexcept;

private:
    struct Key {
        uint64_t PrimitiveGeneration{0};
        uint32_t SectionIndex{0};
        string PassName;
        PassPolicyId Policy;
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyRef {
        uint64_t PrimitiveGeneration{0};
        uint32_t SectionIndex{0};
        std::string_view PassName;
        PassPolicyId Policy;
    };
    struct KeyHash {
        using is_transparent = void;
        size_t operator()(const Key& key) const noexcept;
        size_t operator()(const KeyRef& key) const noexcept;
    };
    struct KeyEqual {
        using is_transparent = void;
        bool operator()(const Key& lhs, const Key& rhs) const noexcept { return lhs == rhs; }
        bool operator()(const Key& lhs, const KeyRef& rhs) const noexcept;
        bool operator()(const KeyRef& lhs, const Key& rhs) const noexcept { return (*this)(rhs, lhs); }
    };
    struct Cached {
        uint64_t Epoch{0};
        uint64_t MaterialGeneration{0}, MaterialStructureRevision{0};
        uint64_t GeometryRevision{0}, ProgramGeneration{0};
        uint64_t PolicyRevision{0};
        Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
        Nullable<ShaderProgram*> Program{nullptr};
        MaterialPipelineState PipelineState{};
        uint32_t FirstIndex{0}, IndexCount{0};
        int32_t VertexOffset{0};
        RenderQueue Queue{RenderQueue::Geometry};
        DrawRecordStatus Status{DrawRecordStatus::InvalidGeometry};
        DrawRecord Record{};
    };

    unordered_map<Key, Cached, KeyHash, KeyEqual> _cache;
    struct PrimitiveKeys {
        uint64_t Epoch{0};
        uint32_t Packed{0};
        vector<Key> Keys;
    };
    unordered_map<uint64_t, PrimitiveKeys> _primitiveKeys;
    struct BindingKey {
        uint64_t ProgramGeneration{0}, Layout{0}, Policy{0}, Revision{0};
        Nullable<ShaderProgram*> Program{nullptr};
        friend bool operator==(const BindingKey&, const BindingKey&) = default;
    };
    struct BindingKeyHash {
        size_t operator()(const BindingKey& key) const noexcept;
    };
    unordered_map<BindingKey, uint32_t, BindingKeyHash> _bindings;
    vector<std::optional<BindingKey>> _bindingKeys;
    vector<StaticBindingRecipe> _bindingData;
    vector<uint32_t> _bindingUsers, _freeBindings;
    vector<uint8_t> _bindingChanged;
    vector<uint32_t> _bindingTouched;
    struct StateEntry {
        MaterialPipelineState State;
        uint32_t Users{0};
        size_t Hash{0};
    };
    unordered_map<uint64_t, StateEntry> _states;
    unordered_map<size_t, vector<uint64_t>> _stateBuckets;
    struct GeometryEntry {
        Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
        uint64_t Epoch{0};
        uint32_t Users{0};
    };
    unordered_map<const GpuMesh::DrawData*, uint32_t> _geometryIndices;
    vector<GeometryEntry> _geometryEntries;
    vector<CpuGeometryBindingPlan> _geometryPlans;
    vector<uint32_t> _freeGeometry, _geometryTouched;
    vector<uint8_t> _geometryChanged;
    vector<CpuDrawRange> _changedGeometryRanges;
    unordered_map<uint64_t, unordered_map<uint64_t, uint32_t>> _policyUsers;
    vector<PassPolicy> _policies;
    vector<uint64_t> _dirtyPolicies;
    std::optional<uint64_t> _policySerial;
    bool _policyMembershipChanged{false};
    vector<CpuDrawRange> _changedDrawRanges, _changedBindingRanges;
    vector<uint32_t> _syncPrimitives;
    vector<uint8_t> _syncMarked;
    PrimitiveVertexLayoutRegistry _layouts;
    bool SyncPrimitive(const RenderSceneSnapshot& scene, uint32_t primitiveIndex, vector<DrawRecord>& records, size_t firstRecord);
    void WriteStats(RenderSceneSnapshot& scene) noexcept;
    void ReleaseCached(const Key& key, const Cached& cached);
    void ReleaseBinding(uint32_t index);
    void MarkBinding(uint32_t index);
    void PublishBindings(RenderSceneSnapshot& scene);
    uint64_t AcquireState(const MaterialPipelineState& state);
    void ReleaseState(uint64_t id);
    uint32_t AcquireGeometry(Nullable<const GpuMesh::DrawData*> geometry);
    void ReleaseGeometry(uint32_t index);
    void MarkGeometry(uint32_t index);
    size_t RecordCount(const MaterialRenderData& material) const noexcept;
    uint64_t _epoch{0};
    uint32_t _nextId{1};
    CpuDrawStoreStats _stats;
};

}  // namespace radray
