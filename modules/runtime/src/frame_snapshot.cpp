#include <radray/runtime/frame_snapshot.h>
#include <radray/runtime/frame_snapshot_queue.h>

#include <algorithm>
#include <string_view>
#include <utility>

namespace radray::runtime {

FrameSnapshot& FrameSnapshotBuilder::Snapshot() noexcept {
    RADRAY_ASSERT(_snapshot != nullptr);
    return *_snapshot;
}

const FrameSnapshot& FrameSnapshotBuilder::Snapshot() const noexcept {
    RADRAY_ASSERT(_snapshot != nullptr);
    return *_snapshot;
}

void FrameSnapshotBuilder::AttachSnapshot(FrameSnapshot* snapshot) noexcept {
    _snapshot = snapshot != nullptr ? snapshot : &_ownedSnapshot;
}

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
    FrameSnapshot& snapshot = this->Snapshot();
    snapshot = FrameSnapshot{};
    snapshot.Header.FrameId = frameId;
    snapshot.Header.SimulationTick = simulationTick;
    snapshot.Header.CpuTimeSeconds = cpuTimeSeconds;
}

void FrameSnapshotBuilder::ResetFromSlot(
    FrameSnapshotSlot& slot,
    uint64_t frameId,
    uint64_t simulationTick,
    double cpuTimeSeconds) noexcept {
    this->AttachSnapshot(&slot.Snapshot);
    this->Reset(frameId, simulationTick, cpuTimeSeconds);
}

CameraRenderData& FrameSnapshotBuilder::AddCamera() {
    return this->Snapshot().Cameras.emplace_back();
}

VisibleMeshBatch& FrameSnapshotBuilder::AddMeshBatch() {
    return this->Snapshot().MeshBatches.emplace_back();
}

RenderViewRequest& FrameSnapshotBuilder::AddView() {
    return this->Snapshot().Views.emplace_back();
}

FrameSnapshot FrameSnapshotBuilder::Finalize(Nullable<string*> reason) noexcept {
    if (!this->Validate(reason.HasValue() ? reason.Get() : nullptr)) {
        return FrameSnapshot{};
    }

    FrameSnapshot& snapshot = this->Snapshot();
    snapshot.Header.CameraCount = static_cast<uint32_t>(snapshot.Cameras.size());
    snapshot.Header.MeshBatchCount = static_cast<uint32_t>(snapshot.MeshBatches.size());
    snapshot.Header.ViewCount = static_cast<uint32_t>(snapshot.Views.size());
    return std::move(snapshot);
}

bool FrameSnapshotBuilder::Validate(string* reason) const noexcept {
    auto fail = [&](std::string_view message) noexcept {
        if (reason != nullptr) {
            *reason = string{message};
        }
        return false;
    };

    const FrameSnapshot& snapshot = this->Snapshot();

    for (const auto& view : snapshot.Views) {
        if (view.OutputWidth == 0 || view.OutputHeight == 0) {
            return fail("render view output size must be non-zero");
        }
    }

    for (const auto& camera : snapshot.Cameras) {
        if (camera.OutputWidth == 0 || camera.OutputHeight == 0) {
            return fail("camera output size must be non-zero");
        }
        const bool hasView = std::find_if(
                                 snapshot.Views.begin(),
                                 snapshot.Views.end(),
                                 [&](const RenderViewRequest& view) noexcept {
                                     return view.ViewId == camera.ViewId && view.CameraId == camera.CameraId;
                                 }) != snapshot.Views.end();
        if (!hasView) {
            return fail("camera must reference an existing render view");
        }
    }

    for (const auto& meshBatch : snapshot.MeshBatches) {
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
