#pragma once

#include <radray/sparse_set.h>
#include <radray/runtime/primitive_scene_info.h>
#include <radray/runtime/mesh_batch.h>
#include <radray/runtime/material_render_proxy.h>

namespace radray {

// See docs/architecture/runtime.md for the scene storage contract.
struct Scene {
    SparseSet<PrimitiveSceneInfo> Primitives;
    SparseSet<MeshBatch> Meshes;
    SparseSet<MaterialRenderProxy> Materials;
};

}  // namespace radray
