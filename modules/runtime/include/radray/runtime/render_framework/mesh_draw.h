#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/types.h>

namespace radray {

class Material;
class Scene;

struct MeshDrawItem {
    const GpuMesh::DrawData* Geometry;
    Material* DrawMaterial;
    Eigen::Matrix4f LocalToWorld;
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint32_t SectionIndex{0};
    float ViewDepth{0.0f};
};

class MeshDrawList {
public:
    void Collect(const Scene* scene, const Eigen::Matrix4f& viewMatrix);
    void Sort();
    void Clear() noexcept { _items.clear(); }

    std::span<MeshDrawItem> Items() noexcept { return _items; }
    std::span<const MeshDrawItem> Items() const noexcept { return _items; }
    size_t Size() const noexcept { return _items.size(); }

private:
    vector<MeshDrawItem> _items;
};

}  // namespace radray
