#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/pipeline_state_cache.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/types.h>

namespace radray {

class DrawList;
struct DrawItem;

/// 逐物体绘制执行器 (对应 Unity 的 ScriptableRenderContext.DrawRenderers /
/// UE5 的 FMeshDrawCommand::SubmitDraw)。
///
/// 打通 DrawList -> 实际 RHI draw 的闭环:
///   对每个 DrawItem:
///     1. ResolveVariant  -> 编译好的变体 (VS/PS + binding layout)
///     2. 构造 GraphicsPipelineStateDescriptor (pass 固定状态 + 顶点布局) -> PSO 缓存
///     3. 分配 per-object cbuffer (CBufferArena), 写入 proxy 的 LocalToWorld
///     4. material.ApplyProperties  -> push/root constant
///     5. encoder: BindPSO / BindVB / BindIB / BindShaderParameters / DrawIndexed
///
/// 设计要点:
/// - 每个 draw item 分配独立的 ShaderParameterTable (BindShaderParameters 立即录制,
///   复用同一 table 会让后一次 SetResource 覆盖前一次已引用的描述符)。
/// - per-object / per-view 常量走真实 cbuffer (CBufferArena + SetResource CBuffer),
///   与 material property 走的 push constant 互不冲突。
/// - 帧内分配的 table / arena buffer 需存活到命令提交完成; 调用方在 SubmitAndWait 后
///   再调用 BeginFrame 回收。
class MeshPassExecutor {
public:
    /// per-object 常量布局 (对应 Unity 的 UnityPerDraw / UE5 的 FPrimitiveUniformShaderParameters 精简版)。
    struct PerObjectConstants {
        float ObjectToWorld[16];  // 行优先存储 (Eigen 默认列优先, 拷贝时转置以匹配 HLSL row_major)
    };

    MeshPassExecutor(
        render::Device* device,
        render::ShaderVariantCache* variantCache,
        render::GraphicsPipelineStateCache* psoCache,
        std::string perObjectCBufferName = "PerObject",
        uint32_t flightCount = 1) noexcept;

    MeshPassExecutor(const MeshPassExecutor&) = delete;
    MeshPassExecutor& operator=(const MeshPassExecutor&) = delete;

    /// 开始录制指定 flight 的一帧: 回收 *该 flight* 上一轮分配的 table / arena。
    /// 每个 flight 持有独立的 table / arena; 调用方保证同一 flightIndex 的上一轮命令
    /// 已随 fence 完成 (双缓冲下即 N-2 帧), 故此处回收不会触及仍在飞行中的其他 flight。
    void BeginFrame(uint32_t flightIndex = 0) noexcept;

    /// 设置一个 per-view 常量块 (可选)。传入的字节会在每次 draw 前写入名为
    /// viewCBufferName 的 cbuffer。传空 name 关闭该功能。
    void SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept;

    /// 把已排序的 DrawList 提交进一个已 BeginRenderPass 的图形编码器。
    /// 返回成功提交的 draw 数。
    uint32_t Execute(render::GraphicsCommandEncoder* encoder, const DrawList& list) noexcept;

    /// 提交单条 draw item。成功返回 true。
    bool SubmitItem(render::GraphicsCommandEncoder* encoder, const DrawItem& item) noexcept;

private:
    Nullable<render::GraphicsPipelineState*> ResolvePso(
        const DrawItem& item,
        const render::CompiledShaderVariant& variant) noexcept;

    // 每个 flight 独立持有 cbuffer arena + 帧内参数表, 避免在 GPU 仍读取上一帧描述符时
    // 就 Reset/clear 导致描述符堆槽位被覆盖 (D3D12 STATIC descriptor 校验错误)。
    struct FlightResources {
        render::CBufferArena Arena;
        vector<unique_ptr<render::ShaderParameterTable>> Tables;  // 帧内存活

        explicit FlightResources(render::Device* device) noexcept : Arena(device) {}
    };

    render::Device* _device{nullptr};
    render::ShaderVariantCache* _variantCache{nullptr};
    render::GraphicsPipelineStateCache* _psoCache{nullptr};
    std::string _perObjectName;
    std::string _viewName;
    vector<byte> _viewData;
    vector<FlightResources> _flights;
    uint32_t _currentFlight{0};
};

}  // namespace radray
