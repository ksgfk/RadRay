#pragma once

#include <span>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render/shader_variant_cache.h>
#include <radray/runtime/render/renderer_list.h>
#include <radray/runtime/render/render_state.h>
#include <radray/runtime/render/culling_results.h>

namespace radray::srp {

class RenderPass;

/// 渲染上下文:本框架的"引擎机器",game 不可见其内部循环。
/// 持有三级缓存(ShaderVariantCache + 复用 GpuSystem 的 RS/PSO 缓存)与当前帧录制句柄。
/// 提供 CreateRendererList(filter→relevance→variant→PSO→sort)与 RecordRendererList。
///
/// 不拥有 GpuSystem(借用)。一帧内 encoder 由 pipeline 在 BeginRenderPass 后 SetEncoder 注入。
/// 设计依据:srp_runtime_design.md §4。
class RenderContext {
public:
    RenderContext(GpuSystem* gpu, ShaderVariantCache* variantCache) noexcept
        : _gpu(gpu), _variantCache(variantCache) {}
    RenderContext(const RenderContext&) = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    GpuSystem* Gpu() const noexcept { return _gpu; }
    ShaderVariantCache* VariantCache() const noexcept { return _variantCache; }

    /// 当前 pass 的录制目标。由 pipeline 在 BeginRenderPass 后注入,EndRenderPass 前清空。
    void SetEncoder(render::GraphicsCommandEncoder* enc) noexcept { _encoder = enc; }
    render::GraphicsCommandEncoder* Encoder() const noexcept { return _encoder; }

    /// 构建一个 RendererList:filter(意图)→ resolveTag(relevance)→ 查变体/PSO → 生成命令 → 按 sortFlags 排序。
    /// pass 提供 RenderState、RT 格式、per-object writer 与 PerObjectData flag。
    /// 对应 Unity CreateRendererListWithRenderStateBlock。
    RendererList CreateRendererList(
        const CullingResults& cull,
        const DrawingSettings& draw,
        const FilteringSettings& filter,
        const GpuRenderTargetFormats& rtFormats,
        const RenderPass& pass);

    /// 录制一个 RendererList:绑 space0(每 pass 一次)+ 逐命令绑 PSO/RootSig/space1 + push per-object + draw。
    /// 对应 Unity cmd.DrawRendererList。
    void DrawRendererList(const DrawingSettings& draw, const RendererList& list);

private:
    GpuSystem* _gpu;
    ShaderVariantCache* _variantCache;
    render::GraphicsCommandEncoder* _encoder{nullptr};
};

}  // namespace radray::srp
