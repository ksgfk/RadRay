#pragma once

#include <algorithm>
#include <optional>

#include <radray/logger.h>
#include <radray/nullable.h>
#include <radray/types.h>

#include <radray/runtime/frame_snapshot.h>

namespace radray::runtime {

class FrameSnapshotBuilder {
public:
    void Reset(uint64_t frameId, uint64_t simulationTick, double cpuTimeSeconds = 0.0) noexcept;

    CameraRenderData& AddCamera();

    VisibleMeshBatch& AddMeshBatch();

    RenderViewRequest& AddView();

    FrameSnapshot Finalize(Nullable<string*> reason = nullptr) noexcept;

private:
    bool Validate(string* reason) const noexcept;

    FrameSnapshot _snapshot{};
};

inline void FrameSnapshotBuilder::Reset(uint64_t frameId, uint64_t simulationTick, double cpuTimeSeconds) noexcept {
    _snapshot = FrameSnapshot{};
    _snapshot.Header.FrameId = frameId;
    _snapshot.Header.SimulationTick = simulationTick;
    _snapshot.Header.CpuTimeSeconds = cpuTimeSeconds;
}

inline CameraRenderData& FrameSnapshotBuilder::AddCamera() {
    return _snapshot.Cameras.emplace_back();
}

inline VisibleMeshBatch& FrameSnapshotBuilder::AddMeshBatch() {
    return _snapshot.MeshBatches.emplace_back();
}

inline RenderViewRequest& FrameSnapshotBuilder::AddView() {
    return _snapshot.Views.emplace_back();
}

inline FrameSnapshot FrameSnapshotBuilder::Finalize(Nullable<string*> reason) noexcept {
    if (!this->Validate(reason.HasValue() ? reason.Get() : nullptr)) {
        return FrameSnapshot{};
    }

    _snapshot.Header.CameraCount = static_cast<uint32_t>(_snapshot.Cameras.size());
    _snapshot.Header.MeshBatchCount = static_cast<uint32_t>(_snapshot.MeshBatches.size());
    _snapshot.Header.ViewCount = static_cast<uint32_t>(_snapshot.Views.size());
    return std::move(_snapshot);
}

inline bool FrameSnapshotBuilder::Validate(string* reason) const noexcept {
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
