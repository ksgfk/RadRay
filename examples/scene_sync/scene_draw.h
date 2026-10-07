#pragma once

#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include <radray/runtime/gpu_system.h>

namespace radray::example {

/// Example-owned fixed unlit pass. All bindings and pipelines survive frame recording.
class SceneDraw {
public:
    bool Initialize(RenderSystem& renderer, render::Device* device, render::RenderPass* pass, render::TextureFormat format, uint32_t flights);
    bool Draw(SceneManager& scenes, AppFrameContext& frame, render::GraphicsCommandEncoder* encoder,
              const SceneViewRequest& request, const SceneGpuView& objects, uint32_t viewIndex, uint32_t width, uint32_t height);

private:
    struct ViewResources {
        unique_ptr<MappedUploadPage> Constants;
        unique_ptr<render::ShaderParameterSet> Parameters;
    };
    struct FlightViews {
        vector<ViewResources> Views;
        size_t Used{0};
        uint64_t Serial{0};
    };
    vector<FlightViews> _flights;
    Nullable<render::PipelineLayout*> _layout;
    array<unique_ptr<render::GraphicsPipelineState>, 2> _pipelines;
    render::BindingHandle _objects, _view, _draw;
    uint32_t _group{0};
};

unique_ptr<StaticMesh> CreateCube(GpuSystem& gpu);

}  // namespace radray::example
