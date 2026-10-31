#pragma once

#include <radray/basic_math.h>
#include <radray/sparse_set.h>
#include <radray/types.h>

namespace radray {

struct PrimitiveSceneInfo {
    AffineTransform LocalToWorld{};
    Aabb LocalBounds{};
    // Handles resolve only in the owning Scene's Meshes container.
    vector<SparseSetHandle> Meshes;
};

}  // namespace radray
