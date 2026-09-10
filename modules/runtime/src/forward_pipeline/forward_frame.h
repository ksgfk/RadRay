#pragma once

#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>
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

/// Lights depend on culling, so the view cbuffer is filled here rather than frozen at PrepareFrame.
/// Overwrites every field of `out`, including the unused light slots.
void FillViewParameters(Forward_ViewData& out, const CullingResults& culling,
                        const ResolvedRenderView& view, bool& lightOverflowWarned, bool localLightsFromPass = false);
/// Game thread. Freezes one row per snapshot primitive, indexed by primitive. Motion defaults to no
/// motion; only views with a temporal context patch it while gathering visible rows.
void FreezeObjectData(const RenderSceneSnapshot& scene, PackedCBufferTable& out);
RenderViewDesc CollectRenderView(const CameraComponent& camera);
/// A positive multiple of the inverse transpose; normalize transformed normals in the shader.
Eigen::Matrix4f MakeNormalToWorld(const Eigen::Matrix4f& localToWorld);

}  // namespace forward_detail
}  // namespace radray
