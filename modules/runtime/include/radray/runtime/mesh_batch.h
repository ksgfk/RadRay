#pragma once

#include <radray/basic_math.h>
#include <radray/inline_vector.h>
#include <radray/nullable.h>
#include <radray/sparse_set.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/vertex_input.h>

namespace radray {

// Submesh range of a geometry asset; Scene does not store it.
struct MeshSection {
    uint32_t MaterialSlot{0};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
};

struct MeshBatchElement {
    // Borrowed; required when IndexCount is nonzero. Offset is in bytes; stride is 2 or 4.
    Nullable<render::Buffer*> IndexBuffer{nullptr};
    uint32_t IndexBufferOffset{0};
    uint32_t IndexStride{4};
    // FirstIndex is relative to IndexBufferOffset and measured in indices.
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};

struct MeshBatch {
    // Resolves only in the owning Scene's Materials container.
    SparseSetHandle Material{};
    // Borrowed; must remain at a stable address for all referencing batches.
    Nullable<const radray::VertexInput*> VertexInput{nullptr};
    vector<MeshBatchElement> Elements;
    render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
    // Managed by Scene; callers must not copy scene links or cache entries into another batch.
    SparseSetHandle OwningPrimitive{};
    bool Dirty{false};
    InlineVector<CachedMeshDrawCommandInfo, 4> CachedCommands;
};

struct PrimitiveSceneInfo {
    AffineTransform LocalToWorld{};
    Aabb LocalBounds{};
    // Handles resolve only in the owning Scene's Meshes container.
    vector<SparseSetHandle> Meshes;
};

}  // namespace radray
