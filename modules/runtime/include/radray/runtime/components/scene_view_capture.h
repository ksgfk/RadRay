#pragma once

#include <radray/runtime/render_framework/scene_view.h>

namespace radray {
class CameraComponent;
/// GT: capture a live camera connected to a scene; empty for unavailable or invalid views.
std::optional<SceneViewRequest> CaptureSceneView(const CameraComponent& camera, const Eigen::Vector4f& viewport = {0, 0, 1, 1});
}  // namespace radray
