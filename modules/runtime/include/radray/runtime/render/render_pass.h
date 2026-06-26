#pragma once

#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render/render_pass_event.h>
#include <radray/runtime/render/sorting.h>
#include <radray/runtime/render/per_object_data.h>
#include <radray/runtime/render/render_state.h>
#include <radray/runtime/render/renderer_list.h>
#include <radray/runtime/render/culling_results.h>

namespace radray::srp {

class RenderContext;

/// 一个 LightMode 的编排单元(对应 Unity ScriptableRenderPass / DrawObjectsPass)。
/// 退回它不可剥夺的核心:决定 LightMode、filter、render state、RT、space0、per-object 布局。
/// 它【完全不认识】MVP/相机/具体 shader —— 那些要么在它自建的 space0 里,要么由变体缓存兜底。
///
/// 设计依据:srp_runtime_architecture.md §6、srp_runtime_design.md §3。
class RenderPass {
public:
    virtual ~RenderPass() = default;

    // —— 排序与解析 ——
    virtual RenderPassEvent Event() const = 0;                 ///< pass 队列排序键
    virtual const WantedLightModes& ShaderTags() const = 0;    ///< 按优先级的 LightMode 列表
    virtual FilteringSettings Filtering() const = 0;           ///< 第一层意图谓词
    virtual SortingCriteria SortFlags() const { return SortingCriteria::FrontToBack; }
    virtual KeywordSet PassKeywords() const { return {}; }     ///< multi_compile 轴
    virtual PerObjectDataFlags RequiredPerObjectData() const { return PerObjectData::None; }

    // —— 渲染状态 + RT 意图 ——
    /// 本 pass 的合并状态。可读 material 语义(twoSided)细调,但权威归 pass。
    virtual MeshPassRenderState RenderState(const Material& material) const = 0;
    /// RT/DS 格式(供 PSO 与 BeginRenderPass)。
    virtual GpuRenderTargetFormats RTFormats() const = 0;

    // —— space0 / per-object ——
    /// per-view descriptor set(space0),pass 自建自填。无则返回 nullptr。
    virtual render::DescriptorSet* ViewSet(const SceneView& view) const = 0;
    virtual render::DescriptorSetIndex ViewSetIndex() const { return render::DescriptorSetIndex{0}; }

    /// per-object push-constant 的参数名(在 root signature 里查 id)。
    virtual std::string_view PerObjectParamName() const = 0;
    /// 单个 draw 的 per-object 字节数(WritePerObject 写入的上限)。
    virtual uint32_t PerObjectByteSize() const = 0;
    /// 写一个 draw 的 per-object 字节。dst 大小 = PerObjectByteSize()。布局与内容由 pass 决定。
    virtual void WritePerObject(std::span<byte> dst, const Renderer& renderer, const SceneView& view) const = 0;

    // —— 录制入口 ——
    /// 默认实现 = CreateRendererList(本 pass 的 tag/filter/state/rt) → DrawRendererList。
    /// 派生类一般无需覆写;特殊 pass(compute 预览等)可覆写。
    virtual void Execute(RenderContext& ctx, const SceneView& view, const CullingResults& cull);
};

}  // namespace radray::srp
