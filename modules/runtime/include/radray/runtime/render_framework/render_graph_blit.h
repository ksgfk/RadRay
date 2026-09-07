#pragma once
#include <radray/runtime/render_framework/render_graph.h>

namespace radray {
class RenderSystem;
/// A regular graph raster operation for scaling and display encoding. Sources remain
/// immutable values; the destination advances to the returned written version.
RgTextureValue AddRenderGraphBlit(RenderGraph& graph, RenderSystem& system, render::RenderBackend backend,
                                  RgTextureValue source, RgTextureValue destination, bool decodeSrgb = false, bool encodeSrgb = false);
}  // namespace radray
