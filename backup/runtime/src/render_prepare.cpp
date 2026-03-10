#include <algorithm>

#include <radray/runtime/render_prepare.h>

namespace radray::runtime {
namespace {

const CameraRenderData* FindCamera(const FrameSnapshot& snapshot, uint32_t cameraId) noexcept {
    const auto it = std::find_if(
        snapshot.Cameras.begin(),
        snapshot.Cameras.end(),
        [&](const CameraRenderData& camera) noexcept { return camera.CameraId == cameraId; });
    return it == snapshot.Cameras.end() ? nullptr : &(*it);
}

bool IsMeshVisibleInView(const VisibleMeshBatch& meshBatch, uint32_t viewId) noexcept {
    if (meshBatch.ViewMask == 0) {
        return true;
    }
    if (viewId >= 32) {
        return false;
    }
    return (meshBatch.ViewMask & (1u << viewId)) != 0u;
}

}  // namespace

bool PrepareScene(
    const FrameSnapshot& snapshot,
    const RenderAssetRegistry& assets,
    PreparedScene& prepared,
    Nullable<string*> reason) noexcept {
    prepared = PreparedScene{};
    prepared.Views.reserve(snapshot.Views.size());

    auto fail = [&](std::string_view message) noexcept {
        if (reason.HasValue()) {
            *reason.Get() = string{message};
        }
        prepared = PreparedScene{};
        return false;
    };

    for (const auto& viewRequest : snapshot.Views) {
        const CameraRenderData* camera = FindCamera(snapshot, viewRequest.CameraId);
        if (camera == nullptr || camera->ViewId != viewRequest.ViewId) {
            return fail("render view does not map to a valid camera");
        }
        prepared.Views.push_back(PreparedView{
            .ViewId = viewRequest.ViewId,
            .CameraId = viewRequest.CameraId,
            .OutputWidth = viewRequest.OutputWidth,
            .OutputHeight = viewRequest.OutputHeight,
            .Camera = camera,
        });
    }

    for (auto& view : prepared.Views) {
        for (const auto& meshBatch : snapshot.MeshBatches) {
            if (!IsMeshVisibleInView(meshBatch, view.ViewId)) {
                continue;
            }
            auto mesh = assets.ResolveMesh(meshBatch.Mesh);
            if (!mesh.HasValue()) {
                return fail("prepared scene references an unknown mesh handle");
            }
            if (meshBatch.SubmeshIndex >= mesh.Get()->Submeshes.size()) {
                return fail("prepared scene references an invalid submesh index");
            }
            if (!assets.ResolveMaterial(meshBatch.Material).HasValue()) {
                return fail("prepared scene references an unknown material handle");
            }
            view.DrawItems.push_back(PreparedDrawItem{
                .Mesh = meshBatch.Mesh,
                .Material = meshBatch.Material,
                .SubmeshIndex = meshBatch.SubmeshIndex,
                .ViewId = view.ViewId,
                .LocalToWorld = meshBatch.LocalToWorld,
                .Tint = Eigen::Vector4f::Ones(),
                .SortKeyHigh = meshBatch.SortKeyHigh,
                .SortKeyLow = meshBatch.SortKeyLow,
            });
        }

        std::sort(
            view.DrawItems.begin(),
            view.DrawItems.end(),
            [](const PreparedDrawItem& lhs, const PreparedDrawItem& rhs) noexcept {
                if (lhs.SortKeyHigh != rhs.SortKeyHigh) {
                    return lhs.SortKeyHigh < rhs.SortKeyHigh;
                }
                if (lhs.SortKeyLow != rhs.SortKeyLow) {
                    return lhs.SortKeyLow < rhs.SortKeyLow;
                }
                if (lhs.Material.Value != rhs.Material.Value) {
                    return lhs.Material.Value < rhs.Material.Value;
                }
                if (lhs.Mesh.Value != rhs.Mesh.Value) {
                    return lhs.Mesh.Value < rhs.Mesh.Value;
                }
                return lhs.SubmeshIndex < rhs.SubmeshIndex;
            });
    }

    return true;
}

}  // namespace radray::runtime
