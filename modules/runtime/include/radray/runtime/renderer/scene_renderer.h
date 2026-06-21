#pragma once

#include <array>
#include <functional>

#include <radray/basic_math.h>
#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/gpu_system.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

class Scene;

/// 渲染一帧所用的视图参数。RadRay 版的 UE5 FSceneView(最小化)。
/// 当前仅承载相机矩阵与视口尺寸;SceneRenderer 负责填充并驱动。
struct SceneView {
    Eigen::Matrix4f ViewMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ProjMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ViewProjMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Vector3f EyePosition{Eigen::Vector3f::Zero()};
    uint32_t ViewportWidth{0};
    uint32_t ViewportHeight{0};
};

/// 一条已解析的 per-material 绑定:set 索引 + descriptor set 句柄。
/// 对应 UE5 FMeshDrawShaderBindings 的最小子集(当前仅 per-material 一个 set)。
struct BoundDescriptorSet {
    render::DescriptorSetIndex Set{};
    render::DescriptorSet* Handle{nullptr};
};

/// 自包含、可直接录制的绘制命令。对应 UE5 的 FMeshDrawCommand:
/// 几何 + PSO + RootSignature + per-draw 常量,全部解析完毕,录制时只需机械绑定。
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

    // —— per-draw push constant ——
    // 当前最小化:单个 push-constant 槽位 + 一段 CPU 端常量数据(命令自持,录制时上传)。
    render::BindingParameterId PushConstantId{};
    vector<byte> PushConstantData{};

    // —— per-material descriptor set ——
    // 已构建好的频率绑定(set1=per-material)。录制时 BindDescriptorSet。
    // 当前最多 1 个;用定长内联存储避免每命令堆分配。
    static constexpr uint32_t MaxBoundSets = 2;
    std::array<BoundDescriptorSet, MaxBoundSets> DescriptorSets{};
    uint32_t DescriptorSetCount{0};

    // —— 排序键 ——
    // 对应 UE5 FMeshDrawCommandSortKey:高位 PSO 分组(减少状态切换),低位深度。
    uint64_t SortKey{0};
};

/// 框架标准的 per-object 常量。对应 UE5 FPrimitiveUniformShaderParameters 的最小子集。
/// 布局与 shader 端 push-constant 约定一致。
struct ObjectConstants {
    float MVP[16];
    float Model[16];
    float CameraPosition[4];
    uint32_t Debug[4];
};

/// 一个 View 的可见图元列表。对应 UE5 InitViews 产出的 FViewInfo 可见集(最小化)。
/// 当前不做视锥裁剪:收集场景全部可渲染代理。保留此接缝便于后续接入裁剪/relevance。
struct VisiblePrimitiveList {
    vector<const PrimitiveSceneProxy*> Primitives;

    void Clear() noexcept { Primitives.clear(); }
};

/// 图元过滤谓词。对应 Unity 的 FilteringSettings:每个 pass 据此从【共享】可见集里挑
/// 自己要画的子集。返回 true = 保留。空谓词 = 全画。
/// 用谓词而非固定 enum:把"按什么过滤"的决定权留给 pass,runtime 不预设分类标准。
using PrimitiveFilter = std::function<bool(const PrimitiveSceneProxy&)>;

/// 网格 pass 处理器。对应 UE5 的 FMeshPassProcessor:
/// 输入一份可见图元列表 + 一个 View,输出一组完全解析、可排序的 MeshDrawCommand。
/// 负责:取代理几何 → 取材质 → 经 PSOCache 取 PSO → 填 per-object 常量 → 算 SortKey。
///
/// 当前为单一 BasePass 用途;后续 DepthPrePass/ShadowPass 各自派生/配置一个处理器。
class MeshPassProcessor {
public:
    struct Config {
        PSOCache* Cache{nullptr};
        PSOCache::RenderTargetFormats RtFormats{};
        /// 材质 push-constant 槽位名(填 ObjectConstants 的目标)。
        std::string_view ObjectConstantsParam{"gScene"};
        /// 用于懒构建 MaterialRenderProxy 的 GPU 系统。为空时跳过 per-material 绑定。
        GpuSystem* Gpu{nullptr};
        /// Optional per-view descriptor set, e.g. space0 light buffers.
        render::DescriptorSet* ViewDescriptorSet{nullptr};
        render::DescriptorSetIndex ViewDescriptorSetIndex{0};
        PSOCache::GraphicsPipelineOverride PipelineOverride{};
    };

    explicit MeshPassProcessor(const Config& config) noexcept : _config(config) {}

    /// 遍历可见图元列表,构建 MeshDrawCommand 列表(追加到 out)。
    /// filter 非空时,只为通过过滤的图元产出命令(对应 Unity DrawRenderers 的 FilteringSettings)。
    void BuildCommands(
        const VisiblePrimitiveList& visible,
        const SceneView& view,
        vector<MeshDrawCommand>& out,
        const PrimitiveFilter& filter = {}) const;

    /// 按 SortKey 升序排序(PSO 分组在前以减少状态切换)。
    static void SortCommands(vector<MeshDrawCommand>& commands);

private:
    Config _config;
};

/// 场景渲染器。对应 UE5 FSceneRenderer / Unity 的 cull + DrawRenderers(最小化)。
///
/// 职责拆成两个解耦的动作,对齐 Unity 模型:
///  - Cull:从 Scene 收集本 View 的可见图元,产出一份【共享】可见集。一帧一次,各 pass 共享。
///  - DrawRenderers:从外部传入的可见集里(按 filter)挑子集,构建/排序/录制 MeshDrawCommand。
///
/// RenderPass 的 Begin/End、depth buffer、barrier、视口(含后端差异)由调用方(pass)管理,
/// 使本类与具体窗口/帧资源解耦。
class SceneRenderer {
public:
    SceneRenderer() noexcept = default;

    /// Cull 接缝(对应 Unity context.Cull / UE5 InitViews)。
    /// 当前【不做视锥裁剪】:收集场景全部可渲染代理到 out。view 预留给后续裁剪/relevance。
    /// 无内部状态,故为静态:调用方持有 out 存储,一帧 cull 一次填 RenderContext.Visible。
    static void Cull(const Scene& scene, const SceneView& view, VisiblePrimitiveList& out);

    /// 从【共享】可见集挑子集并录制(对应 Unity context.DrawRenderers)。
    /// encoder 须为已 Begin 的 RenderPass;visible 通常来自 RenderContext.Visible;
    /// filter 空 = 全画,非空 = 只画通过过滤的图元。
    void DrawRenderers(
        render::GraphicsCommandEncoder* encoder,
        const VisiblePrimitiveList& visible,
        const SceneView& view,
        const MeshPassProcessor::Config& processorConfig,
        const PrimitiveFilter& filter = {});

private:
    // 复用的命令缓冲,避免每帧重新分配。
    vector<MeshDrawCommand> _drawCommands;
};

}  // namespace radray
