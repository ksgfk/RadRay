#pragma once

#include <radray/basic_math.h>
#include <radray/render/rhi.h>

namespace radray {
enum class RasterDepthConvention : uint8_t { ZeroToOne, MinusOneToOne, ReverseZeroToOne };
/// Already evaluated application matrices. Viewport/scissor use top-left pixel coordinates.
struct RasterView {
    Eigen::Matrix4f View{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f Projection{Eigen::Matrix4f::Identity()};
    radray::Viewport Viewport;
    Rect Scissor;
    RasterDepthConvention Depth{RasterDepthConvention::ZeroToOne};
};
struct ResolvedRasterView {
    Eigen::Matrix4f View, Projection, ViewProjection;
    radray::Viewport Viewport;
    Rect Scissor;
    bool ReverseDepth{false};
};
std::optional<ResolvedRasterView> ResolveRasterView(const RasterView& view, uint32_t width, uint32_t height,
                                                  render::RenderBackend backend) noexcept;
}  // namespace radray
