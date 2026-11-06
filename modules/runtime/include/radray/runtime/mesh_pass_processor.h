#pragma once

#include <optional>
#include <span>

#include <radray/nullable.h>
#include <radray/sparse_set.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/material.h>
#include <radray/runtime/mesh_batch.h>
#include <radray/runtime/mesh_draw_command.h>

namespace radray {

// The caller resolves handles and supplies matching records. References are borrowed only during AddMeshBatch.
// The processor must not mutate the input records or their owning Scene.
struct MeshBatchContext {
    const PrimitiveSceneInfo& Primitive;
    const MeshBatch& Batch;
    SparseSetHandle PrimitiveHandle{};
    SparseSetHandle BatchHandle{};
    Nullable<const MaterialRenderProxy*> Material{nullptr};
};

struct MeshDrawCommandSharedState {
    render::GraphicsPipelineState* Pipeline;
    uint8_t StencilReference{0};
    uint64_t BaseSortKey{0};
    std::span<const MeshShaderParameterBinding> ParameterSets{};
    std::optional<MeshPushConstantBinding> PushConstants{};
    bool AllowDeferredInstanceStream{false};
};

class MeshPassDrawListContext {
public:
    virtual ~MeshPassDrawListContext() noexcept = default;

    virtual MeshDrawCommandBindingStorage& GetBindingStorage() noexcept = 0;

    // Transfers exclusive payload ownership from the initializer; the context publishes baseSortKey at Finalize.
    // The reference must survive until FinalizeCommand returns.
    // The caller must finalize each command before adding another; references need not survive later additions.
    virtual MeshDrawCommand& AddCommand(MeshDrawCommand& initializer, uint64_t baseSortKey = 0) = 0;

    // Fills the inline primary element. Overrides call this before publishing output.
    // elementIndex must be the lowest valid bit; the initializer already contains the mask and extra elements.
    virtual void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command);
};

class CachedPassMeshDrawListContext final : public MeshPassDrawListContext {
public:
    CachedPassMeshDrawListContext(MeshPassId passId, CachedPassMeshDrawList& passDrawList, MeshBatch& targetBatch) noexcept;
    MeshDrawCommandBindingStorage& GetBindingStorage() noexcept override;
    MeshDrawCommand& AddCommand(MeshDrawCommand& initializer, uint64_t baseSortKey = 0) override;
    void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) override;

private:
    MeshPassId _passId;
    CachedPassMeshDrawList* _passDrawList;
    MeshBatch* _targetBatch;
    SparseSetHandle _pendingHandle{};
    uint64_t _pendingSortKey{0};
};

// Commands and their visible pointers are frame-local; Clear invalidates all prior output.
class DynamicMeshPassDrawListContext final : public MeshPassDrawListContext {
public:
    DynamicMeshPassDrawListContext() noexcept = default;
    explicit DynamicMeshPassDrawListContext(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept;
    MeshDrawCommandBindingStorage& GetBindingStorage() noexcept override;
    // Rebinding must preserve the emitted entries at their original positions (e.g. after a list move).
    void BindVisibleCommands(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept;
    MeshDrawCommand& AddCommand(MeshDrawCommand& initializer, uint64_t baseSortKey = 0) override;
    void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) override;
    // Call after all commands are finalized, before sorting, copying or submitting visible entries.
    // Publishes stable pointers once; further command additions require Clear and a fresh visible list.
    void FinalizeVisibleCommands() noexcept;
    void Clear() noexcept;

    // Do not resize, reserve, erase or replace storage after FinalizeVisibleCommands until Clear.
    vector<MeshDrawCommand> Commands;

private:
    // Stable identity across moves of the owning frame draw list.
    unique_ptr<MeshDrawCommandBindingStorage> _bindings;

    struct PendingVisibleCommand {
        size_t CommandIndex;
        size_t VisibleIndex;
    };

    Nullable<vector<VisibleMeshDrawCommand>*> _visibleCommands{nullptr};
    vector<PendingVisibleCommand> _pendingVisibleCommands;
    uint64_t _pendingSortKey{0};
    bool _finalized{false};
};

class MeshPassProcessor {
public:
    // The default output context must outlive this processor. Rebinding is borrowed until restored.
    explicit MeshPassProcessor(MeshPassDrawListContext& drawListContext) noexcept;
    virtual ~MeshPassProcessor() noexcept = default;
    void SetDrawListContext(MeshPassDrawListContext& drawListContext) noexcept;
    MeshPassDrawListContext& DrawListContext() noexcept { return *_drawListContext; }
    // Emits at most one batch command into the output context. Returning without emitting skips the batch.
    virtual void AddMeshBatch(const MeshBatchContext& context) = 0;

protected:
    bool BuildMeshDrawCommands(const MeshBatchContext& context, const MeshDrawCommandSharedState& sharedState);

    MeshPassDrawListContext* _drawListContext;
};

struct MeshPassRegistration {
    MeshPassId Pass{};
    MeshPassProcessor* Processor;
};

}  // namespace radray
