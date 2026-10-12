#pragma once

#include <radray/basic_math.h>
#include <radray/types.h>

namespace radray {
/// Whole primitive/group metadata, independent of vertex format or triangle topology.
struct RasterSortGroup {
    uint32_t FirstIndex, IndexCount;
    Eigen::Vector3d Center;
    uint32_t Domain{0};
};
bool RasterBoundsVisible(const Eigen::Vector3d& min, const Eigen::Vector3d& max,
                         const Eigen::Vector3d& camera, const Eigen::Matrix4f& clip) noexcept;
/// Groups are indivisible, nonoverlapping, and contiguous in the input range.
/// Domains keep their original relative order; sorting occurs within each contiguous domain.
bool SortRasterIndices(std::span<const uint32_t> source, std::span<const RasterSortGroup> groups,
                       const Eigen::Matrix4d& transform, const Eigen::Vector3d& camera,
                       bool preserveOrder, std::span<uint32_t> destination);
/// Split double positions once; subtract high and low pairs per view before adding local float geometry.
struct RasterSplitPosition { array<float, 4> High{}, Low{}; };
std::optional<RasterSplitPosition> SplitRasterPosition(const Eigen::Vector3d& position) noexcept;
}  // namespace radray
