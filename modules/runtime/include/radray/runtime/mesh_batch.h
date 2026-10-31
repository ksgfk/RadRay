#pragma once

#include <radray/nullable.h>
#include <radray/sparse_set.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/mesh_batch_element.h>
#include <radray/runtime/vertex_input.h>

namespace radray {

struct MeshBatch {
    // Resolves only in the owning Scene's Materials container.
    SparseSetHandle Material{};
    // Borrowed; must remain at a stable address for all referencing batches.
    Nullable<const radray::VertexInput*> VertexInput{nullptr};
    vector<MeshBatchElement> Elements;
    render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
};

}  // namespace radray
