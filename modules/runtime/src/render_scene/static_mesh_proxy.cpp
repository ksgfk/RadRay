#include "static_mesh_proxy.h"

#include <algorithm>
#include <cmath>

#include <radray/logger.h>

namespace radray {

StaticMeshProxy::StaticMeshProxy(const StaticMeshStateUpdate& update) {
    Replace(update);
}

void StaticMeshProxy::Replace(const StaticMeshStateUpdate& update) {
    const auto& mesh = update.Mesh;
    if (!mesh.LocalBoundsMin.allFinite() || !mesh.LocalBoundsMax.allFinite() ||
        (mesh.LocalBoundsMin.array() > mesh.LocalBoundsMax.array()).any()) {
        RADRAY_ABORT("Invalid static mesh local bounds");
    }
    _mesh = mesh;
    SetTransform(update.LocalToWorld);
}

void StaticMeshProxy::SetTransform(const Eigen::Matrix4f& localToWorld) noexcept {
    const float* m = localToWorld.data();
#ifdef RADRAY_IS_DEBUG
    // writer→flight 的契约校验：每 transform 一次的 isfinite/仿射检查只留在 Debug；
    // Release 直接写矩阵，AABB 与行列式照算（渲染需要）。契约见 docs/architecture/render-framework.md。
    if (m[3] != 0 || m[7] != 0 || m[11] != 0 || m[15] != 1) {
        RADRAY_ABORT("Static mesh transform must be finite and affine");
    }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) RADRAY_ABORT("Static mesh transform must be finite and affine");
    }
#endif
    float* storedTransform = _localToWorld.data();
    for (int i = 0; i < 16; ++i) {
        storedTransform[i] = m[i];
    }
    const float* localMin = _mesh.LocalBoundsMin.data();
    const float* localMax = _mesh.LocalBoundsMax.data();
    float* worldMin = _worldBoundsMin.data();
    float* worldMax = _worldBoundsMax.data();
    for (int row = 0; row < 3; ++row) {
        float lower = m[12 + row];
        float upper = lower;
        for (int column = 0; column < 3; ++column) {
            const float a = m[column * 4 + row] * localMin[column];
            const float b = m[column * 4 + row] * localMax[column];
            lower += std::min(a, b);
            upper += std::max(a, b);
        }
        worldMin[row] = lower;
        worldMax[row] = upper;
    }
    const double determinant = static_cast<double>(m[0]) * (static_cast<double>(m[5]) * m[10] - static_cast<double>(m[9]) * m[6]) -
                               static_cast<double>(m[4]) * (static_cast<double>(m[1]) * m[10] - static_cast<double>(m[9]) * m[2]) +
                               static_cast<double>(m[8]) * (static_cast<double>(m[1]) * m[6] - static_cast<double>(m[5]) * m[2]);
    _reverseCulling = determinant < 0;
}

StaticMeshSceneView StaticMeshProxy::GetView() const noexcept {
    return {_mesh, _localToWorld, _worldBoundsMin, _worldBoundsMax, _reverseCulling};
}

}  // namespace radray
