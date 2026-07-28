#pragma once

#include <optional>
#include <span>

#include <radray/runtime/shader_asset.h>

// PSO 库。把 "program + 变体 + 固定功能状态 -> GraphicsPipelineState" 收敛成一次调用,
// 并按 key 缓存。
//
// == 在分层里的位置 ==
//
//   shader_manifest.h      格式层。manifest / resolver / cook。
//   shader_program.h       对象层。PipelineLayout + 字节码缓存。
//   shader_asset.h         资产层。一份 manifest 一个 Asset。
//   pipeline_state_cache.h 管线状态层 (本文件)。唯一创建 render::Shader 与 PSO 的地方。
//
// 【为什么 render::Shader 只在这里出现】: 它是瞬态参数, 不是资源。两个后端都只在建 PSO
// 时消费它, PSO 建成后无任何回指 —— D3D12 在 CreateGraphicsPipelineState 内把字节码拷进
// PSO, Vulkan 规范明确允许 pipeline 建成后立即销毁 shader module。ShaderEntry::EntryPoint
// 的 string_view 同理只需活到调用返回。所以 Shader 是 GetOrCreateGraphics 内的局部量,
// 出作用域即销毁; 常驻缓存它只能省下一次 CreateShader (输入是上层已缓存的字节码, 不读盘
// 不 JIT), 代价却是永久驻留全部 VkShaderModule。
//
// 【固定功能状态必须由调用方给全】: 本层不做 "基线 + 覆盖" 合成。manifest 刻意不含固定
// 功能段 (shader_manifest.h 的 ShaderAssetDesc), 而 MaterialRenderState 的注释又说沿用
// "pass 基线" —— 那个基线在重写中被删掉了, 两处形成空环 (见
// docs/shader_asset_gap_analysis.md 的 G13 / 8.6)。若本层自己补一套默认值, 它就成了第二套
// 真相: manifest 校验与反射校验都看不到固定功能状态, 谁都拦不住两边写歪。本层是执行层,
// 要求一份已经完整的状态。合成属未来的 material 层。

namespace radray {

/// 一个 graphics PSO 的缓存身份。
///
/// 与 render::GraphicsPipelineStateDescriptor 的固定功能段一一对应, 调用方必须填满 ——
/// 本层不补默认值 (见文件头)。
///
/// 【不含 ShaderVariantKey】: 字节码身份用解析后的 ShaderHash 表达, 由本层在 miss 路径上
/// 自行取得, 不进 key 的公开形状。理由有两条: ShaderVariantKey 是变长结构且属作者期概念
/// (见 shader_manifest.h 的说明), 不该进每帧路径; 且两个不同变体若投影到同一份字节码
/// (ShaderProgramVariant 的共享机制), 本就该命中同一个 PSO。
///
/// 【Program 指针即代表 PipelineLayout 与 vertex input】: 二者都是 pass 级、与 variant
/// 无关, 且由 program 拥有, 故指针相同即这两项相同。
struct GraphicsPipelineStateKey {
    /// 非 const: miss 路径要调 GetOrCreateVariant。必须非空。
    ShaderPassProgram* Program{nullptr};
    /// 必须非空 (Vulkan 要求显式给出)。RenderPassRegistry 已按 attachment 描述去重,
    /// 故指针相同即兼容类相同。
    render::RenderPass* CompatibleRenderPass{nullptr};
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    std::optional<render::DepthStencilState> DepthStencil{};
    render::MultiSampleState MultiSample{};
    /// 仅需活到调用返回 —— 缓存条目会拷一份。
    std::span<const render::ColorTargetState> ColorTargets{};
};

/// 按 key 缓存 graphics PSO。
///
/// 【只做 graphics】: compute PSO 目前零消费者, 现有 manifest 里也没有 compute pass。
/// 等真有 compute pass 时再加, 免得先造一个没有使用点的对称 API。
///
/// 非线程安全 (与 RenderPassRegistry 一致)。
class PipelineStateCache {
public:
    explicit PipelineStateCache(render::Device* device) noexcept;
    ~PipelineStateCache() noexcept;
    PipelineStateCache(const PipelineStateCache&) = delete;
    PipelineStateCache& operator=(const PipelineStateCache&) = delete;

    /// 取或建一个 graphics PSO。
    ///
    /// asset 必须是 key.Program 所属的资产 —— 条目持它的一份引用把资产钉住, 因为两个后端
    /// 的 PSO 都存了 PipelineLayout 裸指针而 layout 归 ShaderPassProgram 所有。
    ///
    /// 【失败不写缓存】: 任一步失败返回 nullptr, 原因写入 outDiag。字节码仍会留在 program
    /// 的缓存里 (它本身有效), 但不产生 PSO 条目。
    Nullable<render::GraphicsPipelineState*> GetOrCreateGraphics(
        const StreamingAssetRef<ShaderAsset>& asset,
        const GraphicsPipelineStateKey& key,
        const ShaderVariantKey& variant,
        render::ShaderBlobCategory category,
        ShaderAssetDiagnostic& outDiag) noexcept;

    /// 逐出所有引用该资产的 PSO。
    ///
    /// 【为什么需要它】: 条目持 StreamingAssetRef 只挡住 AssetManager::CollectUnreferenced,
    /// 挡不住显式 Unload —— 后者是 "确需强制清空" 的场景, 不看引用计数。显式 Unload 的
    /// 调用方必须先调本函数。
    uint32_t RemovePipelineStatesUsing(const ShaderAsset* asset) noexcept;

    void Clear() noexcept;

    uint32_t GetGraphicsPipelineStateCount() const noexcept {
        return static_cast<uint32_t>(_graphics.size());
    }
    uint64_t GetGraphicsHitCount() const noexcept { return _graphicsHits; }
    uint64_t GetGraphicsMissCount() const noexcept { return _graphicsMisses; }

private:
    struct GraphicsEntry {
        ShaderPassProgram* Program{nullptr};
        render::RenderPass* CompatibleRenderPass{nullptr};
        /// 各 stage 的字节码 key。stage 集合与顺序也参与比较 —— 少一个 stage 是不同的 PSO。
        vector<std::pair<render::ShaderStage, ShaderHash>> StageKeys;
        render::PrimitiveState Primitive{};
        std::optional<render::DepthStencilState> DepthStencil{};
        render::MultiSampleState MultiSample{};
        vector<render::ColorTargetState> ColorTargets;
        /// 【只作身份比较, 从不解引用】RemovePipelineStatesUsing 用它匹配。不能改用
        /// Ref.Get() —— 资产被 Unload 后 Ref 立刻失效返回 nullptr, 那时正是最需要逐出的
        /// 时刻。
        const ShaderAsset* Owner{nullptr};
        /// 钉住资产, 防 PipelineLayout 悬垂。
        StreamingAssetRef<ShaderAsset> Ref;
        unique_ptr<render::GraphicsPipelineState> Object;
    };

    render::Device* _device{nullptr};
    vector<GraphicsEntry> _graphics;
    uint64_t _graphicsHits{0};
    uint64_t _graphicsMisses{0};
};

}  // namespace radray
