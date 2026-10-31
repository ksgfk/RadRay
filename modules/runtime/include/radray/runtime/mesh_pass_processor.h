#pragma once

#include <radray/runtime/mesh_pass_draw_list_context.h>
#include <radray/runtime/mesh_batch.h>
#include <radray/runtime/primitive_scene_info.h>

namespace radray {

// The caller resolves handles and supplies matching records. References are borrowed only during AddMeshBatch.
// The processor must not mutate the input records or their owning Scene.
struct MeshBatchContext {
    const PrimitiveSceneInfo& Primitive;
    const MeshBatch& Batch;
    SparseSetHandle PrimitiveHandle;
    SparseSetHandle BatchHandle;
};

class MeshPassProcessor {
public:
    // The output context must outlive this processor.
    explicit MeshPassProcessor(MeshPassDrawListContext& drawListContext) noexcept;
    virtual ~MeshPassProcessor() noexcept = default;
    // Emits zero or more commands into the output context. Returning without emitting skips the batch.
    virtual void AddMeshBatch(const MeshBatchContext& context) = 0;

protected:
    MeshPassDrawListContext& _drawListContext;
};

}  // namespace radray
