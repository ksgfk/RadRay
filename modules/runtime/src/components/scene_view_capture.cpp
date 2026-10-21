#include <radray/runtime/components/scene_view_capture.h>

#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/game_framework/world.h>

namespace radray {

std::optional<SceneViewRequest> CaptureSceneView(const CameraComponent& camera, const Eigen::Vector4f& viewport) {
    if (!camera.IsLive()) return std::nullopt;
    auto world = camera.GetWorld();
    if (!world) return std::nullopt;
    const auto scene = world->GetRenderSceneId();
    if (!scene) return std::nullopt;
    SceneViewRequest request{*scene, camera.ComputeViewMatrix(), camera.GetFovY(), camera.GetNearZ(), camera.GetFarZ(), viewport, camera.GetProjection(), camera.GetOrthographicHeight()};
    if (!IsValidSceneView(request)) return std::nullopt;
    return request;
}

}  // namespace radray
