#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/pipeline_state_cache.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/runtime/render_framework/material_constant_binder.h>
#include <radray/types.h>

namespace radray {

class DrawList;
struct DrawItem;
struct MaterialRenderSnapshot;

namespace render {
class SamplerCache;
}  // namespace render

/// 逐物体绘制执行器 (对应 Unity 的 ScriptableRenderContext.DrawRenderers /
/// UE5 的 FMeshDrawCommand::SubmitDraw)。
///
/// 打通 DrawList -> 实际 RHI draw 的闭环:
///   对每个 DrawItem:
///     1. ResolveVariant  -> 编译好的变体 (VS/PS + binding layout)
///     2. 构造 GraphicsPipelineStateDescriptor (pass 固定状态 + 顶点布局) -> PSO 缓存
///     3. 分配 per-object cbuffer (CBufferArena), 写入 proxy 的 LocalToWorld
///     4. MaterialConstantBinder 按反射把材质常量字段打进所属 cbuffer 块 + 绑定纹理/采样器
///     5. encoder: BindPSO / BindVB / BindIB / BindShaderParameters / DrawIndexed
///
/// 设计要点:
/// - 每个 draw item 使用独立的 ShaderParameterTable storage (BindShaderParameters 立即录制,
///   帧内不能覆盖)。同一 flight 再次使用且 fence 完成后，table 按 binding layout 重置复用。
/// - per-object / per-view 常量走真实 cbuffer (CBufferArena + SetResource CBuffer);
///   材质常量由 MaterialConstantBinder 按块的绑定类型走 push constant 或 CBV, 系统块名被跳过。
/// - table / arena buffer 需存活到命令提交完成; 调用方仅在对应 flight fence 完成后
///   再调用 BeginFrame 重置复用。
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
        render::SamplerCache* samplerCache,
        std::string perObjectCBufferName = "PerObject",
        uint32_t flightCount = 1) noexcept;

    MeshPassExecutor(const MeshPassExecutor&) = delete;
    MeshPassExecutor& operator=(const MeshPassExecutor&) = delete;

    /// 开始录制指定 flight 的一帧: 重置 *该 flight* 上一轮使用的 table / arena。
    /// 每个 flight 持有独立的 table / arena; 调用方保证同一 flightIndex 的上一轮命令
    /// 已随 fence 完成 (双缓冲下即 N-2 帧), 故此处回收不会触及仍在飞行中的其他 flight。
    void BeginFrame(uint32_t flightIndex = 0) noexcept;

    /// 设置一个 per-view 常量块 (可选)。传入的字节在下一次需要它的 draw 前上传一次，
    /// 同一设置下的后续 draw 复用该 cbuffer。传空 name 关闭该功能。
    void SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept;

    /// 设置管线级 (per-pass) 的全局纹理 / 采样器。每次 draw 前按名字绑定 (名字未在 shader
    /// 声明时静默跳过)。用于阴影图等由管线而非材质提供的资源。传 nullptr 清除。
    /// 名字借用调用方存储, 需在 Execute 期间存活。
    void SetGlobalTexture(std::string_view name, render::TextureView* view) noexcept;
    void SetGlobalSampler(std::string_view name, render::Sampler* sampler) noexcept;

    /// 设置管线级 (per-pass) 的全局 shader keyword (对应 Unity 的 Shader.EnableKeyword)。
    /// 解析变体时并入每个材质自身的 keyword 集: 用于阴影等由管线按帧决定的编译期分支,
    /// 让无阴影帧编译进不含阴影采样的变体。名字借用调用方存储, 需在 Execute 期间存活。
    void EnableGlobalKeyword(std::string_view name) noexcept;
    void ClearGlobals() noexcept;

    /// 把已排序的 DrawList 提交进一个已 BeginRenderPass 的图形编码器。
    /// 返回成功提交的 draw 数。
    uint32_t Execute(render::GraphicsCommandEncoder* encoder, const DrawList& list) noexcept;

    /// 提交单条 draw item。成功返回 true。
    bool SubmitItem(render::GraphicsCommandEncoder* encoder, const DrawItem& item) noexcept;

private:
    Nullable<render::GraphicsPipelineState*> ResolvePso(
        const DrawItem& item,
        const render::CompiledShaderVariant& variant) noexcept;

    struct ParameterTableCache {
        render::ShaderBindingLayout* Layout{nullptr};
        vector<unique_ptr<render::ShaderParameterTable>> Tables;
        size_t Used{0};
    };

    struct ResolvedDrawState {
        weak_ptr<const MaterialRenderSnapshot> Material;
        uint32_t PassIndex{0};
        vector<string> GlobalKeywords;
        const render::CompiledShaderVariant* Variant{nullptr};
        render::GraphicsPipelineState* Pso{nullptr};
    };

    // 每个 flight 独立持有 cbuffer arena + 参数表高水位缓存，避免在 GPU 仍读取上一帧
    // descriptor 时就 Reset 导致描述符堆槽位被覆盖 (D3D12 STATIC descriptor 校验错误)。
    struct FlightResources {
        render::CBufferArena Arena;
        vector<ParameterTableCache> TableCaches;

        explicit FlightResources(render::Device* device) noexcept : Arena(device) {}
    };

    Nullable<render::ShaderParameterTable*> AcquireParameterTable(
        FlightResources& resources,
        render::ShaderBindingLayout* layout) noexcept;

    bool EnsureViewBinding(FlightResources& resources) noexcept;

    Nullable<const ResolvedDrawState*> ResolveDrawState(const DrawItem& item) noexcept;

    render::Device* _device{nullptr};
    render::ShaderVariantCache* _variantCache{nullptr};
    render::GraphicsPipelineStateCache* _psoCache{nullptr};
    render::SamplerCache* _samplerCache{nullptr};
    std::string _perObjectName;
    std::string _viewName;
    vector<byte> _viewData;
    render::BufferBindingDescriptor _viewBinding{};
    bool _viewBindingValid{false};
    // 管线级全局纹理 / 采样器 (per-pass, 非 per-material)。名字借用调用方存储。
    struct GlobalTexture {
        std::string Name;
        render::TextureView* View{nullptr};
    };
    struct GlobalSampler {
        std::string Name;
        render::Sampler* Sampler{nullptr};
    };
    vector<GlobalTexture> _globalTextures;
    vector<GlobalSampler> _globalSamplers;
    // 管线级全局 keyword (per-pass, 非 per-material)。名字借用调用方存储。
    vector<std::string_view> _globalKeywords;
    vector<ResolvedDrawState> _resolvedDrawStates;
    vector<FlightResources> _flights;
    uint32_t _currentFlight{0};
    // 材质常量打包器: 用变体反射把散字段打进所属 cbuffer 块, 整块提交 (push constant / CBV)。
    MaterialConstantBinder _constantBinder;
    // 复用缓冲: 每 draw 收集快照常量供打包器消费, 避免每次分配。
    vector<MaterialConstantValue> _constantScratch;
    // 复用缓冲: ResolvePso 里叠加材质 blend 覆盖时, 拷贝 pass 的 ColorTargets 再改, 避免每次分配。
    vector<render::ColorTargetState> _colorTargetScratch;
};

}  // namespace radray
