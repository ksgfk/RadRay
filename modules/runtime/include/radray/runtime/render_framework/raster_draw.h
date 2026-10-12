#pragma once
#include <radray/runtime/render_framework/material.h>
#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/vertex_layout.h>

namespace radray {
/// Cold reflection result. Owned by RasterDraw, borrows a RenderSystem program generation.
struct RasterMaterialPlan {
    ShaderProgram* Program;
    vector<MaterialInput> Inputs;
    vector<render::BindingHandle> Handles;
    vector<render::ShaderBindingInfo> Bindings;
    vector<uint32_t> BufferSizes;
    string PushName;
    render::BindingHandle Push;
    uint32_t PushSize{0};
};
/// Values are borrowed only through PrepareMaterial. Empty overrides use material defaults.
struct RasterMaterialValues {
    std::span<const std::span<const byte>> Constants;
    std::span<render::TextureView* const> Textures;
    std::span<const render::SamplerDescriptor> Samplers;
    std::span<const std::span<const byte>> Views;
    std::span<const std::span<const byte>> Frames;
    std::optional<render::ShaderBufferBinding> Objects;
};
struct PreparedRasterMaterial {
    struct Group {
        uint32_t Index;
        render::ShaderParameterSet* Parameters;
        vector<render::ShaderParameterDynamicOffset> Offsets;
    };
    vector<Group> Groups;
    render::BindingHandle Push;
    uint32_t PushSize{0};
};
/// Execution slices only; no Scene or producer storage is retained.
struct PreparedRasterDraw {
    render::GraphicsPipelineState* Pipeline;
    vector<render::VertexBufferBinding> Vertices;
    std::optional<render::IndexBufferView> Indices;
    uint32_t Count{0}, First{0}, Instances{1}, FirstInstance{0};
    int32_t VertexOffset{0};
    radray::Viewport Viewport;
    radray::Rect Scissor;
    vector<byte> Push;
    void Record(render::GraphicsCommandEncoder* encoder, const PreparedRasterMaterial& material) const;
};
class RasterDraw {
public:
    Nullable<const RasterMaterialPlan*> ResolveMaterial(RenderSystem& renderer, const MaterialPass& pass);
    std::optional<PreparedRasterMaterial> PrepareMaterial(PipelineContext& context, const RasterMaterialPlan& plan,
        const MaterialRenderData& material, const RasterMaterialValues& values = {});
    Nullable<render::GraphicsPipelineState*> PreparePipeline(PipelineContext& context, const RasterMaterialPlan& plan,
        const MaterialPass& material, const GeometryVertexLayout& geometry, render::PrimitiveTopology topology,
        render::RenderPass* pass, const RenderOutputInfo& output, bool reverseCulling = false);

private:
    struct SamplerGroup {
        const RasterMaterialPlan* Plan;
        uint32_t Index;
        vector<render::SamplerDescriptor> Descriptors;
        unique_ptr<render::ShaderParameterSet> Parameters;
    };
    vector<unique_ptr<RasterMaterialPlan>> _plans;
    vector<SamplerGroup> _samplerGroups;
};
}  // namespace radray
