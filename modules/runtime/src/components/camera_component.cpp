#include <radray/runtime/components/camera_component.h>

#include <radray/runtime/renderer/scene_renderer.h>

namespace radray {

void CameraComponent::FillSceneView(SceneView& view, uint32_t width, uint32_t height) const noexcept {
    const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    view.ViewMatrix = ComputeViewMatrix();
    view.ProjMatrix = ComputeProjMatrix(aspect);
    view.ViewProjMatrix = view.ProjMatrix * view.ViewMatrix;
    view.EyePosition = GetWorldLocation();
    view.ViewportWidth = width;
    view.ViewportHeight = height;
}

}  // namespace radray
