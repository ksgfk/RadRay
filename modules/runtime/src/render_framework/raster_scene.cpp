#include <radray/runtime/render_framework/raster_scene.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace radray {
bool RasterBoundsVisible(const Eigen::Vector3d& min, const Eigen::Vector3d& max,
                         const Eigen::Vector3d& camera, const Eigen::Matrix4f& clip) noexcept {
    if (!min.allFinite() || !max.allFinite() || !camera.allFinite() || !clip.allFinite() || (min.array() > max.array()).any()) return false;
    uint32_t common = 63;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        Eigen::Vector4d point;
        for (uint32_t axis = 0; axis < 3; ++axis) point[axis] = ((corner & (1u << axis)) ? max[axis] : min[axis]) - camera[axis];
        point[3] = 1; const Eigen::Vector4d projected = clip.cast<double>() * point;
        if (!projected.allFinite()) return true;
        uint32_t outside = 0;
        if (projected.x() < -projected.w()) outside |= 1;
        if (projected.x() > projected.w()) outside |= 2;
        if (projected.y() < -projected.w()) outside |= 4;
        if (projected.y() > projected.w()) outside |= 8;
        if (projected.z() < 0) outside |= 16;
        if (projected.z() > projected.w()) outside |= 32;
        common &= outside;
    }
    return common == 0;
}
bool SortRasterIndices(std::span<const uint32_t> source, std::span<const RasterSortGroup> groups,
                       const Eigen::Matrix4d& transform, const Eigen::Vector3d& camera,
                       bool preserveOrder, std::span<uint32_t> destination) {
    if (!transform.allFinite() || !camera.allFinite()) return false;
    uint64_t count = 0;
    for (size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        if (!group.IndexCount || !group.Center.allFinite() || group.FirstIndex > source.size() || group.IndexCount > source.size() - group.FirstIndex ||
            (i && uint64_t{groups[i - 1].FirstIndex} + groups[i - 1].IndexCount != group.FirstIndex)) return false;
        count += group.IndexCount;
    }
    if (count != destination.size()) return false;
    struct Item { size_t Index; double Distance; };
    vector<Item> items; items.reserve(groups.size());
    for (size_t i = 0; i < groups.size(); ++i) {
        Eigen::Vector4d local; local << groups[i].Center, 1;
        const Eigen::Vector3d position = (transform * local).head<3>() - camera;
        items.push_back({i, position.squaredNorm()});
    }
    if (!preserveOrder) for (size_t begin = 0; begin < items.size();) {
        size_t end = begin + 1;
        while (end < items.size() && groups[end].Domain == groups[begin].Domain) ++end;
        std::stable_sort(items.begin() + begin, items.begin() + end, [](const auto& a, const auto& b) { return a.Distance > b.Distance; });
        begin = end;
    }
    size_t offset = 0;
    for (const auto& item : items) {
        const auto& group = groups[item.Index];
        std::memcpy(destination.data() + offset, source.data() + group.FirstIndex, size_t{group.IndexCount} * 4); offset += group.IndexCount;
    }
    return true;
}
std::optional<RasterSplitPosition> SplitRasterPosition(const Eigen::Vector3d& position) noexcept {
    if (!position.allFinite() || position.cwiseAbs().maxCoeff() > std::numeric_limits<float>::max() / 4) return std::nullopt;
    RasterSplitPosition result;
    for (size_t i = 0; i < 3; ++i) { result.High[i] = float(position[i]); result.Low[i] = float(position[i] - double(result.High[i])); }
    return result;
}
}  // namespace radray
