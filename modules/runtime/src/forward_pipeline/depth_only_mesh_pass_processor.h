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
    void PrepareRecord(const RendererListDesc& desc, const RenderSceneSnapshot& scene,
                       const DrawRecord& record, MeshPassDrawListContext& out) override;
    void ResetView() noexcept;

private:
    void PrepareCommand(const RendererListDesc& desc, const RenderSceneSnapshot& scene, const MeshBatch& batch,
                         const MaterialPassRenderData& pass, bool mirrored, MeshPassDrawListContext& out);

private:
    struct ObjectPreparation {
        ObjectPreparation(const ShaderParameterLayout* layout, uint32_t group) : Values(layout, group) {}
        ShaderParameterStorage Values;
        unordered_map<RenderPrimitiveIndex, std::optional<PreparedShaderGroup>> Groups;
    };
    unordered_map<ShaderProgram*, ObjectPreparation> _objects;
    FrameDrawResources& _resources;
    DepthOnlyBindingCache& _bindings;
    unordered_map<ShaderProgram*, std::optional<PreparedShaderGroup>> _views;
};

}  // namespace radray::forward_detail
