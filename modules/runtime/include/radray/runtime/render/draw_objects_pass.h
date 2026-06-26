#pragma once

#include <functional>
#include <span>
#include <string>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/render/render_pass.h>

namespace radray::srp {

/// 通用、可直接配置的具体绘制 pass(对应 Unity URP 的 `DrawObjectsPass`)。
///
/// URP 里 BasePass/Transparent 都是 `new DrawObjectsPass(...)` —— 同一个具体类,
/// 靠构造参数区分。RadRay 照此设计:排序/过滤/tag/渲染状态/RT 这些"纯配置"用 `Desc` 字段表达;
/// space0(per-view)与 per-object 字节这些"必须由 game 提供具体内容"的钩子,用 `std::function` 注入。
///
/// 这样 game 无需为每个 pass 派生一个类:配齐 `Desc` 即可得到一个可入队的 `RenderPass`。
/// 设计依据:srp_runtime_design.md §3 / §4。
class DrawObjectsPass final : public RenderPass {
public:
    /// 提供本 pass 的合并渲染状态。可读 material 语义(twoSided 等)细调,权威归 pass。
    using RenderStateProvider = std::function<MeshPassRenderState(const Material&)>;
    /// 提供本 pass 的 per-view descriptor set(space0)。无则返回 nullptr。
    using ViewSetProvider = std::function<render::DescriptorSet*(const SceneView&)>;
    /// 写单个 draw 的 per-object 字节。dst 大小 = Desc::PerObjectByteSize。
    using PerObjectWriter = std::function<void(std::span<byte>, const Renderer&, const SceneView&)>;

    struct Desc {
        // —— 纯配置(对应 DrawObjectsPass 成员)——
        RenderPassEvent Event{RenderPassEvent::BeforeRenderingOpaques};
        WantedLightModes ShaderTags{};                  ///< 按优先级的 LightMode 列表
        FilteringSettings Filtering{};                   ///< 第一层意图谓词
        SortingCriteria SortFlags{SortingCriteria::FrontToBack};
        KeywordSet PassKeywords{};                       ///< multi_compile 轴
        PerObjectDataFlags RequiredPerObjectData{PerObjectData::None};
        GpuRenderTargetFormats RTFormats{};              ///< RT/DS 格式(供 PSO 与 BeginRenderPass)

        // —— 渲染状态:固定预设 或 按 material 解析(二选一,Provider 优先)——
        MeshPassRenderState RenderState{MeshPassRenderState::Opaque()};
        RenderStateProvider RenderStateFn{};

        // —— space0 / per-object 钩子(game 提供具体内容)——
        ViewSetProvider ViewSetFn{};
        render::DescriptorSetIndex ViewSetIndex{0};
        radray::string PerObjectParamName{};
        uint32_t PerObjectByteSize{0};
        PerObjectWriter PerObjectFn{};
    };

    explicit DrawObjectsPass(Desc desc) noexcept : _desc(std::move(desc)) {}

    // —— 排序与解析 ——
    RenderPassEvent Event() const override { return _desc.Event; }
    const WantedLightModes& ShaderTags() const override { return _desc.ShaderTags; }
    FilteringSettings Filtering() const override { return _desc.Filtering; }
    SortingCriteria SortFlags() const override { return _desc.SortFlags; }
    KeywordSet PassKeywords() const override { return _desc.PassKeywords; }
    PerObjectDataFlags RequiredPerObjectData() const override { return _desc.RequiredPerObjectData; }

    // —— 渲染状态 + RT 意图 ——
    MeshPassRenderState RenderState(const Material& material) const override {
        return _desc.RenderStateFn ? _desc.RenderStateFn(material) : _desc.RenderState;
    }
    GpuRenderTargetFormats RTFormats() const override { return _desc.RTFormats; }

    // —— space0 / per-object ——
    render::DescriptorSet* ViewSet(const SceneView& view) const override {
        return _desc.ViewSetFn ? _desc.ViewSetFn(view) : nullptr;
    }
    render::DescriptorSetIndex ViewSetIndex() const override { return _desc.ViewSetIndex; }
    std::string_view PerObjectParamName() const override { return _desc.PerObjectParamName; }
    uint32_t PerObjectByteSize() const override { return _desc.PerObjectByteSize; }
    void WritePerObject(std::span<byte> dst, const Renderer& renderer, const SceneView& view) const override {
        if (_desc.PerObjectFn) {
            _desc.PerObjectFn(dst, renderer, view);
        }
    }

    const Desc& Descriptor() const noexcept { return _desc; }

private:
    Desc _desc;
};

}  // namespace radray::srp
