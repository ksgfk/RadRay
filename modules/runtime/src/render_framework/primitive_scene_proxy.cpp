#include <radray/runtime/render_framework/primitive_scene_proxy.h>

#include <atomic>

namespace radray {

namespace {
std::atomic<uint64_t> gNextPrimitiveProxyGeneration{1};
}

PrimitiveSceneProxy::PrimitiveSceneProxy() noexcept
    : _generation(gNextPrimitiveProxyGeneration.fetch_add(1, std::memory_order_relaxed)) {
    if (_generation == 0 || _generation == UINT64_MAX) RADRAY_ABORT("Primitive generation exhausted");
}

PrimitiveSceneProxy::~PrimitiveSceneProxy() noexcept = default;

void PrimitiveSceneProxy::SetLocalToWorld(const Eigen::Matrix4f& value) noexcept {
    if ((_localToWorld.array() == value.array()).all()) return;
    if (_transformRevision == UINT64_MAX) RADRAY_ABORT("Primitive transform revision exhausted");
    _localToWorld = value;
    ++_transformRevision;
}

void PrimitiveSceneProxy::ResetMotion() noexcept {
    if (_motionRevision == UINT64_MAX) RADRAY_ABORT("Primitive motion revision exhausted");
    ++_motionRevision;
}

void PrimitiveSceneProxy::CollectAssetReferences(vector<StreamingAssetRefAny>&) const {}

}  // namespace radray
