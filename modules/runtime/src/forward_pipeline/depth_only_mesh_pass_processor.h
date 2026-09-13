#pragma once

#include "forward_bindings.h"
#include <radray/runtime/forward_pipeline/gen_forward_cbuffers.h>
#include <radray/runtime/render_framework/cbuffer_view.h>
#include <radray/runtime/render_framework/frame_draw_resources.h>
#include <radray/runtime/render_framework/mesh_pass_processor.h>

namespace radray::forward_detail {

class DepthOnlyMeshPassProcessor final : public MeshPassProcessor {
public:
    DepthOnlyMeshPassProcessor(FrameDrawResources& resources, DepthOnlyBindingCache& bindings, CBufferRows objects)
        : _resources(&resources), _bindings(&bindings), _objects(objects) {}
    bool PrepareBatch(std::span<const MeshPassListPreparation> batches) override;
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;
    void ResetView() noexcept;
    void ResetFrame(FrameDrawResources& resources, DepthOnlyBindingCache& bindings, CBufferRows objects) noexcept {
        _resources = &resources;
        _bindings = &bindings;
        _objects = objects;
        ResetView();
    }

private:
    FrameDrawBindingId PrepareBindings(const RendererListDesc& desc, ShaderProgram& program, uint32_t primitive,
                                       uint32_t viewGroup, uint32_t objectGroup, bool viewFirst, uint32_t planIndex = UINT32_MAX);

    struct BatchBinding {
        const DrawRecord* Record;
        FrameDrawBindingId Binding;
        MeshPassRejectReason Failure;
    };
    vector<BatchBinding> _batchBindings;
    size_t _batchCursor{0};
    uint64_t _batchEpoch{0};
    FrameDrawResources* _resources;
    DepthOnlyBindingCache* _bindings;
    // Object rows frozen at PrepareFrame. DepthOnly has no temporal context, so rows upload as frozen.
    CBufferRows _objects;
    FrameCBufferIdentity _viewValues;
    FrameParameterDomain _viewDomain, _objectDomain;
    struct PlanGroups {
        uint64_t ViewRevision{0};
        FrameParameterGroup View, Object;
        FrameParameterDomain Binding;
    };
    vector<PlanGroups> _planGroups;
    uint64_t _viewRevision{0};
    uint64_t _viewEpoch{0};
    Forward_ViewData _viewScratch{};
};

}  // namespace radray::forward_detail
