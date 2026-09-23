#pragma once

#include "scene_draw.h"
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
    Nullable<CameraComponent*> _camera{nullptr};
    Nullable<SceneComponent*> _parent{nullptr};
    unique_ptr<SceneDraw> _draw;
    float _time{0};
    float _distance{30};
    uint32_t _updates{0};
};

}  // namespace radray::example
