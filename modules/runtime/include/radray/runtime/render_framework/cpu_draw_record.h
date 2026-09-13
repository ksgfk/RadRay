#pragma once

#include <radray/basic_math.h>
#include <radray/hash.h>
#include <radray/runtime/render_framework/mesh_draw_command.h>
#include <radray/runtime/render_framework/mesh_binding_contract.h>
#include <radray/runtime/render_framework/render_graph_runtime_options.h>
#include <radray/types.h>

namespace radray {

class ShaderProgram;
struct RenderSceneSnapshot;
struct MaterialPassRenderData;
struct MaterialRenderData;
struct RenderPrimitiveData;
struct ResolvedRenderView;

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
    MeshBindingPlan Parameters;
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
    uint64_t Configuration{0};
};
struct StaticPassCompileResult {
    MaterialPipelineState NormalState, MirroredState;
    StaticBindingRecipe Bindings;
};
enum class MeshStaticCompileStatus : uint8_t { Ready,
                                               Filtered,
                                               IncompatibleProgram,
                                               InvalidGeometry };
struct MeshStaticDrawCompileInput {
    const MaterialPassRenderData& Pass;
    const MaterialRenderData& Material;
    const MeshBatch& Batch;
    const RenderPrimitiveData& Primitive;
    uint32_t PassIndex{0};
    uint64_t Configuration{0};
};
/// Initialized from the anchor pass and mesh. Program selection refers to another frozen material pass.
struct MeshStaticDrawCompileResult {
    uint32_t ProgramPassIndex{0};
    Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
    uint32_t FirstIndex{0}, IndexCount{0};
    int32_t VertexOffset{0};
    MaterialPipelineState NormalState, MirroredState;
};
/// Pure CPU callbacks over frozen inputs and declared dependencies. Never retain input references or read authoring/frame state.
/// BindingContract is program-only. Legacy CompileStatic bindings may additionally depend on vertex layout and policy identity.
struct PassPolicy {
    PassPolicyId Id;
    uint64_t Revision{0};
    string PassName;
    bool (*CompileStatic)(const StaticPassCompileInput&, StaticPassCompileResult&){nullptr};
    MeshBindingContract BindingContract{};
    uint64_t Configuration{0};
    MeshPassDependencies Dependencies{};
    MeshPassCacheMode CacheMode{MeshPassCacheMode::OnChange};
    MeshStaticCompileStatus (*CompileMesh)(const MeshStaticDrawCompileInput&, MeshStaticDrawCompileResult&){nullptr};
    /// PerView policies use this callback exclusively, after culling. Scene publication never calls it.
    MeshStaticCompileStatus (*CompileView)(const MeshStaticDrawCompileInput&, const ResolvedRenderView&, MeshStaticDrawCompileResult&){nullptr};
    bool HasValidCompiler() const noexcept {
        if (CacheMode == MeshPassCacheMode::PerView)
            return CompileView && !CompileStatic && !CompileMesh && BindingContract.IsValid();
        return !CompileView && (bool(CompileStatic) != bool(CompileMesh)) && (!CompileMesh || BindingContract.IsValid());
    }
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
struct CpuVertexInputPlan;
/// Geometry, range and vertex compatibility shared independently of binding and state policies.
struct CpuGeometryBindingPlan {
    Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
    PrimitiveVertexLayoutId LayoutId{};
    uint32_t FirstIndex{0}, IndexCount{0};
    int32_t VertexOffset{0};
    shared_ptr<const CpuVertexInputPlan> VertexInput;
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
                                        InvalidGeometry,
                                        Filtered };

/// Shared immutable description; membership and view selection remain in DrawRecord.
struct CpuVertexInputPlan {
    uint64_t ProgramGeneration{0};
    PrimitiveVertexLayoutId Layout;
    std::optional<ResolvedPrimitiveVertexLayout> Input;
};
struct CpuDrawPlan {
    Nullable<ShaderProgram*> Program{nullptr};
    uint32_t Geometry{UINT32_MAX}, Binding{UINT32_MAX};
    uint32_t NormalState{UINT32_MAX}, MirroredState{UINT32_MAX};
};

struct CpuStatePlan {
    MaterialPipelineState State;
    uint64_t Id{0};
};

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
    uint64_t PolicyRevision{0}, PolicyConfiguration{0};
    uint32_t BindingRecipe{UINT32_MAX};
    uint32_t GeometryBindingPlan{UINT32_MAX};
    uint64_t NormalStateId{0}, MirroredStateId{0};
    uint32_t Plan{UINT32_MAX};
};

