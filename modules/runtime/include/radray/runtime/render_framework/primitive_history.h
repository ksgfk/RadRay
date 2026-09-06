#pragma once

#include <radray/runtime/render_framework/render_scene_snapshot.h>

namespace radray {

struct PrimitiveMotionData {
    Eigen::Matrix4f PreviousLocalToWorld{Eigen::Matrix4f::Identity()};
    bool Valid{false};
};

/// One view's last successful snapshot. Prepare owns values; Commit only swaps already built maps.
class PrimitiveHistory {
public:
    bool Prepare(const RenderSceneSnapshot& snapshot, uint64_t serial);
    PrimitiveMotionData Lookup(const RenderPrimitiveData& primitive) const noexcept;
    bool CanCommit(uint64_t serial) const noexcept;
    bool Commit(uint64_t serial) noexcept;
    void Invalidate() noexcept;
    uint64_t CommittedSerial() const noexcept { return _committedSerial; }
    uint64_t PendingSerial() const noexcept { return _pendingSerial; }
    size_t Size() const noexcept { return _committed.size(); }

private:
    struct Transform {
        Eigen::Matrix4f LocalToWorld;
        uint64_t Revision;
    };
    unordered_map<uint64_t, Transform> _committed, _pending;
    uint64_t _committedSerial{0}, _pendingSerial{0};
    bool _valid{false};
};

}  // namespace radray
