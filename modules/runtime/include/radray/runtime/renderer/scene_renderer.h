#pragma once

#include <radray/basic_math.h>
#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/runtime/render_system.h>
#include <radray/runtime/renderer/primitive_scene_proxy.h>

namespace radray {

class Scene;

/// 渲染一帧所用的视图参数。RadRay 版的 UE5 FSceneView(最小化)。
/// 当前仅承载相机矩阵与视口尺寸;SceneRenderer 负责填充并驱动。
struct SceneView {
    Eigen::Matrix4f ViewMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ProjMatrix{Eigen::Matrix4f::Identity()};
    Eigen::Matrix4f ViewProjMatrix{Eigen::Matrix4f::Identity()};
    uint32_t ViewportWidth{0};
    uint32_t ViewportHeight{0};
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

    // —— 排序键 ——
    // 对应 UE5 FMeshDrawCommandSortKey:高位 PSO 分组(减少状态切换),低位深度。
    uint64_t SortKey{0};
};

/// 框架标准的 per-object 常量。对应 UE5 FPrimitiveUniformShaderParameters 的最小子集。
/// 布局与 shader 端 push-constant 约定一致:两个列主序 float4x4(128 字节)。
struct ObjectConstants {
    float MVP[16];
    float Model[16];
};

/// 一个 View 的可见图元列表。对应 UE5 InitViews 产出的 FViewInfo 可见集(最小化)。
/// 当前不做视锥裁剪:收集场景全部可渲染代理。保留此接缝便于后续接入裁剪/relevance。
struct VisiblePrimitiveList {
    vector<const PrimitiveSceneProxy*> Primitives;

    void Clear() noexcept { Primitives.clear(); }
};

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
    };

    explicit MeshPassProcessor(const Config& config) noexcept : _config(config) {}

    /// 遍历可见图元列表,构建 MeshDrawCommand 列表(追加到 out)。
    void BuildCommands(const VisiblePrimitiveList& visible, const SceneView& view, vector<MeshDrawCommand>& out) const;

    /// 按 SortKey 升序排序(PSO 分组在前以减少状态切换)。
    static void SortCommands(vector<MeshDrawCommand>& commands);

private:
    Config _config;
};

/// 场景渲染编排器。对应 UE5 的 FSceneRenderer(最小化:InitViews → 单一 BasePass)。
///
/// 一帧流程(对应 UE5 FSceneRenderer::Render 的开头两段接缝):
///  1. InitViews   ← 从 Scene 收集本 View 的可见图元(当前不裁剪,全收集)。
///  2. BasePass    ← MeshPassProcessor 基于可见列表构建并排序 MeshDrawCommand,顺序录制。
///
/// RenderPass 的 Begin/End、depth buffer、barrier、视口(含后端差异)由调用方(应用)
/// 管理,使本类与具体窗口/帧资源解耦。后续扩展多 View/多 pass 时在此追加循环与处理器。
class SceneRenderer {
public:
    SceneRenderer() noexcept = default;

    /// 帧顶资源准备:遍历场景全部代理,对生命周期处于 Pending 的代理调用
    /// UpdateResources,让其经 ResourceUploader 上传 GPU 资源并推进到 Ready。
    /// 【须在 RenderPass 之前、以裸 CommandBuffer 调用】:buffer copy 上传不能在 render pass 内录制。
    /// 数据驱动替代插命令队列:组件只设网格,渲染系统从 Proxy 读数据、处理上传,
    /// 就绪后才参与绘制。对应 UE5 渲染线程驱动的 FRenderResource 初始化。
    void PrepareResources(
        render::CommandBuffer* cmdBuffer,
        const Scene& scene,
        ResourceUploader& uploader);

    /// 渲染一个 View 的场景。内部 = InitViews(收集可见图元)→ BasePass(构建/排序/录制)。
    /// encoder 须为已 Begin 的 RenderPass;processorConfig 提供 PSOCache 与 RT 格式;
    /// view 由相机产出(View/Proj/ViewProj + 视口)。
    void Render(
        render::GraphicsCommandEncoder* encoder,
        const Scene& scene,
        const SceneView& view,
        const MeshPassProcessor::Config& processorConfig);

private:
    /// InitViews 接缝:收集场景全部可渲染代理到 _visible(不做视锥裁剪)。
    void InitViews(const Scene& scene, const SceneView& view);

    /// 在已打开的 encoder 上录制 base pass(基于 _visible)。
    void RenderBasePass(
        render::GraphicsCommandEncoder* encoder,
        const SceneView& view,
        const MeshPassProcessor::Config& processorConfig);

    // 本帧可见图元(InitViews 产出)。复用以避免每帧分配。
    VisiblePrimitiveList _visible;
    // 复用的命令缓冲,避免每帧重新分配。
    vector<MeshDrawCommand> _drawCommands;
};

}  // namespace radray
