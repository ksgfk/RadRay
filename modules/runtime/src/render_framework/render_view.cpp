#include <radray/runtime/render_framework/render_view.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <radray/logger.h>

namespace radray {
namespace {
std::atomic<uint64_t> NextViewStateId{1};

bool ValidRect(const NormalizedRect& rect) noexcept {
    return std::isfinite(rect.X) && std::isfinite(rect.Y) && std::isfinite(rect.Width) && std::isfinite(rect.Height) &&
           rect.X >= 0 && rect.Y >= 0 && rect.Width > 0 && rect.Height > 0 &&
           double{rect.X} + rect.Width <= 1 && double{rect.Y} + rect.Height <= 1;
}

Rect ResolveRect(const NormalizedRect& rect, RenderExtent extent) noexcept {
    const auto minX = static_cast<uint32_t>(std::floor(double{rect.X} * extent.Width));
    const auto minY = static_cast<uint32_t>(std::floor(double{rect.Y} * extent.Height));
    const auto maxX = std::min(extent.Width, static_cast<uint32_t>(std::ceil((double{rect.X} + rect.Width) * extent.Width)));
    const auto maxY = std::min(extent.Height, static_cast<uint32_t>(std::ceil((double{rect.Y} + rect.Height) * extent.Height)));
    return {static_cast<int32_t>(minX), static_cast<int32_t>(minY), maxX - minX, maxY - minY};
}
}  // namespace

ViewStateId AllocateViewStateId() noexcept {
    const uint64_t value = NextViewStateId.fetch_add(1, std::memory_order_relaxed);
    if (value == 0 || value == UINT64_MAX) RADRAY_ABORT("ViewStateId exhausted");
    return {value};
}

bool ValidateRenderView(const RenderViewDesc& view, string& reason) {
    if (!ValidRect(view.ViewRect) || !ValidRect(view.ScissorRect)) {
        reason = "ViewRect and ScissorRect must be finite, nonempty and contained in [0,1]";
        return false;
    }
    if (!view.WorldToView.allFinite() || !view.WorldPosition.allFinite() || !view.JitterPixels.allFinite() ||
        !std::isfinite(view.LodBias) || view.LodBias <= 0) {
        reason = "View transform, position, jitter or LodBias is invalid";
        return false;
    }
    const bool valid = std::visit([](const auto& projection) {
        using T = std::decay_t<decltype(projection)>;
        if constexpr (std::is_same_v<T, ExplicitProjectionDesc>) {
            return projection.Matrix.allFinite();
        } else {
            if (!std::isfinite(projection.NearPlane) || !std::isfinite(projection.FarPlane) || projection.FarPlane <= projection.NearPlane) return false;
            if constexpr (std::is_same_v<T, PerspectiveProjectionDesc>) {
                return projection.NearPlane > 0 && std::isfinite(projection.VerticalFovRadians) &&
                       projection.VerticalFovRadians > 0 && projection.VerticalFovRadians < std::numbers::pi_v<float>;
            } else {
                return std::isfinite(projection.VerticalSize) && projection.VerticalSize > 0;
            }
        }
    },
                                  view.Projection);
    if (!valid) reason = "Projection parameters are invalid";
    return valid;
}

std::optional<ResolvedRenderViewFamily> ResolveRenderViewFamily(
    const RenderViewFamilyDesc& family, const RenderOutputInfo& output, uint32_t index, uint32_t maxDimension, string& reason) {
    if (family.Output != output.Id || output.Width == 0 || output.Height == 0 ||
        !std::isfinite(family.RenderScale) || family.RenderScale <= 0) {
        reason = "Output identity, extent or RenderScale is invalid";
        return std::nullopt;
    }
    const double width = std::ceil(static_cast<double>(output.Width) * family.RenderScale);
    const double height = std::ceil(static_cast<double>(output.Height) * family.RenderScale);
    if (width > maxDimension || height > maxDimension || width > INT32_MAX || height > INT32_MAX) {
        reason = "RenderScale produces an extent beyond DeviceLimits";
        return std::nullopt;
    }
    ResolvedRenderViewFamily result{.FrameLocalIndex = index, .Name = family.Name, .OutputId = output.Id, .OutputSize = {output.Width, output.Height}, .RenderSize = {std::max(1u, static_cast<uint32_t>(width)), std::max(1u, static_cast<uint32_t>(height))}, .OutputFormat = output.Format, .SampleCount = output.SampleCount, .OutputAvailable = output.Active};
    result.Views.reserve(family.Views.size());
    for (const auto& view : family.Views) {
        if (!ValidateRenderView(view, reason)) return std::nullopt;
        ResolvedRenderView resolved{};
        resolved.Name = view.Name;
        resolved.StateId = view.StateId;
        resolved.View = view.WorldToView;
        resolved.WorldPosition = view.WorldPosition;
        resolved.ViewRect = ResolveRect(view.ViewRect, result.RenderSize);
        resolved.ScissorRect = ResolveRect(view.ScissorRect, result.RenderSize);
        if (resolved.ViewRect.Width == 0 || resolved.ViewRect.Height == 0 || resolved.ScissorRect.Width == 0 || resolved.ScissorRect.Height == 0) {
            reason = "View resolves to an empty pixel rectangle";
            return std::nullopt;
        }
        const float aspect = static_cast<float>(resolved.ViewRect.Width) / resolved.ViewRect.Height;
        resolved.Projection = std::visit([aspect](const auto& projection) -> Eigen::Matrix4f {
            using T = std::decay_t<decltype(projection)>;
            if constexpr (std::is_same_v<T, ExplicitProjectionDesc>)
                return projection.Matrix;
            else if constexpr (std::is_same_v<T, PerspectiveProjectionDesc>)
                return PerspectiveLH(projection.VerticalFovRadians, aspect, projection.NearPlane, projection.FarPlane);
            else {
                const float halfHeight = projection.VerticalSize * 0.5f;
                const float halfWidth = halfHeight * aspect;
                return OrthoLH(-halfWidth, halfWidth, -halfHeight, halfHeight, projection.NearPlane, projection.FarPlane);
            }
        },
                                         view.Projection);
        resolved.JitterNdc = {2 * view.JitterPixels.x() / resolved.ViewRect.Width, -2 * view.JitterPixels.y() / resolved.ViewRect.Height};
        Eigen::Matrix4f jitter = Eigen::Matrix4f::Identity();
        jitter(0, 3) = resolved.JitterNdc.x();
        jitter(1, 3) = resolved.JitterNdc.y();
        resolved.Projection = jitter * resolved.Projection;
        resolved.ViewProjection = resolved.Projection * resolved.View;
        resolved.PreviousViewProjection = resolved.ViewProjection;
        resolved.LayerMask = view.LayerMask;
        resolved.LodBias = view.LodBias;
        resolved.CameraCut = view.CameraCut;
        result.Views.push_back(std::move(resolved));
    }
    return result;
}

std::optional<render::TextureDescriptor> ResolveRuntimeTextureDesc(
    const RuntimeTextureDesc& desc, const ResolvedRenderViewFamily& family, const render::Device& device, string& reason) {
    const auto& spec = desc.Extent;
    if (!EnumContains(spec.Mode) || spec.AlignX == 0 || spec.AlignY == 0 || spec.MinWidth == 0 || spec.MinHeight == 0 ||
        !std::isfinite(spec.ScaleX) || !std::isfinite(spec.ScaleY) || spec.ScaleX <= 0 || spec.ScaleY <= 0 ||
        (spec.Mode == RenderExtentMode::Absolute && (spec.Width == 0 || spec.Height == 0))) {
        reason = "Invalid relative extent, alignment or scale";
        return std::nullopt;
    }
    const auto base = spec.Mode == RenderExtentMode::RelativeToFamilyRenderExtent ? family.RenderSize : family.OutputSize;
    const double width = spec.Mode == RenderExtentMode::Absolute ? spec.Width : std::ceil(base.Width * double{spec.ScaleX});
    const double height = spec.Mode == RenderExtentMode::Absolute ? spec.Height : std::ceil(base.Height * double{spec.ScaleY});
    const auto limit = device.GetCapabilities().Limits.MaxTexture2DDimension;
    if (width > limit || height > limit) {
        reason = "Relative extent exceeds DeviceLimits";
        return std::nullopt;
    }
    const auto align = [](uint64_t value, uint32_t alignment) { return ((value + alignment - 1) / alignment) * alignment; };
    const uint64_t alignedWidth = align(std::max(uint64_t{spec.MinWidth}, static_cast<uint64_t>(width)), spec.AlignX);
    const uint64_t alignedHeight = align(std::max(uint64_t{spec.MinHeight}, static_cast<uint64_t>(height)), spec.AlignY);
    if (alignedWidth > limit || alignedHeight > limit) {
        reason = "Aligned extent exceeds DeviceLimits";
        return std::nullopt;
    }
    render::TextureDescriptor result{desc.Dimension, static_cast<uint32_t>(alignedWidth), static_cast<uint32_t>(alignedHeight),
                                     desc.DepthOrArraySize, desc.MipLevels, desc.SampleCount, desc.Format, render::MemoryType::Device, desc.Usage, desc.Hints};
    const auto validation = render::ValidateTextureDescriptor(result, device);
    if (!validation.Supported) {
        reason = validation.Reason;
        return std::nullopt;
    }
    return result;
}

std::optional<render::TextureFormat> SelectFirstSupportedFormat(
    const render::Device& device, std::span<const render::TextureFormat> candidates,
    render::TextureDimension dimension, render::TextureUses usage, uint32_t sampleCount) {
    if (!EnumContains(static_cast<render::SampleCount>(sampleCount))) return std::nullopt;
    for (const auto format : candidates) {
        const auto support = device.QueryTextureSupport({dimension, format, usage});
        if (support.Supported && support.SampleCounts.HasFlag(static_cast<render::SampleCount>(sampleCount))) return format;
    }
    return std::nullopt;
}

}  // namespace radray
