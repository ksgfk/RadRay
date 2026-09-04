#pragma once

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>
#include <radray/runtime/render_framework/render_view.h>

namespace radray {

class CameraComponent;
class Scene;

namespace forward_detail {

struct ForwardFrameDraw {
    const GpuMesh::DrawData* Geometry;
    uint32_t MaterialIndex{0};
    Eigen::Matrix4f LocalToWorld{Eigen::Matrix4f::Identity()};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint32_t SectionIndex{0};
};

struct ForwardFrameLight {
    LightType Type{LightType::Directional};
    LightRenderParameters Parameters{};
    float Radius{0.0f};
};

struct ForwardFrameInput {
    vector<MaterialRenderData> Materials;
    vector<ForwardFrameDraw> Draws;
    vector<ForwardFrameLight> Lights;
};

bool FillViewParameters(ShaderParameterStorage& storage, const ForwardFrameInput& input,
                        const ResolvedRenderView& view, bool& lightOverflowWarned);
RenderViewDesc CollectRenderView(const CameraComponent& camera);

// Game thread only. No game objects or asset references escape into the input.
void CollectFrameInput(
    const Scene* scene,
    const CameraComponent* camera,
    ForwardFrameInput& input,
    vector<StreamingAssetRefAny>& retainedAssets);

}  // namespace forward_detail
}  // namespace radray
