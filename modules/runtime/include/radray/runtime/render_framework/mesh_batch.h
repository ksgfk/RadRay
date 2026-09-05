#pragma once

#include <radray/runtime/gpu_resource.h>

namespace radray {

using RenderPrimitiveIndex = uint32_t;
using RenderMaterialIndex = uint32_t;
using MeshBatchIndex = uint32_t;

struct MeshBatch {
    RenderPrimitiveIndex Primitive{0};
    RenderMaterialIndex Material{0};
    Nullable<const GpuMesh::DrawData*> Geometry{nullptr};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint32_t SectionIndex{0};
};

}  // namespace radray
