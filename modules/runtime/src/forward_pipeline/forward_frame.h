#pragma once

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/material.h>
#include <radray/runtime/render_framework/light_scene_proxy.h>

namespace radray {

class CameraComponent;
class Scene;

namespace forward_detail {

struct CameraFrameData {
    Eigen::Matrix4f View{Eigen::Matrix4f::Identity()};
    Eigen::Vector3f EyePosition{Eigen::Vector3f::Zero()};
    float FovY{0.0f};
    float NearZ{0.0f};
    float FarZ{0.0f};
};

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
    LightRenderParameters Parameters;
    float Radius{0.0f};
};

struct ForwardFrameInput {
    CameraFrameData Camera;
    vector<MaterialRenderData> Materials;
    vector<ForwardFrameDraw> Draws;
    vector<ForwardFrameLight> Lights;
};

bool FillViewParameters(ShaderParameterStorage& storage, const ForwardFrameInput& input,
                        float aspect, bool& lightOverflowWarned);

// Game thread only. No game objects or asset references escape into the input.
void CollectFrameInput(
    const Scene* scene,
    const CameraComponent* camera,
    ForwardFrameInput& input,
    vector<StreamingAssetRefAny>& retainedAssets);

}  // namespace forward_detail
}  // namespace radray
