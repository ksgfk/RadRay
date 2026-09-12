#pragma once

#include "forward_bindings.h"
#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>
#include <radray/runtime/render_framework/cbuffer_view.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

namespace radray::forward_detail {

class DepthOnlyMeshPassProcessor final : public MeshPassProcessor {
public:
    DepthOnlyMeshPassProcessor(FrameDrawResources& resources, DepthOnlyBindingCache& bindings, const PackedCBufferTable& objects)
        : _resources(resources), _bindings(bindings), _objects(objects) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;
    void ResetView() noexcept;

private:
    FrameDrawBindingId PrepareBindings(const RendererListDesc& desc, ShaderProgram& program, uint32_t primitive,
                                       uint32_t viewGroup, uint32_t objectGroup, bool viewFirst);

    FrameDrawResources& _resources;
    DepthOnlyBindingCache& _bindings;
    // Object rows frozen at PrepareFrame. DepthOnly has no temporal context, so rows upload as frozen.
    const PackedCBufferTable& _objects;
    FrameCBufferIdentity _viewValues;
    uint64_t _viewEpoch{0};
    Forward_ViewData _viewScratch{};
};

}  // namespace radray::forward_detail
