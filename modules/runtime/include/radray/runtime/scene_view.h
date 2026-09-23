#pragma once

#include <radray/basic_math.h>
#include <radray/render/rhi.h>
#include <radray/runtime/render_scene/scene_id.h>

namespace radray {

class Application;
class CameraComponent;

struct SceneViewRequest {
    SceneId Scene;
    Eigen::Matrix4f View{Eigen::Matrix4f::Identity()};
    float FovY{Radian(60.0f)};
    float NearZ{0.1f};
    float FarZ{100.0f};
    /// Top-left origin, normalized to the actual render target.
    Eigen::Vector4f Viewport{0, 0, 1, 1};
};

struct SceneViewData {
    Eigen::Matrix4f ViewProjection;
    radray::Viewport Viewport;
    Rect Scissor;
};

bool IsValidSceneView(const SceneViewRequest& request) noexcept;
std::optional<SceneViewData> ResolveSceneView(const SceneViewRequest& request, uint32_t width, uint32_t height, render::RenderBackend backend) noexcept;

class SceneViewCollector {
public:
    bool Add(const SceneViewRequest& request);
    bool Add(const CameraComponent& camera, const Eigen::Vector4f& viewport = {0, 0, 1, 1});

private:
    friend class Application;
    explicit SceneViewCollector(vector<SceneViewRequest>& requests) noexcept : _requests(requests) {}
    vector<SceneViewRequest>& _requests;
};

}  // namespace radray
