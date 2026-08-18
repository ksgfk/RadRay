#include <radray/runtime/render_framework/mesh_draw.h>

#include <algorithm>
#include <functional>

#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/scene.h>
#include <radray/runtime/shader_program.h>

namespace radray {
namespace {

bool IsTransparent(const Material* material) noexcept {
    return static_cast<int32_t>(material->GetRenderQueue()) >=
           static_cast<int32_t>(RenderQueue::GeometryLast);
}

}  // namespace

void MeshDrawList::Collect(
    const Scene* scene,
    const Eigen::Matrix4f& viewMatrix) {
    _items.clear();
    for (const unique_ptr<PrimitiveSceneProxy>& proxy : scene->Primitives()) {
        if (proxy == nullptr) {
            continue;
        }
        const Eigen::Matrix4f localToWorld = proxy->GetLocalToWorld();
        const Eigen::Vector4f viewOrigin =
            viewMatrix * localToWorld.col(3);
        for (uint32_t sectionIndex = 0;
             sectionIndex < proxy->GetSectionCount();
             ++sectionIndex) {
            const MeshDrawArgs args = proxy->GetDrawArgs(sectionIndex);
            const Nullable<Material*> material = proxy->GetMaterial(sectionIndex);
            if (args.Geometry == nullptr || args.IndexCount == 0 || !material.HasValue()) {
                continue;
            }
            _items.push_back(MeshDrawItem{
                .Geometry = args.Geometry,
                .DrawMaterial = material.Get(),
                .LocalToWorld = localToWorld,
                .FirstIndex = args.FirstIndex,
                .IndexCount = args.IndexCount,
                .VertexOffset = args.VertexOffset,
                .SectionIndex = sectionIndex,
                .ViewDepth = viewOrigin.z()});
        }
    }
}

void MeshDrawList::Sort() {
    std::stable_sort(
        _items.begin(),
        _items.end(),
        [](const MeshDrawItem& lhs, const MeshDrawItem& rhs) noexcept {
            const int32_t lhsQueue = static_cast<int32_t>(lhs.DrawMaterial->GetRenderQueue());
            const int32_t rhsQueue = static_cast<int32_t>(rhs.DrawMaterial->GetRenderQueue());
            if (lhsQueue != rhsQueue) {
                return lhsQueue < rhsQueue;
            }
            if (IsTransparent(lhs.DrawMaterial)) {
                return lhs.ViewDepth > rhs.ViewDepth;
            }
            ShaderProgram* lhsProgram = lhs.DrawMaterial->GetProgram();
            ShaderProgram* rhsProgram = rhs.DrawMaterial->GetProgram();
            if (lhsProgram != rhsProgram) {
                return std::less<const void*>{}(lhsProgram, rhsProgram);
            }
            if (lhs.DrawMaterial != rhs.DrawMaterial) {
                return std::less<const void*>{}(lhs.DrawMaterial, rhs.DrawMaterial);
            }
            return false;
        });
}

}  // namespace radray
