#include <radray/runtime/render_framework/raster_view.h>
#include <algorithm>
#include <cmath>

namespace radray {
std::optional<ResolvedRasterView> ResolveRasterView(const RasterView& view, uint32_t width, uint32_t height,
                                                  render::RenderBackend backend) noexcept {
    const auto& v = view.Viewport;
    if (!width || !height || !view.View.allFinite() || !view.Projection.allFinite() ||
        !std::isfinite(v.X) || !std::isfinite(v.Y) || !std::isfinite(v.Width) || !std::isfinite(v.Height) ||
        v.X < 0 || v.Y < 0 || v.Width <= 0 || v.Height <= 0 || v.X + v.Width > width || v.Y + v.Height > height ||
        !std::isfinite(v.MinDepth) || !std::isfinite(v.MaxDepth) || v.MinDepth < 0 || v.MaxDepth > 1 || v.MinDepth > v.MaxDepth ||
        view.Scissor.X < 0 || view.Scissor.Y < 0 || uint64_t(view.Scissor.X) + view.Scissor.Width > width ||
        uint64_t(view.Scissor.Y) + view.Scissor.Height > height) return std::nullopt;
    ResolvedRasterView result{view.View, view.Projection, {}, v, view.Scissor, view.Depth == RasterDepthConvention::ReverseZeroToOne};
    if (view.Depth == RasterDepthConvention::MinusOneToOne) {
        result.Projection.row(2) = (view.Projection.row(2) + view.Projection.row(3)) * 0.5f;
    }
    result.ViewProjection = result.Projection * result.View;
    if (backend == render::RenderBackend::Vulkan) { result.Viewport.Y += v.Height; result.Viewport.Height = -v.Height; }
    return result;
}
}  // namespace radray
