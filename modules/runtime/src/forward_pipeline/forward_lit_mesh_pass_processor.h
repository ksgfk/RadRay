#pragma once

#include "forward_bindings.h"
#include <algorithm>
#include <limits>
#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>
#include <radray/runtime/render_framework/cbuffer_view.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>
#include <radray/runtime/render_framework/render_pipeline.h>

namespace radray::forward_detail {

/// One processor serves renderer lists built from a single RenderSceneSnapshot within one frame:
/// material and primitive preparations are keyed by snapshot indices.
class ForwardLitMeshPassProcessor final : public MeshPassProcessor {
public:
    ForwardLitMeshPassProcessor(FrameDrawResources& resources, ForwardBindingCache& bindings, bool& lightOverflowWarned,
                                const PackedCBufferTable& objects, Nullable<const RenderPipelineContext*> temporal = nullptr)
        : _resources(resources), _bindings(bindings), _lightOverflowWarned(lightOverflowWarned), _objects(objects), _temporal(temporal) {}
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;

    // Call at the boundary of a different or updated view. Frame tables keep successful tuples.
    void ResetView() noexcept;
    uint64_t DuplicateSameFramePreparations() const noexcept { return _resources.GetStats().SharedGroupHits; }

private:
    FrameDrawBindingId PrepareBindings(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                       uint32_t materialIndex, uint32_t primitiveIndex,
                                       const MaterialPassRenderData& pass, const StaticBindingRecipe& binding);
    FrameDrawResources& _resources;
    ForwardBindingCache& _bindings;
    bool& _lightOverflowWarned;
    const PackedCBufferTable& _objects;
    Nullable<const RenderPipelineContext*> _temporal;
    array<Forward_ViewData, 2> _viewScratch{};
    array<FrameCBufferIdentity, 2> _viewValues{};
    Forward_ObjectData _objectScratch{};
    uint64_t _viewEpoch{0};
};

}  // namespace radray::forward_detail
