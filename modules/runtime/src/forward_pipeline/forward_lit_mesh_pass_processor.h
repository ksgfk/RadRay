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
                                CBufferRows objects, Nullable<const RenderPipelineContext*> temporal = nullptr)
        : _resources(&resources), _bindings(&bindings), _lightOverflowWarned(&lightOverflowWarned), _objects(objects), _temporal(temporal) {}
    bool PrepareBatch(std::span<const MeshPassListPreparation> batches) override;
    void AddMeshBatch(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                      const MeshBatch& batch, MeshPassDrawListContext& out) override;
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;

    // Call at the boundary of a different or updated view. Frame tables keep successful tuples.
    void ResetView() noexcept;
    void ResetFrame(FrameDrawResources& resources, ForwardBindingCache& bindings, bool& warned,
                    CBufferRows objects, Nullable<const RenderPipelineContext*> temporal = nullptr) noexcept {
        _resources = &resources;
        _bindings = &bindings;
        _lightOverflowWarned = &warned;
        _objects = objects;
        _temporal = temporal;
        ResetView();
    }
    uint64_t DuplicateSameFramePreparations() const noexcept { return _resources->GetStats().SharedGroupHits; }

private:
    FrameDrawBindingId PrepareBindings(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                                       uint32_t materialIndex, uint32_t primitiveIndex,
                                       const MaterialPassRenderData& pass, const StaticBindingRecipe& binding, uint32_t planIndex = UINT32_MAX, uint32_t batchIndex = UINT32_MAX, std::optional<bool> separateLighting = std::nullopt);
    struct BatchBinding {
        const DrawRecord* Record;
        FrameDrawBindingId Binding;
        MeshPassRejectReason Failure;
    };
    vector<BatchBinding> _batchBindings;
    size_t _batchCursor{0};
    uint64_t _batchEpoch{0};
    FrameDrawResources* _resources;
    ForwardBindingCache* _bindings;
    bool* _lightOverflowWarned;
    CBufferRows _objects;
    Nullable<const RenderPipelineContext*> _temporal;
    array<Forward_ViewData, 2> _viewScratch{};
    array<FrameCBufferIdentity, 2> _viewValues{};
    array<FrameParameterDomain, 2> _viewDomains{};
    FrameParameterDomain _objectDomain, _materialDomain;
    struct PlanGroups {
        uint64_t ViewRevision{0};
        array<FrameParameterGroup, 2> Views{};
        array<FrameParameterDomain, 2> Bindings{};
        FrameParameterGroup Object, Material;
    };
    vector<PlanGroups> _planGroups;
    uint64_t _viewRevision{0};
    Forward_ObjectData _objectScratch{};
    uint64_t _viewEpoch{0};
};

}  // namespace radray::forward_detail
