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

    // Call before reusing this processor for a list whose view differs from the previous one.
    // Drops view-group preparations and object preparations that depend on the view (motion vectors);
    // material groups and view-independent object groups are kept.
    void ResetView() noexcept;

private:
    struct ObjectPreparation {
        ObjectPreparation(const ShaderParameterLayout* layout, uint32_t group);
        ShaderParameterStorage Values;
        const ShaderParameterInfo* LocalToWorld{nullptr};
        const ShaderParameterInfo* NormalToWorld{nullptr};
        const ShaderParameterInfo* PreviousLocalToWorld{nullptr};
        const ShaderParameterInfo* MotionValid{nullptr};
        unordered_map<RenderPrimitiveIndex, std::optional<PreparedShaderGroup>> Groups;
        bool ViewDependent() const noexcept { return PreviousLocalToWorld != nullptr; }
    };
    unordered_map<ShaderProgram*, ObjectPreparation> _objects;
    FrameDrawResources& _resources;
    ForwardBindingCache& _bindings;
    bool& _lightOverflowWarned;
    Nullable<const RenderPipelineContext*> _temporal;
    unordered_map<ShaderProgram*, std::optional<PreparedShaderGroup>> _views;
    unordered_map<ShaderProgram*, unordered_map<RenderMaterialIndex, std::optional<PreparedShaderGroup>>> _materials;
};

}  // namespace radray::forward_detail
