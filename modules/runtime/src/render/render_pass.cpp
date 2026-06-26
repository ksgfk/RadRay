#include <radray/runtime/render/render_pass.h>

#include <radray/runtime/render/render_context.h>
#include <radray/runtime/render/material.h>

namespace radray::srp {

void RenderPass::Execute(RenderContext& ctx, const SceneView& view, const CullingResults& cull) {
    // (1) DrawingSettings:把 tag 列表 + sortFlags + space0 交给引擎。
    DrawingSettings draw{
        .ShaderTags = ShaderTags(),
        .SortFlags = SortFlags(),
        .PassKeywords = PassKeywords(),
        .ViewConstants = ViewSet(view),
        .ViewConstantsIndex = ViewSetIndex()};

    // (2) RendererList:filter + relevance + variant/PSO 解析 + 排序,一步到位。
    //     RenderState 需要一个 material 来细调;CreateRendererList 内部对每个 renderer 各取其 material,
    //     这里仅传 RTFormats。RenderState 由 CreateRendererList 调 pass.RenderState(mat) 解析。
    RendererList list = ctx.CreateRendererList(cull, draw, Filtering(), RTFormats(), *this);

    // (3) 录制。
    ctx.DrawRendererList(draw, list);
}

}  // namespace radray::srp
