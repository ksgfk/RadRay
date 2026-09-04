#include "forward_frame.h"

#include <algorithm>

#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/render_framework/mesh_draw.h>
#include <radray/runtime/render_framework/scene.h>

namespace radray::forward_detail {

void CollectFrameInput(
    const Scene* scene,
    const CameraComponent* camera,
    ForwardFrameInput& input,
    vector<StreamingAssetRefAny>& retainedAssets) {
    input.Camera = CameraFrameData{
        .View = camera->ComputeViewMatrix(),
        .EyePosition = camera->GetEyePosition(),
        .FovY = camera->GetFovY(),
        .NearZ = camera->GetNearZ(),
        .FarZ = camera->GetFarZ()};
    input.Materials.clear();
    input.Draws.clear();
    input.Lights.clear();

    MeshDrawList collected;
    collected.Collect(scene, input.Camera.View);
    collected.Sort();
    for (const auto& proxy : scene->Primitives()) {
        if (proxy != nullptr) {
            proxy->CollectAssetReferences(retainedAssets);
        }
    }
    vector<Material*> materials;
    vector<bool> validMaterials;
    for (const MeshDrawItem& item : collected.Items()) {
        const auto found = std::find(materials.begin(), materials.end(), item.DrawMaterial);
        const uint32_t index = static_cast<uint32_t>(found - materials.begin());
        if (found == materials.end()) {
            materials.push_back(item.DrawMaterial);
            input.Materials.emplace_back();
            validMaterials.push_back(item.DrawMaterial->BuildRenderData(input.Materials.back(), retainedAssets));
        }
        if (!validMaterials[index]) {
            continue;
        }
        input.Draws.push_back(ForwardFrameDraw{
            .Geometry = item.Geometry,
            .MaterialIndex = index,
            .LocalToWorld = item.LocalToWorld,
            .FirstIndex = item.FirstIndex,
            .IndexCount = item.IndexCount,
            .VertexOffset = item.VertexOffset,
            .SectionIndex = item.SectionIndex});
    }
    for (const auto& light : scene->Lights()) {
        if (light == nullptr || !light->AffectsWorld()) {
            continue;
        }
        ForwardFrameLight value{.Type = light->GetLightType(), .Radius = light->GetRadius()};
        light->GetLightRenderParameters(value.Parameters);
        input.Lights.push_back(value);
    }
}

}  // namespace radray::forward_detail
