#include <radray/runtime/render_framework/primitive_history.h>

namespace radray {

bool PrimitiveHistory::Prepare(const RenderSceneSnapshot& snapshot, uint64_t serial) {
    if (!serial || serial <= _committedSerial || serial <= _pendingSerial) return false;
    _pending.clear();
    _pendingSerial = 0;
    _pending.reserve(snapshot.Primitives.size());
    for (const auto& primitive : snapshot.Primitives) {
        if (!primitive.Generation || !primitive.LocalToWorld.allFinite() ||
            !_pending.try_emplace(primitive.Generation, Transform{primitive.LocalToWorld, primitive.MotionRevision}).second) {
            _pending.clear();
            return false;
        }
    }
    _pendingSerial = serial;
    return true;
}

PrimitiveMotionData PrimitiveHistory::Lookup(const RenderPrimitiveData& primitive) const noexcept {
    if (_valid) {
        const auto found = _committed.find(primitive.Generation);
        if (found != _committed.end() && found->second.Revision == primitive.MotionRevision)
            return {found->second.LocalToWorld, true};
    }
    return {primitive.LocalToWorld, false};
}

bool PrimitiveHistory::CanCommit(uint64_t serial) const noexcept { return serial && _pendingSerial == serial && serial > _committedSerial; }
bool PrimitiveHistory::Commit(uint64_t serial) noexcept {
    if (!CanCommit(serial)) return false;
    _committed.swap(_pending);
    _committedSerial = serial;
    _pendingSerial = 0;
    _valid = true;
    return true;
}
void PrimitiveHistory::Invalidate() noexcept {
    _valid = false;
    _pendingSerial = 0;
}

}  // namespace radray