struct ResolvedDrawView {
    const DrawRecord& Record;
    MeshDrawDescriptionView Description;
    const MaterialPipelineState& MirroredState;
};

struct CpuDrawStoreStats {
    uint64_t VertexInputCompiles{0};
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
    std::span<const CpuDrawRange> ChangedStateRanges() const noexcept { return _changedStateRanges; }
    std::span<const CpuDrawRange> ChangedPlanRanges() const noexcept { return _changedPlanRanges; }
    size_t GetActiveDrawPlanCount() const noexcept { return _drawPlanIndices.size(); }
    size_t GetActiveStateCount() const noexcept { return _states.size(); }
    size_t GetActiveVertexInputCount() const noexcept { return _vertexInputs.size(); }
    size_t GetActiveGeometryPlanCount() const noexcept { return _geometryIndices.size(); }
    size_t GetActiveLayoutCount() const noexcept { return _layouts.Size(); }
    bool UsesMaterialDependency(MeshPassDependency dependency) const noexcept;
    RenderMemoryStats GetMemoryStats() const noexcept;

private:
    struct Key {
        uint64_t PrimitiveGeneration{0};
        uint32_t SectionIndex{0};
        string PassName;
        PassPolicyId Policy;
        uint64_t Configuration{0};
        friend bool operator==(const Key&, const Key&) = default;
    };
    struct KeyRef {
        uint64_t PrimitiveGeneration{0};
        uint32_t SectionIndex{0};
        std::string_view PassName;
        PassPolicyId Policy;
        uint64_t Configuration{0};
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
        uint64_t MaterialValuesRevision{0}, MaterialBindingsRevision{0}, MaterialReadinessRevision{0};
        uint64_t GeometryRevision{0}, ProgramGeneration{0};
        uint64_t PolicyRevision{0};
        uint64_t SelectedProgramGeneration{0}, TransformRevision{0}, MotionRevision{0};
        uint32_t LayerMask{0};
        bool SelectedReady{false};
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
        uint64_t Configuration{0};
        bool Contract{false};
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
        uint32_t Index{UINT32_MAX};
    };
    vector<CpuStatePlan> _statePlans;
    vector<uint32_t> _freeStates, _stateTouched;
    vector<uint8_t> _stateChanged;
    vector<CpuDrawRange> _changedStateRanges;
    void MarkState(uint32_t index);
    unordered_map<uint64_t, StateEntry> _states;
    unordered_map<size_t, vector<uint64_t>> _stateBuckets;
    struct GeometryKey {
        Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
        uint64_t Layout{0}, VertexProgram{0};
        uint32_t FirstIndex{0}, IndexCount{0};
        int32_t VertexOffset{0};
        friend bool operator==(const GeometryKey&, const GeometryKey&) = default;
    };
    struct GeometryKeyHash {
        size_t operator()(const GeometryKey& key) const noexcept;
    };
    struct GeometryEntry {
        GeometryKey Key;
        uint64_t Epoch{0};
        uint32_t Users{0};
    };
    unordered_map<GeometryKey, uint32_t, GeometryKeyHash> _geometryIndices;
    vector<GeometryEntry> _geometryEntries;
    vector<CpuGeometryBindingPlan> _geometryPlans;
    vector<uint32_t> _freeGeometry, _geometryTouched;
    vector<uint8_t> _geometryChanged;
    vector<CpuDrawRange> _changedGeometryRanges;
    struct DrawPlanKey {
        uint64_t ProgramGeneration{0}, Layout{0}, NormalState{0}, MirroredState{0};
        Nullable<ShaderProgram*> Program{nullptr};
        Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
        uint32_t Binding{UINT32_MAX}, GeometryPlan{UINT32_MAX}, FirstIndex{0}, IndexCount{0};
        int32_t VertexOffset{0};
        friend bool operator==(const DrawPlanKey&, const DrawPlanKey&) = default;
    };
    struct DrawPlanKeyHash {
        size_t operator()(const DrawPlanKey& key) const noexcept;
    };
    unordered_map<DrawPlanKey, uint32_t, DrawPlanKeyHash> _drawPlanIndices;
    vector<DrawPlanKey> _drawPlanKeys;
    vector<CpuDrawPlan> _drawPlans;
    vector<uint32_t> _drawPlanUsers, _freeDrawPlans, _drawPlanTouched;
    vector<uint8_t> _drawPlanChanged;
    vector<CpuDrawRange> _changedPlanRanges;
    struct VertexInputKey {
        uint64_t Program{0}, Layout{0};
        friend bool operator==(const VertexInputKey&, const VertexInputKey&) = default;
    };
    struct VertexInputKeyHash {
        size_t operator()(const VertexInputKey& key) const noexcept {
            HashCode hash;
            hash.Add(key.Program);
            hash.Add(key.Layout);
            return hash.ToHashCode();
        }
    };
    struct VertexInputEntry {
        shared_ptr<const CpuVertexInputPlan> Plan;
        uint32_t Users{0};
    };
    unordered_map<VertexInputKey, VertexInputEntry, VertexInputKeyHash> _vertexInputs;
    shared_ptr<const CpuVertexInputPlan> ResolveVertexInput(const MeshDrawDescription& draw, uint64_t programGeneration);
    uint32_t AcquireDrawPlan(const DrawRecord& record, const MeshDrawDescription& draw, uint64_t programGeneration);
    void ReleaseDrawPlan(uint32_t index);
    void MarkDrawPlan(uint32_t index);
    struct PolicyKey {
        PassPolicyId Id;
        uint64_t Configuration{0};
        friend bool operator==(const PolicyKey&, const PolicyKey&) = default;
    };
    struct PolicyKeyHash {
        size_t operator()(const PolicyKey& key) const noexcept {
            HashCode hash;
            hash.Add(key.Id.Value);
            hash.Add(key.Configuration);
            return hash.ToHashCode();
        }
    };
    unordered_map<PolicyKey, unordered_map<uint64_t, uint32_t>, PolicyKeyHash> _policyUsers;
    vector<PassPolicy> _policies;
    shared_ptr<const vector<PassPolicy>> _policyVersion;
    vector<PolicyKey> _dirtyPolicies;
    std::optional<uint64_t> _policySerial;
    bool _policyMembershipChanged{false};
    vector<CpuDrawRange> _changedDrawRanges, _changedBindingRanges;
    vector<uint32_t> _syncPrimitives;
    vector<uint8_t> _syncMarked;
    PrimitiveVertexLayoutRegistry _layouts;
    bool SyncPrimitive(const RenderSceneSnapshot& scene, uint32_t primitiveIndex, vector<DrawRecord>& records, size_t firstRecord);
    void WriteStats(RenderSceneSnapshot& scene) noexcept;
    void InvalidateCompiled(RenderSceneSnapshot& scene) noexcept;
    void ReleaseCached(const Key& key, const Cached& cached);
    void ReleaseBinding(uint32_t index);
    void MarkBinding(uint32_t index);
    void PublishBindings(RenderSceneSnapshot& scene);
    uint64_t AcquireState(const MaterialPipelineState& state);
    void ReleaseState(uint64_t id);
    uint32_t AcquireGeometry(const MeshDrawDescription& draw, shared_ptr<const CpuVertexInputPlan> vertexInput);
    void ReleaseGeometry(uint32_t index);
    void MarkGeometry(uint32_t index);
    size_t RecordCount(const MaterialRenderData& material) const noexcept;
    uint64_t _epoch{0};
    uint32_t _nextId{1};
    CpuDrawStoreStats _stats;
};

}  // namespace radray
