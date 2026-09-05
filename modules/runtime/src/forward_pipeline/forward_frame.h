#pragma once

#include <radray/runtime/render_framework/renderer_list.h>

namespace radray {
class CameraComponent;
namespace forward_detail {

struct ForwardViewDrawWork {
    ResolvedRenderView View;
    CullingResults Culling;
    RendererList DepthOnly, Opaque, Transparent;
    void ResetForReuse() noexcept {
        DepthOnly.ResetForReuse();
        Opaque.ResetForReuse();
        Transparent.ResetForReuse();
        Culling.ResetForReuse();
    }
};
struct ForwardFamilyDrawWork {
    vector<ForwardViewDrawWork> Views;
};

bool FillViewParameters(ShaderParameterStorage& storage, const CullingResults& culling,
                        const ResolvedRenderView& view, bool& lightOverflowWarned);
RenderViewDesc CollectRenderView(const CameraComponent& camera);
/// A positive multiple of the inverse transpose; normalize transformed normals in the shader.
Eigen::Matrix4f MakeNormalToWorld(const Eigen::Matrix4f& localToWorld);

}  // namespace forward_detail
}  // namespace radray
