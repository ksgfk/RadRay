#include <radray/runtime/render_framework/render_graph_blit.h>
#include <radray/runtime/render_framework/viewport.h>
#include <radray/runtime/render_system.h>

namespace radray {
namespace {
#include "render_graph_blit_shaders.inc"
}
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
        RgGraphicsProgramHandle Program;
        RgParameterSetHandle Parameters;
        uint32_t Width, Height;
        render::RenderBackend Backend;
    };
    const array<float, 8> constants{0, 0, 0, 0, decodeSrgb ? 1.f : 0.f, encodeSrgb ? 1.f : 0.f, 0, 0};
    MaterialPipelineState state;
    state.Primitive.Cull = render::CullMode::None;
    state.DepthStencil.DepthTestEnable = state.DepthStencil.DepthWriteEnable = false;
    render::SamplerDescriptor sampler;
    sampler.AddressS = sampler.AddressT = sampler.AddressR = render::AddressMode::ClampToEdge;
    graph.AddRasterPass<Data>("Display.Blit", [&](Data& data, RenderGraphRasterBuilder& builder) {
        const RgParameterBinding bindings[]{
            {"Blit", 0, RgCBufferParameterBinding{std::as_bytes(std::span{constants})}},
            {"Image", 0, RgTextureParameterBinding{source}},
            {"ImageSampler", 0, RgSamplerParameterBinding{sampler}}};
        data = {builder.UseGraphicsProgram(*program, state), builder.CreateParameterSet(*program, 0, bindings), desc->Width, desc->Height, backend};
        builder.SetColorAttachment(0, destination); }, +[](const Data& data, RenderGraphRasterContext& context) {
        context.BindGraphicsProgram(data.Program); context.BindParameterSet(data.Parameters);
        context.Encoder().SetViewport(MakeViewport(data.Backend, 0, 0, float(data.Width), float(data.Height)));
        context.Encoder().SetScissor({0, 0, data.Width, data.Height});
        context.Encoder().Draw(3, 1, 0, 0); });
    return destination;
}
}  // namespace radray
