#pragma once

#include <radray/types.h>
#include <radray/runtime/renderer/render_pass.h>

namespace radray {

/// 渲染管线:持有一组 RenderPass,按插入顺序执行,组织成一帧渲染流程。
/// 对应 Unity RenderPipeline / UE5 FSceneRenderer 的"编排"职责(最小化)。
///
/// 设计(最小化):
/// - 空容器:new 出来不含任何 pass。runtime 不做任何默认装填,也不强加渲染策略。
/// - 默认 Render 按插入顺序逐个 Execute。要自定义编排(排序 / 资源准备 / 分组提交)
///   时派生并重写 Render——这就是"组织"职责将来生长的归属点。
/// - 是否使用管线、装填哪些 pass、相机/上下文怎么来,全在调用方(game 层)手里。
///
/// 实现细节藏在 render_pipeline.cpp:头文件只暴露接口。
class RenderPipeline {
public:
    RenderPipeline() noexcept = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;
    virtual ~RenderPipeline() noexcept;

    /// 追加一个 pass。所有权移交管线,按追加顺序执行。nullptr 被忽略。
    void AddPass(unique_ptr<RenderPass> pass);

    /// 已装填的 pass 数量。
    size_t GetPassCount() const noexcept;

    /// 执行整条管线。默认按插入顺序逐个 Execute。
    virtual void Render(RenderContext& ctx);

protected:
    vector<unique_ptr<RenderPass>> _passes;
};

}  // namespace radray
