#pragma once
#include <radray/runtime/render_framework/render_graph.h>

namespace radray {
class RenderSystem;
struct RenderGraphBlitFrameData;
struct RenderGraphBlitTemplatePass {
    RgTextureValue Output;
    RgTemplateSlot<RenderGraphBlitFrameData> Frame;
};
/// Declares only logical accesses and immutable blit constants in a cold template builder.
RenderGraphBlitTemplatePass DeclareRenderGraphBlit(RenderGraph& builder, RgTextureValue source,
                                                   RgTextureValue destination, bool decodeSrgb = false, bool encodeSrgb = false);
/// Frame data is retained by the instance; program/PSO/parameter preparation runs only if live.
bool BindRenderGraphBlit(const RenderGraphTemplateInstance& instance, const RenderGraphBlitTemplatePass& pass,
                         RenderSystem& system, render::RenderBackend backend);
/// A regular graph raster operation for scaling and display encoding. Sources remain
/// immutable values; the destination advances to the returned written version.
RgTextureValue AddRenderGraphBlit(RenderGraph& graph, RenderSystem& system, render::RenderBackend backend,
                                  RgTextureValue source, RgTextureValue destination, bool decodeSrgb = false, bool encodeSrgb = false);
}  // namespace radray
