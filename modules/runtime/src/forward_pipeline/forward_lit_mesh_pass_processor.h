#pragma once

#include "forward_bindings.h"
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray::forward_detail {

class ForwardLitMeshPassProcessor final : public MeshPassProcessor {
public:
    ForwardLitMeshPassProcessor(FrameDrawResources& resources, ForwardBindingCache& bindings, bool& lightOverflowWarned,
                                Nullable<const RenderPipelineContext*> temporal = nullptr)
        : _resources(resources), _bindings(bindings), _lightOverflowWarned(lightOverflowWarned), _temporal(temporal) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;

private:
    FrameDrawResources& _resources;
    ForwardBindingCache& _bindings;
    bool& _lightOverflowWarned;
    Nullable<const RenderPipelineContext*> _temporal;
    unordered_map<ShaderProgram*, std::optional<PreparedShaderGroup>> _views;
    unordered_map<RenderMaterialIndex, std::optional<PreparedShaderGroup>> _materials;
};

}  // namespace radray::forward_detail
