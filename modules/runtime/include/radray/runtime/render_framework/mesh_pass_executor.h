#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include <radray/render/common.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/pipeline_state_cache.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/runtime/render_framework/material_constant_binder.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/types.h>

namespace radray {

class DrawList;
class PrimitiveSceneProxy;
struct DrawItem;
struct MaterialRenderSnapshot;

struct MeshDrawCommandTemplate {
    PrimitiveSceneProxy* Proxy{nullptr};
    uint64_t ProxyGeneration{0};
    uint32_t SectionIndex{0};
    uint32_t PassIndex{0};
    ShaderVariantKey VariantKey{};
    MaterialBindingKey MaterialKey{};
    render::GraphicsPipelineState* Pipeline{nullptr};
    render::PipelineLayout* Layout{nullptr};
    render::BindingGroup* MaterialGroup{nullptr};
    GpuMesh::DrawData Geometry{};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint64_t PipelineId{0};
    uint64_t MaterialBindingId{0};
    uint64_t GeometryPageId{0};
    uint64_t LastUsedFrame{0};
};

struct MeshDrawCommand {
    const MeshDrawCommandTemplate* Template{nullptr};
    render::BindingGroup* PerObjectGroup{nullptr};
    render::BindingGroup* ViewGroup{nullptr};
    uint32_t PerObjectDynamicOffset{0};
    uint32_t ViewDynamicOffset{0};
    uint64_t PerObjectBufferPage{0};
    int32_t RenderQueue{0};
    float ViewDistance{0.0f};
};

/// 逐物体绘制执行器 (对应 Unity 的 ScriptableRenderContext.DrawRenderers /
/// UE5 的 FMeshDrawCommand::SubmitDraw)。
///
/// 打通 DrawList -> 实际 RHI draw 的闭环:
///   对每个 DrawItem:
///     1. ResolveVariant  -> 编译好的变体 (VS/PS + binding layout)
///     2. 构造 GraphicsPipelineStateDescriptor (pass 固定状态 + 顶点布局) -> PSO 缓存
///     3. 分配 per-object cbuffer (CBufferArena), 写入 proxy 的 LocalToWorld
///     4. MaterialConstantBinder 按反射把材质常量字段打进所属 cbuffer 块 + 绑定纹理/采样器
///     5. encoder: BindPSO / BindVB / BindIB / BindBindingGroup / DrawIndexed
///
/// 设计要点:
/// - system BindingGroup 按 flight/layout 复用，材质 BindingGroup 按不可变快照版本复用。
/// - per-object / per-view 常量使用 dynamic cbuffer offset；材质常量使用持久 cbuffer slice。
/// - FrameResources 统一持有 arena、descriptor pool 与 retire list，并在 flight fence 完成后重置。
class MeshPassExecutor {
public:
    /// per-object 常量布局 (对应 Unity 的 UnityPerDraw / UE5 的 FPrimitiveUniformShaderParameters 精简版)。
    struct PerObjectConstants {
        float ObjectToWorld[16];  // 行优先存储 (Eigen 默认列优先, 拷贝时转置以匹配 HLSL row_major)
    };
    static_assert(sizeof(PerObjectConstants) == 64);

    MeshPassExecutor(
        render::Device* device,
        ShaderVariantLibrary* variantCache,
        GraphicsPipelineStateLibrary* psoCache,
        SamplerCache* samplerCache,
        std::string perObjectCBufferName = "PerObject") noexcept;

    MeshPassExecutor(const MeshPassExecutor&) = delete;
    MeshPassExecutor& operator=(const MeshPassExecutor&) = delete;

    /// 设置一个 per-view 常量块 (可选)。传入的字节在下一次需要它的 draw 前上传一次，
    /// 同一设置下的后续 draw 复用该 cbuffer。传空 name 关闭该功能。
    void SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept;

    void SetRenderPass(render::RenderPass* renderPass) noexcept {
        _renderPass = renderPass;
        _recorder = {};
    }

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
    /// instanceCount 应用于列表中的每条 indexed draw；返回成功提交的 API draw 数。
    uint32_t Execute(
        render::GraphicsCommandEncoder* encoder,
        const DrawList& list,
        FrameResources& resources,
        uint32_t instanceCount = 1) noexcept;

    /// 提交单条 draw item。成功返回 true。
    bool SubmitItem(
        render::GraphicsCommandEncoder* encoder,
        const DrawItem& item,
        FrameResources& resources) noexcept;

private:
    Nullable<render::GraphicsPipelineState*> ResolvePso(
        const DrawItem& item,
        const CompiledShaderVariant& variant) noexcept;

