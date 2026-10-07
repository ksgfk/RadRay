#include <radray/runtime/render_framework/scene_view.h>

#include <algorithm>
#include <cmath>

namespace radray {

bool IsValidSceneView(const SceneViewRequest& request) noexcept {
    const auto& v = request.Viewport;
    return request.Scene.IsValid() && request.View.allFinite() && v.allFinite() &&
           ((request.Projection == SceneProjection::Perspective && std::isfinite(request.FovY) && request.FovY > 0 && request.FovY < Radian(180.0f)) ||
            (request.Projection == SceneProjection::Orthographic && std::isfinite(request.OrthographicHeight) && request.OrthographicHeight > 0)) &&
           std::isfinite(request.NearZ) && std::isfinite(request.FarZ) && request.NearZ > 0 && request.FarZ > request.NearZ &&
           v.x() >= 0 && v.y() >= 0 && v.z() > 0 && v.w() > 0 && v.x() + v.z() <= 1.000001f && v.y() + v.w() <= 1.000001f;
}
std::optional<SceneViewData> ResolveSceneView(const SceneViewRequest& request, uint32_t width, uint32_t height, render::RenderBackend backend) noexcept {
    if (!IsValidSceneView(request) || width == 0 || height == 0) return std::nullopt;
    const auto& v = request.Viewport;
    const uint32_t x = static_cast<uint32_t>(std::round(v.x() * width));
    const uint32_t y = static_cast<uint32_t>(std::round(v.y() * height));
    const uint32_t right = std::min(width, static_cast<uint32_t>(std::round((v.x() + v.z()) * width)));
    const uint32_t bottom = std::min(height, static_cast<uint32_t>(std::round((v.y() + v.w()) * height)));
    if (right <= x || bottom <= y) return std::nullopt;
    const float w = static_cast<float>(right - x), h = static_cast<float>(bottom - y);
    SceneViewData result;
    result.View = request.View;
    if (request.Projection == SceneProjection::Perspective) {
        result.Projection = PerspectiveLH<float>(request.FovY, w / h, request.NearZ, request.FarZ);
    } else {
        const float halfHeight = request.OrthographicHeight * 0.5f;
        const float halfWidth = halfHeight * w / h;
        result.Projection = OrthoLH<float>(-halfWidth, halfWidth, -halfHeight, halfHeight, request.NearZ, request.FarZ);
    }
    result.ViewProjection = result.Projection * result.View;
    result.Viewport = {float(x), float(y), w, h, 0, 1};
    if (backend == render::RenderBackend::Vulkan) {
        result.Viewport.Y += h;
        result.Viewport.Height = -h;
    }
    result.Scissor = {static_cast<int32_t>(x), static_cast<int32_t>(y), right - x, bottom - y};
    return result;
}
bool SceneViewCollector::Add(const SceneViewRequest& request) {
    if (!IsValidSceneView(request)) return false;
    _requests.push_back(request);
    return true;
}

}  // namespace radray
