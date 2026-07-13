#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <radray/render/common.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/pipeline_state_cache.h>
#include <radray/runtime/sampler_cache.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/runtime/shader_binding_plan.h>
#include <radray/runtime/shader_parameter_set.h>
#include <radray/runtime/render_framework/material_render_snapshot.h>
#include <radray/runtime/render_framework/primitive_scene_proxy.h>
#include <radray/types.h>

namespace radray {

class DrawList;
class PrimitiveSceneProxy;
class ShaderDefaultResourceLibrary;
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
    GpuMesh::DrawData Geometry{};
    uint32_t FirstIndex{0};
    uint32_t IndexCount{0};
    int32_t VertexOffset{0};
    uint64_t PipelineId{0};
    uint64_t GeometryPageId{0};
    uint64_t LastUsedFrame{0};
};

struct MeshDrawCommand {
    struct BindingGroupState {
        uint32_t GroupIndex{0};
        render::BindingGroup* Group{nullptr};
        vector<uint32_t> DynamicOffsets;
    };

    const MeshDrawCommandTemplate* Template{nullptr};
    uint64_t PerObjectBufferPage{0};
    vector<BindingGroupState> BindingGroups;
    bool IsErrorFallback{false};
    MaterialBindingKey SortMaterialKey{};
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
///     3. ShaderBindingPlan 按实际反射解析任意 group 及字段 source
///     4. 按 scope 分配/缓存 cbuffer，并解析材质默认值、override、纹理与采样器
///     5. encoder: BindPSO / BindVB / BindIB / BindBindingGroup / DrawIndexed
///
/// 设计要点:
/// - group 生命周期取其最频繁 source；标准 Object/View/Material ABI 自动落入快路径。
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
        std::string perObjectCBufferName = "PerObject",
        Nullable<ShaderDefaultResourceLibrary*> defaultResources = nullptr,
        shared_ptr<const MaterialRenderSnapshot> errorMaterial = nullptr) noexcept;

    MeshPassExecutor(const MeshPassExecutor&) = delete;
    MeshPassExecutor& operator=(const MeshPassExecutor&) = delete;

    /// 设置一个 per-view 常量块 (可选)。传入的字节在下一次需要它的 draw 前上传一次，
    /// 同一设置下的后续 draw 复用该 cbuffer。传空 name 关闭该功能。
    void SetViewConstants(std::string_view viewCBufferName, std::span<const byte> data) noexcept;
    void SetViewParameter(std::string_view name, std::span<const byte> data);
    void SetViewResource(std::string_view name, render::ResourceView* resource) noexcept;
    void SetViewSampler(std::string_view name, render::Sampler* sampler) noexcept;
    void SetPassConstant(std::string_view name, std::span<const byte> data);
    void SetPassResource(std::string_view name, render::ResourceView* resource) noexcept;
    void SetPassSampler(std::string_view name, render::Sampler* sampler) noexcept;

    const ShaderParameterSet& GetViewParameters() const noexcept { return _viewParameters; }
    const ShaderParameterSet& GetPassParameters() const noexcept { return _passParameters; }
    void SetErrorMaterial(shared_ptr<const MaterialRenderSnapshot> material) noexcept {
        _errorMaterial = std::move(material);
    }

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

    struct GenericMaterialBinding {
        MaterialBindingKey Key{};
        shared_ptr<const MaterialRenderSnapshot> Snapshot{};
        render::PipelineLayout* Layout{nullptr};
        uint32_t GroupIndex{0};
        unique_ptr<render::BindingGroup> Group{};
        vector<MaterialConstantPool::Allocation> ConstantAllocations;
        vector<uint32_t> DynamicOffsets;
        uint64_t LastUsedFrame{0};
    };

    enum class BindingResolveStatus {
        Ready,
        Pending,
        Invalid,
    };

    struct BindingResolveResult {
        BindingResolveStatus Status{BindingResolveStatus::Invalid};
        vector<MeshDrawCommand::BindingGroupState> Groups;
        uint64_t ObjectBufferPage{0};
        ShaderBindingDiagnostic Diagnostic{};
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

    BindingResolveResult ResolveBindingGroups(
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
        FrameResources& resources) noexcept;

    bool CompileCommand(const DrawItem& item, FrameResources& resources) noexcept;
    bool CompileCommandInternal(
        const DrawItem& item,
        FrameResources& resources,
        bool allowErrorFallback,
        bool isErrorFallback) noexcept;
    bool CompileErrorFallback(
        const DrawItem& item,
        FrameResources& resources,
        std::string_view reason) noexcept;
    void ReportDiagnostic(
        const DrawItem& item,
        const ShaderBindingDiagnostic& diagnostic) noexcept;
    bool RecordCommand(
        render::GraphicsCommandEncoder* encoder,
        const MeshDrawCommand& command,
        FrameResources& resources,
        uint32_t instanceCount) noexcept;

    render::Device* _device{nullptr};
    ShaderVariantLibrary* _variantCache{nullptr};
    GraphicsPipelineStateLibrary* _psoCache{nullptr};
    SamplerCache* _samplerCache{nullptr};
    ShaderDefaultResourceLibrary* _defaultResources{nullptr};
    shared_ptr<const MaterialRenderSnapshot> _errorMaterial;
    render::RenderPass* _renderPass{nullptr};
    std::string _perObjectName;
    std::string _viewName;
    vector<byte> _viewData;
    DynamicCBufferArena::Allocation _viewAllocation{};
    bool _viewBindingValid{false};
    uint64_t _viewGeneration{0};
    ShaderParameterSet _viewParameters;
    ShaderParameterSet _passParameters;
    // 管线级全局 keyword (per-pass, 非 per-material)。名字借用调用方存储。
    vector<std::string_view> _globalKeywords;
    vector<ResolvedDrawState> _resolvedDrawStates;
    shared_ptr<render::DescriptorPool> _materialDescriptorPool;
    shared_ptr<MaterialConstantPool> _materialConstantPool;
    vector<GenericMaterialBinding> _genericMaterialBindings;
    vector<unique_ptr<MeshDrawCommandTemplate>> _commandTemplates;
    vector<MeshDrawCommand> _commands;
    FrameResources* _lastFrameResources{nullptr};
    uint64_t _lastFrameGeneration{0};
    uint64_t _frameSerial{0};
    struct RecorderState {
        struct GenericGroup {
            uint32_t GroupIndex{0};
            render::BindingGroup* Group{nullptr};
            vector<uint32_t> DynamicOffsets;
        };

        render::GraphicsCommandEncoder* Encoder{nullptr};
        render::GraphicsPipelineState* Pipeline{nullptr};
        vector<GenericGroup> GenericGroups;
        render::VertexBufferView Vertex{};
        render::IndexBufferView Index{};
        bool HasVertex{false};
        bool HasIndex{false};
    } _recorder;
    ShaderBindingPlanLibrary _bindingPlans;
    vector<ShaderBindingDiagnostic> _reportedDiagnostics;
    // 复用缓冲: ResolvePso 里叠加材质 blend 覆盖时, 拷贝 pass 的 ColorTargets 再改, 避免每次分配。
    vector<render::ColorTargetState> _colorTargetScratch;
};

}  // namespace radray
