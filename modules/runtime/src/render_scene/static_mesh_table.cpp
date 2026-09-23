#include <radray/runtime/render_scene/static_mesh_table.h>

#include <algorithm>
#include <cmath>
#include <radray/logger.h>

namespace radray {

StaticMeshSceneView StaticMeshSceneColumns::Get(size_t row) const noexcept {
    return {Bindings[row], Transforms[TransformRows[row]], Bounds[row].Min, Bounds[row].Max, Bounds[row].ReverseCulling};
}

void StaticMeshTable::Add(const StaticMeshStateUpdate& update, uint32_t transformRow) {
    const auto row = _ids.size();
    _ids.push_back(update.Id);
    _bindings.emplace_back();
    _transformRows.push_back(transformRow);
    _bounds.emplace_back();
    Replace(row, update, transformRow);
}

void StaticMeshTable::Remove(size_t row) noexcept {
    const auto remove = [row](auto& column) {
        if (row != column.size() - 1) column[row] = std::move(column.back());
        column.pop_back();
    };
    remove(_ids);
    remove(_bindings);
    remove(_transformRows);
    remove(_bounds);
}

void StaticMeshTable::Replace(size_t row, const StaticMeshStateUpdate& update, uint32_t transformRow) {
    const auto& mesh = update.Mesh;
    if (const auto data = mesh.RenderData; data &&
                                           (!data->LocalBoundsMin.allFinite() || !data->LocalBoundsMax.allFinite() ||
                                            (data->LocalBoundsMin.array() > data->LocalBoundsMax.array()).any())) {
        RADRAY_ABORT("Invalid static mesh local bounds");
    }
    _bindings[row] = mesh;
    _transformRows[row] = transformRow;
}

void StaticMeshTable::UpdateBounds(size_t row, const Eigen::Matrix4f& transform) noexcept {
    const float* m = transform.data();
#ifdef RADRAY_IS_DEBUG
    for (size_t i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) RADRAY_ABORT("Scene transform must be finite and affine");
    }
#endif
    auto& bounds = _bounds[row];
    float* worldMin = bounds.Min.data();
    float* worldMax = bounds.Max.data();
    if (const auto data = _bindings[row].RenderData) {
        const float* localMin = data->LocalBoundsMin.data();
        const float* localMax = data->LocalBoundsMax.data();
        for (size_t axis = 0; axis < 3; ++axis) {
            float lower = m[12 + axis];
            float upper = lower;
            for (size_t column = 0; column < 3; ++column) {
                const float a = m[column * 4 + axis] * localMin[column];
                const float b = m[column * 4 + axis] * localMax[column];
                lower += std::min(a, b);
                upper += std::max(a, b);
            }
            worldMin[axis] = lower;
            worldMax[axis] = upper;
        }
    } else {
        for (size_t axis = 0; axis < 3; ++axis) worldMin[axis] = worldMax[axis] = m[12 + axis];
    }
    const double determinant = static_cast<double>(m[0]) * (static_cast<double>(m[5]) * m[10] - static_cast<double>(m[9]) * m[6]) -
                               static_cast<double>(m[4]) * (static_cast<double>(m[1]) * m[10] - static_cast<double>(m[9]) * m[2]) +
                               static_cast<double>(m[8]) * (static_cast<double>(m[1]) * m[6] - static_cast<double>(m[5]) * m[2]);
    bounds.ReverseCulling = determinant < 0;
}

}  // namespace radray