    struct MaterialBinding {
        MaterialBindingKey Key{};
        shared_ptr<const MaterialRenderSnapshot> Snapshot{};
        render::PipelineLayout* Layout{nullptr};
        unique_ptr<render::BindingGroup> Group{};
        vector<MaterialConstantPool::Allocation> ConstantAllocations;
        uint64_t LastUsedFrame{0};
    };

    struct ResolvedDrawState {
        MaterialBindingKey MaterialKey{};
        uint32_t PassIndex{0};
        vector<string> GlobalKeywords;
        render::RenderPass* RenderPass{nullptr};
        const CompiledShaderVariant* Variant{nullptr};
        render::GraphicsPipelineState* Pso{nullptr};
        uint64_t LastUsedFrame{0};
    };

    Nullable<render::BindingGroup*> AcquireSystemGroup(
        FrameResources& resources,
        const CompiledShaderVariant& variant,
        uint32_t groupIndex,
        render::Buffer* dynamicBuffer) noexcept;

    Nullable<render::BindingGroup*> AcquireMaterialGroup(
        const DrawItem& item,
        const CompiledShaderVariant& variant,
        FrameResources& resources) noexcept;

    Nullable<const DynamicCBufferArena::Allocation*> EnsureObjectBinding(
        FrameResources& resources,
        PrimitiveSceneProxy* proxy) noexcept;

    Nullable<const DynamicCBufferArena::Allocation*> EnsureViewBinding(FrameResources& resources) noexcept;

    Nullable<const ResolvedDrawState*> ResolveDrawState(
        const DrawItem& item,
        FrameResources& resources) noexcept;

    void PrepareFrame(FrameResources& resources) noexcept;
    void RetireStaleMaterialBindings(FrameResources& resources) noexcept;

    Nullable<const MeshDrawCommandTemplate*> GetOrCreateCommandTemplate(
        const DrawItem& item,
        const ResolvedDrawState& state,
        const MeshDrawArgs& args,
        render::BindingGroup* materialGroup,
        FrameResources& resources) noexcept;

    bool CompileCommand(const DrawItem& item, FrameResources& resources) noexcept;
    bool RecordCommand(
        render::GraphicsCommandEncoder* encoder,
        const MeshDrawCommand& command,
        FrameResources& resources,
        uint32_t instanceCount) noexcept;

    render::Device* _device{nullptr};
    ShaderVariantLibrary* _variantCache{nullptr};
    GraphicsPipelineStateLibrary* _psoCache{nullptr};
    SamplerCache* _samplerCache{nullptr};
    render::RenderPass* _renderPass{nullptr};
    std::string _perObjectName;
    std::string _viewName;
    vector<byte> _viewData;
    DynamicCBufferArena::Allocation _viewAllocation{};
    bool _viewBindingValid{false};
    uint64_t _viewGeneration{0};
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
    shared_ptr<render::DescriptorPool> _materialDescriptorPool;
    shared_ptr<MaterialConstantPool> _materialConstantPool;
    vector<MaterialBinding> _materialBindings;
    vector<unique_ptr<MeshDrawCommandTemplate>> _commandTemplates;
    vector<MeshDrawCommand> _commands;
    FrameResources* _lastFrameResources{nullptr};
    uint64_t _lastFrameGeneration{0};
    uint64_t _frameSerial{0};
    struct RecorderState {
        render::GraphicsCommandEncoder* Encoder{nullptr};
        render::GraphicsPipelineState* Pipeline{nullptr};
        std::array<render::BindingGroup*, 3> Groups{};
        std::array<uint32_t, 3> DynamicOffsets{};
        render::VertexBufferView Vertex{};
        render::IndexBufferView Index{};
        bool HasVertex{false};
        bool HasIndex{false};
    } _recorder;
    // 材质常量打包器: 用变体反射把散字段打进所属 cbuffer 块, 整块提交 (push constant / CBV)。
    MaterialConstantBinder _constantBinder;
    // 复用缓冲: 每 draw 收集快照常量供打包器消费, 避免每次分配。
    vector<MaterialConstantValue> _constantScratch;
    // 复用缓冲: ResolvePso 里叠加材质 blend 覆盖时, 拷贝 pass 的 ColorTargets 再改, 避免每次分配。
    vector<render::ColorTargetState> _colorTargetScratch;
};

}  // namespace radray
