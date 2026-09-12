#include <radray/runtime/render_framework/render_graph_blit.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>

namespace radray {
struct RenderGraphBlitFrameData {
    Nullable<RenderSystem*> System{nullptr};
    render::RenderBackend Backend{};
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    PreparedShaderGroup Parameters{};
};

namespace {
#include "render_graph_blit_shaders.inc"

struct BlitRecipe {
    RgTextureViewHandle Image;
    array<float, 8> Constants{};
    uint32_t Width{0}, Height{0};
};
MaterialPipelineState BlitState() {
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
    return state;
}
render::SamplerDescriptor BlitSampler() {
    render::SamplerDescriptor sampler;
    sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::ClampToEdge;
    return sampler;
}
void DeclareBlit(BlitRecipe& recipe, RenderGraphRasterBuilder& builder, const render::TextureDescriptor& destinationDesc,
                 RgTextureValue source, RgTextureValue destination, bool decodeSrgb, bool encodeSrgb) {
    recipe.Image = builder.ReadTexture(source);
    recipe.Constants = {0, 0, 0, 0, decodeSrgb ? 1.f : 0.f, encodeSrgb ? 1.f : 0.f, 0, 0};
    recipe.Width = destinationDesc.Width;
    recipe.Height = destinationDesc.Height;
    builder.SetColorAttachment(0, destination);
}
bool PrepareBlit(const BlitRecipe& recipe, RenderGraphBlitFrameData& data, RenderGraphPrepareContext& context) {
    if (!data.System) return false;
    const bool dxil = data.Backend == render::RenderBackend::D3D12;
    const auto program = data.System->GetOrCreateShaderProgram(dxil ? std::as_bytes(std::span<const unsigned char>{blit_dxil}) : std::as_bytes(std::span<const unsigned char>{blit_spirv}), dxil ? blit_dxil_identity : blit_spirv_identity);
    if (!program) return false;
    data.Pipeline = context.ResolveGraphicsPipeline(*program, BlitState());
    if (!data.Pipeline) return false;
    const RgParameterBinding bindings[]{
        {"Blit", 0, RgCBufferParameterBinding{std::as_bytes(std::span{recipe.Constants})}},
        {"Image", 0, RgTextureParameterBinding{recipe.Image}},
        {"ImageSampler", 0, RgSamplerParameterBinding{BlitSampler()}}};
    data.Parameters = context.CreateParameterSet(*program, 0, bindings);
    return data.Parameters.IsValid();
}
void RecordBlit(const BlitRecipe& recipe, const RenderGraphBlitFrameData& data, RenderGraphRasterContext& context) {
    auto& encoder = context.Encoder();
    encoder.BindGraphicsPipelineState(data.Pipeline.Get());
    encoder.BindShaderParameterSet(data.Parameters);
    encoder.SetViewport(MakeViewport(data.Backend, 0, 0, float(recipe.Width), float(recipe.Height)));
    encoder.SetScissor({0, 0, recipe.Width, recipe.Height});
    encoder.Draw(3, 1, 0, 0);
}
}  // namespace

RenderGraphBlitTemplatePass DeclareRenderGraphBlit(RenderGraph& builder, RgTextureValue source,
                                                   RgTextureValue destination, bool decodeSrgb, bool encodeSrgb) {
    const auto desc = builder.GetTextureDescriptor(destination);
    if (!desc) {
        builder.AddDiagnostic("BlitProgram", "Blit requires a supported destination descriptor");
        return {};
    }
    destination = builder.NextVersion(destination);
    const auto slot = builder.DeclareTemplateSlot<RenderGraphBlitFrameData>();
    builder.AddTemplateRasterPass<BlitRecipe>("Display.Blit", slot, [&](BlitRecipe& recipe, RenderGraphRasterBuilder& pass) { DeclareBlit(recipe, pass, *desc, source, destination, decodeSrgb, encodeSrgb); }, &PrepareBlit, &RecordBlit);
    return {destination, slot};
}
bool BindRenderGraphBlit(const RenderGraphTemplateInstance& instance, const RenderGraphBlitTemplatePass& pass,
                         RenderSystem& system, render::RenderBackend backend) {
    return instance.Bind(pass.Frame, make_shared<RenderGraphBlitFrameData>(RenderGraphBlitFrameData{&system, backend}));
}

RgTextureValue AddRenderGraphBlit(RenderGraph& graph, RenderSystem& system, render::RenderBackend backend,
                                  RgTextureValue source, RgTextureValue destination, bool decodeSrgb, bool encodeSrgb) {
    const auto desc = graph.GetTextureDescriptor(destination);
    if (!desc) {
        graph.AddDiagnostic("BlitProgram", "Blit requires a supported destination descriptor");
        return {};
    }
    destination = graph.NextVersion(destination);
    struct Data {
        BlitRecipe Recipe;
        RenderGraphBlitFrameData Frame;
    };
    graph.AddRasterPass<Data>("Display.Blit", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data.Frame = {&system, backend};
        DeclareBlit(data.Recipe, builder, *desc, source, destination, decodeSrgb, encodeSrgb); }, +[](Data& data, RenderGraphPrepareContext& context) { return PrepareBlit(data.Recipe, data.Frame, context); }, +[](const Data& data, RenderGraphRasterContext& context) { RecordBlit(data.Recipe, data.Frame, context); });
    return destination;
}
}  // namespace radray
