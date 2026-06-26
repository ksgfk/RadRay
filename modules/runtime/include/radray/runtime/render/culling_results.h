#pragma once

#include <span>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/render/scene_view.h>
#include <radray/runtime/render/renderer.h>

namespace radray::srp {

/// 一次相机裁剪的产物(对应 Unity CullingResults / UE5 InitViews 可见集)。
/// 一帧一次,所有 pass 共享。最小化:不做视锥裁剪,收集全部可见 renderer。
struct CullingResults {
    vector<const Renderer*> Renderers;
    SceneView View;

    void Clear() noexcept { Renderers.clear(); }
};

}  // namespace radray::srp
