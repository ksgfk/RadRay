#pragma once

#include <radray/basic_math.h>
#include <radray/inline_vector.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/render/rhi.h>
#include <radray/runtime/vertex_input.h>
#include <radray/runtime/scene_handle.h>

namespace radray {

// 几何资产的子网格区间；Scene 不保存它。
struct MeshSection {
    uint32_t MaterialSlot{0};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
};

struct MeshBatchElement {
    // 借用索引缓冲；IndexCount 非零时必须提供；偏移单位为字节，步长只能为 2 或 4。
    Nullable<render::Buffer*> IndexBuffer{nullptr};
    uint32_t IndexBufferOffset{0};
    uint32_t IndexStride{4};
    // FirstIndex 相对 IndexBufferOffset，以索引个数为单位。
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint32_t FirstInstance{0};
    uint32_t InstanceCount{1};
};

// 共享绘制条件的一组网格
struct MeshBatch {
    // 句柄仅在所属 Scene 的材质容器中解析。
    MaterialHandle Material{};
    // 借用顶点输入；被任何 Batch 引用期间都必须保持地址稳定。
    Nullable<const radray::VertexInput*> VertexInput{nullptr};
    // 最多 32 项；掩码的第 i 位始终对应源索引，不因前面的空项改变。
    InlineVector<MeshBatchElement, 8> Elements;
    render::PrimitiveTopology Topology{render::PrimitiveTopology::TriangleList};
};

struct PrimitiveSceneInfo {
    AffineTransform LocalToWorld{};
    Aabb LocalBounds{};
    // 句柄仅在所属 Scene 的网格容器中解析。
    vector<MeshBatchHandle> Meshes;
};

}  // namespace radray
