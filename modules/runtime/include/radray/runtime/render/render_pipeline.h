#pragma once

#include <functional>
#include <span>

#include <radray/types.h>
#include <radray/runtime/render/render_pass.h>
#include <radray/runtime/render/render_pipeline_executor.h>
#include <radray/runtime/render/render_context.h>
#include <radray/runtime/render/scene_view.h>
#include <radray/runtime/render/culling_results.h>

namespace radray::srp {

/// 渲染管线:相机循环的顶层编排(对应 Unity URP 的 `UniversalRenderPipeline`)。
///
/// 职责(srp_runtime_design.md §7):对每个相机(SceneView + 其 CullingResults),
/// 配置 pass 队列 → 交给 `RenderPipelineExecutor` 排序并执行。
///
/// 设计取舍:本框架最小化阶段,裁剪(Cull)与提交(Submit)的具体实现绑定到 Scene/Camera/
/// CommandBuffer 等设施(Phase 5 接入)。因此 `RenderPipeline` 本身不做裁剪、不做 Submit,
/// 而是接收 game 已构建好的 `CullingResults`(含 SceneView),并通过 `SetupPasses` 钩子让 game
/// 为该相机声明本帧 pass。这样保持编排层与场景/设备层解耦,Phase 5/6 只需提供钩子内容。
class RenderPipeline {
public:
    /// 为一个相机声明本帧需要的 pass(enqueue 到 executor)。
    /// 对应 URP 里 renderer 的 `EnqueuePass` 调用集合 + `RecordRenderGraph` 之前的 setup。
    using SetupPassesFn = std::function<void(RenderPipelineExecutor&, const SceneView&, const CullingResults&)>;

    RenderPipeline() = default;
    explicit RenderPipeline(SetupPassesFn setup) noexcept : _setup(std::move(setup)) {}

    void SetSetupPasses(SetupPassesFn setup) noexcept { _setup = std::move(setup); }

    /// 渲染一个相机(对应 RenderSingleCamera,URP.cs:910)。
    /// 调用方负责裁剪填充 cull、BeginRenderPass/SetEncoder、以及 EndRenderPass/Submit。
    void RenderSingleCamera(RenderContext& ctx, const CullingResults& cull) {
        _executor.Clear();
        if (_setup) {
            _setup(_executor, cull.View, cull);
        }
        _executor.Execute(ctx, cull.View, cull);
    }

    /// 渲染多个相机(对应 Render(context, cameras),URP.cs:570)。
    /// 不在此处 Submit:提交时机由调用方掌控(与 RHI 命令缓冲生命周期对齐)。
    void Render(RenderContext& ctx, std::span<const CullingResults> culls) {
        for (const CullingResults& cull : culls) {
            RenderSingleCamera(ctx, cull);
        }
    }

    RenderPipelineExecutor& Executor() noexcept { return _executor; }
    const RenderPipelineExecutor& Executor() const noexcept { return _executor; }

private:
    RenderPipelineExecutor _executor;
    SetupPassesFn _setup;
};

}  // namespace radray::srp
