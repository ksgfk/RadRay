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

    // Copies the shared initializer into output storage. The reference must survive until FinalizeCommand returns.
    // The caller must finalize each command before adding another; references need not survive later additions.
    virtual MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) = 0;

    // Fills the selected Element's indexed draw arguments. Overrides call this before adding their own output data.
    // context is borrowed for this call; elementIndex must identify an existing Element.
    virtual void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command);
};

class CachedPassMeshDrawListContext final : public MeshPassDrawListContext {
public:
    CachedPassMeshDrawListContext(MeshPassId passId, CachedPassMeshDrawList& passDrawList, MeshBatch& targetBatch) noexcept;
    MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) override;
    void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) override;

private:
    MeshPassId _passId;
    CachedPassMeshDrawList* _passDrawList;
    MeshBatch* _targetBatch;
    SparseSetHandle _pendingHandle{};
};

// Commands and their handles are frame-local; Clear invalidates all prior output.
class DynamicMeshPassDrawListContext final : public MeshPassDrawListContext {
public:
    DynamicMeshPassDrawListContext() noexcept = default;
    explicit DynamicMeshPassDrawListContext(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept;
    void BindVisibleCommands(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept;
    MeshDrawCommand& AddCommand(const MeshDrawCommand& initializer) override;
    void FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) override;
    void Clear() noexcept;

    vector<MeshDrawCommand> Commands;

private:
    Nullable<vector<VisibleMeshDrawCommand>*> _visibleCommands{nullptr};
};

class MeshPassProcessor {
public:
    // The default output context must outlive this processor. Rebinding is borrowed until restored.
    explicit MeshPassProcessor(MeshPassDrawListContext& drawListContext) noexcept;
    virtual ~MeshPassProcessor() noexcept = default;
    void SetDrawListContext(MeshPassDrawListContext& drawListContext) noexcept;
    MeshPassDrawListContext& DrawListContext() noexcept { return *_drawListContext; }
    // Emits zero or more commands into the output context. Returning without emitting skips the batch.
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
