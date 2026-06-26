#pragma once

#include <radray/runtime/render/scene.h>
#include <radray/runtime/render/renderer.h>
#include <radray/runtime/render/scene_view.h>
#include <radray/runtime/render/culling_results.h>

namespace radray::srp {

/// 最小裁剪:遍历 Scene 的全部 Renderer,收集可见者(IsVisible())进 CullingResults。
/// 对应 Unity ScriptableRenderContext.Cull / UE5 InitViews 的可见集构建,但不做视锥/遮挡裁剪。
/// 一帧一相机调用一次,产物供该相机所有 pass 共享。
inline CullingResults CullAll(const Scene& scene, const SceneView& view) {
    CullingResults results;
    results.View = view;
    auto renderers = scene.Renderers();
    results.Renderers.reserve(renderers.size());
    for (Renderer* renderer : renderers) {
        if (renderer != nullptr && renderer->IsVisible()) {
            results.Renderers.push_back(renderer);
        }
    }
    return results;
}

}  // namespace radray::srp
