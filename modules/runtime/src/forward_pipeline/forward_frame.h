#pragma once

#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>
#include <radray/runtime/render_framework/renderer_list.h>

namespace radray {
class CameraComponent;
namespace forward_detail {

inline constexpr byte kForwardObjectWireIdentity{};
inline constexpr uint64_t kForwardWorkDepth = 1, kForwardWorkOpaque = 2, kForwardWorkTransparent = 4, kForwardWorkLights = 8;

struct ForwardViewDrawWork {
    ResolvedRenderView View;
    CullingResults Culling;
    RendererList DepthOnly, Opaque, Transparent;
    RgWorkHandle Work{};
    uint64_t RequestedRoles{0};
    uint32_t CullCalls{0};
    void ResetForReuse() noexcept {
        DepthOnly.ResetForReuse();
        Opaque.ResetForReuse();
        Transparent.ResetForReuse();
        Culling.ResetForReuse();
        Work = {};
        RequestedRoles = 0;
        CullCalls = 0;
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

/// Writable-flight storage. Published rows are immutable until the next safe Update/Clear.
/// Temporal previous/motion values belong to each view and are patched when gathering its rows.
class ForwardObjectDataCache {
public:
    uint64_t Update(const RenderSceneSnapshot& scene);
    void Clear() noexcept {
        _rows.Clear();
        _versions.clear();
        _publicationId = 0;
        _publicationRevision = 0;
    }
    const PackedCBufferTable& Rows() const noexcept { return _rows; }

private:
    struct Version {
        uint64_t Generation{0}, TransformRevision{0};
    };
    PackedCBufferTable _rows;
    vector<Version> _versions;
    uint64_t _publicationId{0};
    uint64_t _publicationRevision{0};
};
RenderViewDesc CollectRenderView(const CameraComponent& camera);
/// A positive multiple of the inverse transpose; normalize transformed normals in the shader.
Eigen::Matrix4f MakeNormalToWorld(const Eigen::Matrix4f& localToWorld);

}  // namespace forward_detail
}  // namespace radray
