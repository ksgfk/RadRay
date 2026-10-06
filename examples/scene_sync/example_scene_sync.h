#pragma once

#include <atomic>
#include <chrono>
#include <sigslot/signal.hpp>

#include "scene_draw.h"
#include <radray/camera_control.h>
#include <radray/runtime/application.h>
#include <radray/runtime/components/camera_component.h>
#include <radray/runtime/components/static_mesh_component.h>

namespace radray::example {

class SceneSyncApp final : public Application {
public:
    uint32_t InstanceCount{1000};
    uint32_t ViewCount{1};
    uint32_t FrameLimit{0};
    bool Failed{false};

protected:
    void OnInit() override;
    void OnUpdate(const AppUpdateContext& context) override;
    void OnCollectRenderViews(SceneViewCollector& collector) override;
    void OnRender(AppFrameContext& frame) override;
    void OnShutdown() override;

private:
    void ResetCameraInput() noexcept;

    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<SceneComponent*> _parent{nullptr};
    unique_ptr<SceneDraw> _draw;
    CameraControl _cameraControl;
    array<sigslot::scoped_connection, 4> _inputConnections;
    bool _rightMouseDown{false};
    bool _middleMouseDown{false};
    std::chrono::steady_clock::time_point _fpsSampleStart{};
    std::atomic_uint32_t _fpsFrames{0};
    float _time{0};
    float _distance{30};
    uint32_t _updates{0};
};

}  // namespace radray::example
