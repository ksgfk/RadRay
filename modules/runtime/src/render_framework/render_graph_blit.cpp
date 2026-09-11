#include <radray/runtime/render_framework/render_graph_blit.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {
#include "render_graph_blit_shaders.inc"

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
}  // namespace
RgTextureValue AddRenderGraphBlit(RenderGraph& graph, RenderSystem& system, render::RenderBackend backend,
                                  RgTextureValue source, RgTextureValue destination, bool decodeSrgb, bool encodeSrgb) {
    const bool dxil = backend == render::RenderBackend::D3D12;
    const auto program = system.GetOrCreateShaderProgram(dxil ? std::as_bytes(std::span<const unsigned char>{blit_dxil}) : std::as_bytes(std::span<const unsigned char>{blit_spirv}), dxil ? blit_dxil_identity : blit_spirv_identity);
    const auto desc = graph.GetTextureDescriptor(destination);
    if (!program || !desc) {
        graph.AddDiagnostic("BlitProgram", "Blit requires a supported destination and built-in shader artifact");
        return {};
    }
    destination = graph.NextVersion(destination);
    struct Data {
        Nullable<ShaderProgram*> Program{nullptr};
        RgTextureViewHandle Image{};
        array<float, 8> Constants{};
        Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
        PreparedShaderGroup Parameters{};
        uint32_t Width{0}, Height{0};
        render::RenderBackend Backend{};
    };
    graph.AddRasterPass<Data>("Display.Blit", [&](Data& data, RenderGraphRasterBuilder& builder) {
        data.Program = program; data.Image = builder.ReadTexture(source);
        data.Constants = {0, 0, 0, 0, decodeSrgb ? 1.f : 0.f, encodeSrgb ? 1.f : 0.f, 0, 0};
        data.Width = desc->Width; data.Height = desc->Height; data.Backend = backend;
        builder.SetColorAttachment(0, destination); }, +[](Data& data, RenderGraphPrepareContext& context) {
        data.Pipeline = context.ResolveGraphicsPipeline(*data.Program, BlitState());
        if (!data.Pipeline) return false;
        const RgParameterBinding bindings[]{
            {"Blit", 0, RgCBufferParameterBinding{std::as_bytes(std::span{data.Constants})}},
            {"Image", 0, RgTextureParameterBinding{data.Image}},
            {"ImageSampler", 0, RgSamplerParameterBinding{BlitSampler()}}};
        data.Parameters = context.CreateParameterSet(*data.Program, 0, bindings);
        return data.Parameters.IsValid(); }, +[](const Data& data, RenderGraphRasterContext& context) {
        auto& encoder = context.Encoder();
        encoder.BindGraphicsPipelineState(data.Pipeline.Get());
        encoder.BindShaderParameterSet(data.Parameters);
        encoder.SetViewport(MakeViewport(data.Backend, 0, 0, float(data.Width), float(data.Height)));
        encoder.SetScissor({0, 0, data.Width, data.Height});
        encoder.Draw(3, 1, 0, 0); });
    return destination;
}
}  // namespace radray
