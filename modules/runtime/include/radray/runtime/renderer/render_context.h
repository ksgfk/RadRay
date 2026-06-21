#pragma once

#include <cstdint>

#include <radray/render/common.h>
#include <radray/runtime/renderer/scene_renderer.h>

namespace radray {

class Scene;
class GpuSystem;
class RenderResourcePool;

/// 一次 RenderPipeline::Render 的全部输入,贯穿所有 RenderPass。
///
/// 由调用方(game 层)从 AppFrameContext + AppFrameTarget + 相机组装,使 RenderPass
/// 不必依赖 AppFrameContext 这类帧录制类型——RenderPass 只看见一份纯数据上下文。
///
/// 设计(最小化):
/// - 承载"画这一帧/这个 view 需要的最小数据":帧资源 + 视图 + 共享可见集 + 共享资源池 + 输出目标。
/// - 可见集(Visible)由调用方 cull 一次填入,各 pass 共享;具体过滤由 pass 自定。
/// - 跨 pass 共享的中间资源(如 depth)走 Resources 池:按名字交接,barrier 由池跟踪发出。
/// - ColorTarget 由调用方保证已 barrier 到 RenderTarget;backbuffer 不进池,仍由 app 持有。
struct RenderContext {
    // —— 帧资源 ——
    uint32_t FlightIndex{0};
    render::CommandBuffer* CmdBuffer{nullptr};
    render::Device* Device{nullptr};
    GpuSystem* Gpu{nullptr};

    // —— 视图(相机产出)——
    const Scene* Scene{nullptr};
    SceneView View{};
    render::DescriptorSet* ViewDescriptorSet{nullptr};
    render::DescriptorSetIndex ViewDescriptorSetIndex{0};

    // —— 可见集(cull 一次的共享产物,对应 Unity RenderingData.cullResults)——
    // 由调用方 cull 一次填入,贯穿所有 pass;每个 pass 据此 + 自己的过滤条件挑子集。
    const VisiblePrimitiveList* Visible{nullptr};

    // —— 跨 pass 共享的瞬态资源池 ——
    // producer pass 用 Acquire 产出、consumer pass 用 Find 接力;状态翻转的 barrier 由池跟踪发出。
    RenderResourcePool* Resources{nullptr};

    // —— 输出(backbuffer)——
    render::TextureView* ColorTarget{nullptr};
    render::TextureFormat ColorFormat{render::TextureFormat::UNKNOWN};
    uint32_t Width{0};
    uint32_t Height{0};
};

}  // namespace radray
