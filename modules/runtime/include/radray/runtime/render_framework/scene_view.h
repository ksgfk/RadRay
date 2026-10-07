#pragma once

#include <radray/basic_math.h>
#include <radray/render/rhi.h>
#include <radray/runtime/render_framework/scene_id.h>

namespace radray {

class Application;

enum class SceneProjection : uint8_t { Perspective,
                                       Orthographic };

struct SceneViewRequest {
    SceneId Scene;
    Eigen::Matrix4f View{Eigen::Matrix4f::Identity()};
    float FovY{Radian(60.0f)};
    float NearZ{0.1f};
    float FarZ{100.0f};
    /// Top-left origin, normalized to the actual render target.
    Eigen::Vector4f Viewport{0, 0, 1, 1};
    SceneProjection Projection{SceneProjection::Perspective};
    float OrthographicHeight{10.0f};
};

struct SceneViewData {
    Eigen::Matrix4f ViewProjection;
    Eigen::Matrix4f View;
    Eigen::Matrix4f Projection;
    radray::Viewport Viewport;
    Rect Scissor;
};

bool IsValidSceneView(const SceneViewRequest& request) noexcept;
std::optional<SceneViewData> ResolveSceneView(const SceneViewRequest& request, uint32_t width, uint32_t height, render::RenderBackend backend) noexcept;

class SceneViewCollector {
public:
    bool Add(const SceneViewRequest& request);

private:
    friend class Application;
    explicit SceneViewCollector(vector<SceneViewRequest>& requests) noexcept : _requests(requests) {}
    vector<SceneViewRequest>& _requests;
};

}  // namespace radray
