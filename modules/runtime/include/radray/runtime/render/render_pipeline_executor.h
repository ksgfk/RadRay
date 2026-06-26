#pragma once

#include <radray/types.h>
#include <radray/runtime/render/render_pass.h>
#include <radray/runtime/render/scene_view.h>
#include <radray/runtime/render/culling_results.h>

namespace radray::srp {

class RenderContext;

/// pass 队列的执行器(对应 Unity URP 的 `ScriptableRenderer`)。
///
/// 职责极小且明确(srp_runtime_design.md §7):
/// 1. 持有一个 `RenderPass*` 队列(对应 `m_ActiveRenderPassQueue`,Renderer.cs:482)。
/// 2. `Execute` 时按 `RenderPassEvent` 做稳定插入排序(对应 SortStable,Renderer.cs:1254),
///    相同 event 保持入队顺序。
/// 3. 依次调用每个 pass 的 `Execute`(Renderer.cs:1004)。
/// 4. 执行完清空队列(Renderer.cs:1218)。
///
/// 不拥有 pass 的生命周期(借用指针),pass 由 pipeline / game 持有。
class RenderPipelineExecutor {
public:
    /// 入队一个 pass(对应 EnqueuePass,Renderer.cs:1038)。借用,不拥有。
    void EnqueuePass(RenderPass* pass) {
        if (pass != nullptr) {
            _queue.push_back(pass);
        }
    }

    /// 清空队列而不执行。
    void Clear() noexcept { _queue.clear(); }

    bool Empty() const noexcept { return _queue.empty(); }
    size_t Count() const noexcept { return _queue.size(); }

    /// 稳定插入排序:按 `RenderPassEvent` 升序,相同 event 保持入队顺序。
    /// 对应 Unity `ScriptableRenderer.SortStable`(Renderer.cs:1254)。
    static void SortStable(vector<RenderPass*>& queue) noexcept {
        for (size_t i = 1; i < queue.size(); ++i) {
            RenderPass* cur = queue[i];
            const auto curEvent = static_cast<int32_t>(cur->Event());
            size_t j = i;
            while (j > 0 && static_cast<int32_t>(queue[j - 1]->Event()) > curEvent) {
                queue[j] = queue[j - 1];
                --j;
            }
            queue[j] = cur;
        }
    }

    /// 排序并依次执行所有 pass,然后清空队列。
    /// 对应 Unity `ScriptableRenderer.Execute`(Renderer.cs:953/1004/1218)。
    void Execute(RenderContext& ctx, const SceneView& view, const CullingResults& cull) {
        SortStable(_queue);
        for (RenderPass* pass : _queue) {
            pass->Execute(ctx, view, cull);
        }
        _queue.clear();
    }

    /// 只读访问当前队列(测试用)。
    const vector<RenderPass*>& Queue() const noexcept { return _queue; }

private:
    vector<RenderPass*> _queue;  ///< 对应 m_ActiveRenderPassQueue
};

}  // namespace radray::srp
