#pragma once

#include "forward_bindings.h"
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

namespace radray::forward_detail {

class DepthOnlyMeshPassProcessor final : public MeshPassProcessor {
public:
    DepthOnlyMeshPassProcessor(FrameDrawResources& resources, DepthOnlyBindingCache& bindings)
        : _resources(resources), _bindings(bindings) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;

private:
    FrameDrawResources& _resources;
    DepthOnlyBindingCache& _bindings;
    unordered_map<ShaderProgram*, std::optional<PreparedShaderGroup>> _views;
};

}  // namespace radray::forward_detail
