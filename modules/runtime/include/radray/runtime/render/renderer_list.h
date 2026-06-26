#pragma once

#include <array>
#include <cstdint>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/render/sorting.h>
#include <radray/runtime/render/tag_set.h>
#include <radray/runtime/render/keyword_set.h>
#include <radray/runtime/render/scene_view.h>

namespace radray::srp {

class Renderer;
class Material;

/// 第一层意图谓词的数据形式(对应 Unity FilteringSettings)。
/// 回答"应不应该画":按渲染队列区间 + layerMask 过滤可见 renderer。
struct FilteringSettings {
    RenderQueueRange QueueRange{RenderQueueRange::All()};
    uint32_t LayerMask{0xFFFFFFFFu};

    /// 凭 renderer 暴露的语义(material 的队列、几何 layer)判定。
    bool Test(const Renderer& r) const noexcept;
};

/// 把 tag 列表 + sortFlags + per-view 常量交给引擎(对应 Unity CreateDrawingSettings)。
struct DrawingSettings {
    WantedLightModes ShaderTags;            ///< 按优先级的 LightMode 字符串列表
    SortingCriteria SortFlags{SortingCriteria::FrontToBack};
    KeywordSet PassKeywords;                ///< multi_compile 轴(管线驱动)
    render::DescriptorSet* ViewConstants{nullptr};  ///< space0(per-view),pass 自建自填
    render::DescriptorSetIndex ViewConstantsIndex{0};
};

/// 一条已解析的 per-frequency 绑定:set 索引 + descriptor set 句柄。
struct BoundDescriptorSet {
    render::DescriptorSetIndex Set{};
    render::DescriptorSet* Handle{nullptr};
};

/// 自包含、可直接录制的绘制命令。等价 UE5 FMeshDrawCommand:
/// 几何 + PSO + RootSignature + per-frequency 绑定 + per-object 字节,全部解析完毕。
struct MeshDrawCommand {
    // —— 几何 ——
    render::VertexBufferView Vbv;
    render::IndexBufferView Ibv;
    uint32_t IndexCount{0};
    uint32_t FirstIndex{0};
    int32_t VertexOffset{0};

    // —— 管线状态 ——
    render::GraphicsPipelineState* Pso{nullptr};
    render::RootSignature* RootSig{nullptr};

    // —— per-object push constant(布局与内容由 pass.WritePerObject 决定)——
    render::BindingParameterId PushConstantId{};
    vector<byte> PushConstantData{};

    // —— per-frequency descriptor set(space0 view + space1 material)——
    static constexpr uint32_t MaxBoundSets = 2;
    std::array<BoundDescriptorSet, MaxBoundSets> DescriptorSets{};
    uint32_t DescriptorSetCount{0};

    // —— 排序键(高位 PSO 分组减少状态切换,低位深度)——
    uint64_t SortKey{0};
};

/// 解析 + 排序后的命令序列(对应 Unity RendererList)。
struct RendererList {
    vector<MeshDrawCommand> Commands;

    void Clear() noexcept { Commands.clear(); }
    bool Empty() const noexcept { return Commands.empty(); }
};

}  // namespace radray::srp
