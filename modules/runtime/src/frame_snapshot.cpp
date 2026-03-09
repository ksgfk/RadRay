#include <radray/runtime/frame_snapshot.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace radray::runtime {

std::span<const CameraRenderData> FrameSnapshot::GetCameras() const noexcept {
    return Cameras;
}

std::span<const VisibleMeshBatch> FrameSnapshot::GetMeshBatches() const noexcept {
    return MeshBatches;
}

std::span<const RenderViewRequest> FrameSnapshot::GetViews() const noexcept {
    return Views;
}

bool FrameSnapshot::IsEmpty() const noexcept {
    return Cameras.empty() && MeshBatches.empty() && Views.empty();
}

void FrameSnapshotBuilder::Reset(uint64_t frameId, uint64_t simulationTick, double cpuTimeSeconds) noexcept {
    _snapshot = FrameSnapshot{};
    _snapshot.Header.FrameId = frameId;
    _snapshot.Header.SimulationTick = simulationTick;
    _snapshot.Header.CpuTimeSeconds = cpuTimeSeconds;
}

CameraRenderData& FrameSnapshotBuilder::AddCamera() {
    return _snapshot.Cameras.emplace_back();
}

VisibleMeshBatch& FrameSnapshotBuilder::AddMeshBatch() {
    return _snapshot.MeshBatches.emplace_back();
}

RenderViewRequest& FrameSnapshotBuilder::AddView() {
    return _snapshot.Views.emplace_back();
}

FrameSnapshot FrameSnapshotBuilder::Finalize(Nullable<string*> reason) noexcept {
    if (!this->Validate(reason.HasValue() ? reason.Get() : nullptr)) {
        return FrameSnapshot{};
    }

    _snapshot.Header.CameraCount = static_cast<uint32_t>(_snapshot.Cameras.size());
    _snapshot.Header.MeshBatchCount = static_cast<uint32_t>(_snapshot.MeshBatches.size());
    _snapshot.Header.ViewCount = static_cast<uint32_t>(_snapshot.Views.size());
    return std::move(_snapshot);
}

bool FrameSnapshotBuilder::Validate(string* reason) const noexcept {
    auto fail = [&](std::string_view message) noexcept {
        if (reason != nullptr) {
            *reason = string{message};
        }
        return false;
    };

    for (const auto& view : _snapshot.Views) {
        if (view.OutputWidth == 0 || view.OutputHeight == 0) {
            return fail("render view output size must be non-zero");
        }
    }

    for (const auto& camera : _snapshot.Cameras) {
        if (camera.OutputWidth == 0 || camera.OutputHeight == 0) {
            return fail("camera output size must be non-zero");
        }
        const bool hasView = std::find_if(
                                 _snapshot.Views.begin(),
                                 _snapshot.Views.end(),
                                 [&](const RenderViewRequest& view) noexcept {
                                     return view.ViewId == camera.ViewId && view.CameraId == camera.CameraId;
                                 }) != _snapshot.Views.end();
        if (!hasView) {
            return fail("camera must reference an existing render view");
        }
    }

    for (const auto& meshBatch : _snapshot.MeshBatches) {
        if (!meshBatch.Mesh.IsValid()) {
            return fail("visible mesh batch must reference a valid mesh handle");
        }
        if (!meshBatch.Material.IsValid()) {
            return fail("visible mesh batch must reference a valid material handle");
        }
    }
    return true;
}

}  // namespace radray::runtime
