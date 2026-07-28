#pragma once

#include <optional>
#include <span>

#include <radray/runtime/shader_manifest.h>

// pass 级 program (G4): 把 "keyword -> 字节码" 那条链收敛成一次调用。
//
// == 三层的分工 ==
//
//   shader_manifest.h  格式层。manifest desc、变体域、产物索引、ShaderResolver、cook。
//                      不含 Asset —— tools/shader_cook 只需要这一层。
//   shader_program.h   对象层。ShaderPassProgram: PipelineLayout + 字节码缓存。
//                      【不含 Asset】, 故不依赖 AssetManager / stdexec。
//   shader_asset.h     资产层。ShaderAsset: 一份 manifest 一个 Asset, 持 resolver
//                      与 N 个 ShaderPassProgram。
//
// 本文件刻意停在 Asset 之下: program 的形状与资产系统无关, material 层若只想拿一个
// pass 的 layout + 字节码, 不该被迫拖进 AssetManager。
//
// == 归属关系 ==
//
//   ShaderAsset (见 shader_asset.h)
//     ├─ ShaderAssetDesc      manifest 解析结果
//     ├─ ShaderResolver       一资产一份, 与 "index.json 每 manifest 一个" 对齐
//     └─ ShaderPassProgram[]  每 pass 一个 (本文件)
//          ├─ ShaderPassDesc      Source 已展开的副本
//          ├─ PipelineLayout      variant / target 无关, 加载期建好
//          ├─ ShaderVertexInputStorage  仅 graphics pass
//          ├─ ShaderVariantDomain
//          └─ 字节码缓存 (两级, 见 ShaderPassProgram)
//
// == 刻意不包含 render::Shader 与 PSO ==
//
// `render::Shader` 是【瞬态参数, 不是资源】: 两个后端都只在建 PSO 时消费它, PSO
// 建成后无任何回指 —— D3D12 的 GraphicsPsoD3D12 成员只有 device/layout/pso/
// vertexStrides/topo (字节码在 CreateGraphicsPipelineState 内被拷进 PSO),
// Vulkan 的 GraphicsPipelineVulkan 成员只有 device/layout/pipeline (规范也明确允许
// pipeline 建成后立即销毁 shader module)。`ShaderEntry::EntryPoint` 那个 string_view
// 同理只需活到调用返回。
//
// 所以 Shader 应在建 PSO 的函数里当局部量创建, 出作用域即销毁。常驻缓存它只能省下
// 一次 CreateShader (输入就是本层缓存的字节码, 不读盘不 JIT), 代价却是永久驻留全部
// VkShaderModule / 字节码副本。
//
// PSO 则相反, 必须常驻缓存, 但归 RenderSystem —— PSO 的 key 比字节码宽 (含
// MaterialRenderState、vertex layout、RT 格式), 同一份字节码会喂给多个 PSO。
// 【注意】两个后端的 PSO 都存了 PipelineLayout 裸指针, 而 layout 归本层所有, 故 PSO
// 缓存条目必须持一个 StreamingAssetRef<ShaderAsset> 把资产钉住, 否则资产卸载后悬垂。

namespace radray {

namespace render {
class PipelineLayout;
}  // namespace render

class IRenderResourceRecycler;

/// 一个 (pass, variant, category) 已解析好的全部 stage 字节码。
///
/// 由 ShaderPassProgram 拥有, 地址在 program 存活期内稳定 —— 调用方可以存指针。
class ShaderProgramVariant {
public:
    /// 一个 stage 的解析结果。Bytecode 与 EntryPoint 都指向 program 内的稳定存储。
    struct StageBlob {
        render::ShaderStage Stage{render::ShaderStage::UNKNOWN};
        /// 指向 program 持有的 ShaderPassDesc 副本, 可直接填 render::ShaderEntry。
        std::string_view EntryPoint;
        const ShaderBytecode* Bytecode{nullptr};
    };

    std::span<const StageBlob> Stages() const noexcept { return _stages; }

    Nullable<const ShaderBytecode*> FindBytecode(render::ShaderStage stage) const noexcept;
    std::optional<std::string_view> FindEntryPoint(render::ShaderStage stage) const noexcept;

    /// 本变体覆盖的 stage 并集。
    render::ShaderStages GetStageMask() const noexcept;

private:
    friend class ShaderPassProgram;

    vector<StageBlob> _stages;
};

/// 一个 pass 的运行时 program。把 "keyword -> 字节码" 那条四步链收敛到一次调用。
///
/// 【为何必须在这层缓存字节码】: ShaderResolver::Resolve 不缓存字节码, 每次调用都会
/// 重新读 blob 或重新 JIT (它缓存的只有 index 与源码身份)。若本层不缓存, 每次 PSO
/// cache miss 都要重新读盘 / 重编。
///
/// 缓存是两级的:
///   1. ShaderHash -> ShaderBytecode   拥有字节码, 按 artifact key 去重;
///   2. (variant, category) -> 各 stage 的 ShaderHash。
/// 两级是必要的, 因为 ProjectToStage 保证 "两个变体投影相同 <=> 该 stage 共用同一份
/// 字节码" (实测 forward_pass 的 Deduplicated == 1)。只按变体缓存会存多份副本。
///
/// 非线程安全: 内部惰性缓存, 且底层 ShaderResolver 本就非线程安全。
class ShaderPassProgram {
public:
    ShaderPassProgram(
        ShaderPassDesc pass,
        ShaderVariantDomain domain,
        ShaderPipelineLayoutStorage layoutStorage,
        unique_ptr<render::PipelineLayout> pipelineLayout,
        std::optional<ShaderVertexInputStorage> vertexInput,
        ShaderResolver* resolver) noexcept;
    ShaderPassProgram(const ShaderPassProgram&) = delete;
    ShaderPassProgram& operator=(const ShaderPassProgram&) = delete;
    ~ShaderPassProgram() noexcept;

    const string& GetName() const noexcept { return _pass.Name; }
    /// Source 已展开为最终路径的 pass 副本 (见 MakeResolvablePass)。
    const ShaderPassDesc& GetDesc() const noexcept { return _pass; }
    const ShaderVariantDomain& GetDomain() const noexcept { return _domain; }

    Nullable<render::PipelineLayout*> GetPipelineLayout() const noexcept { return _pipelineLayout.get(); }

    /// 仅 graphics pass 有值。返回的 VertexInputState 内含 span, 指向本对象。
    std::optional<render::VertexInputState> GetVertexInputState() const noexcept;

    /// 解析一个变体的全部 stage。命中缓存直接返回, 未命中则逐 stage 解析。
    ///
    /// 【失败不写缓存】: 中途任一 stage 失败即整体返回 nullptr, 已解析的 stage 字节码
    /// 仍留在一级缓存 (它们本身是有效的, 且按 key 去重), 但不产生半个变体条目 ——
    /// 否则下次命中会拿到一个缺 stage 的变体。
    Nullable<const ShaderProgramVariant*> GetOrCreateVariant(
        const ShaderVariantKey& variant,
        render::ShaderBlobCategory category,
        ShaderAssetDiagnostic& outDiag) noexcept;

    /// 便利入口: 默认变体 (可选组全关, 必选组取首个)。
    Nullable<const ShaderProgramVariant*> GetOrCreateDefaultVariant(
        render::ShaderBlobCategory category,
        ShaderAssetDiagnostic& outDiag) noexcept;

    /// 已缓存的变体数 / 字节码条目数。供测试与诊断核对去重是否生效。
    size_t GetCachedVariantCount() const noexcept { return _variants.size(); }
    size_t GetCachedBytecodeCount() const noexcept { return _bytecodes.size(); }

    /// 交出 GPU 资源。由 ShaderAsset::OnUnload 调用。
    void ReleaseRenderResources(IRenderResourceRecycler& recycler) noexcept;

private:
    struct VariantEntry {
        ShaderVariantKey Key;
        render::ShaderBlobCategory Category{render::ShaderBlobCategory::DXIL};
        ShaderProgramVariant Variant;
    };

    /// 按 artifact key 去重的字节码。unique_ptr 保证地址稳定 —— StageBlob 存的是裸指针。
    struct BytecodeEntry {
        ShaderHash Key{};
        ShaderBytecode Bytecode;
    };

    Nullable<const ShaderBytecode*> GetOrResolveBytecode(
        render::ShaderStage stage,
        render::ShaderBlobCategory category,
        std::span<const string> defines,
        ShaderAssetDiagnostic& outDiag) noexcept;

    ShaderPassDesc _pass;
    ShaderVariantDomain _domain;
    /// PipelineLayoutDescriptor 内是 span, 必须留住后备存储 —— 即使 layout 已创建,
    /// 未来重建 (如设备丢失) 仍需要它。
    ShaderPipelineLayoutStorage _layoutStorage;
    unique_ptr<render::PipelineLayout> _pipelineLayout;
    std::optional<ShaderVertexInputStorage> _vertexInput;
    /// 借用而非拥有。持有者 (通常是 ShaderAsset) 必须保证 resolver 活得比本对象久 ——
    /// 见 shader_asset.h 里 ShaderAsset 的成员声明顺序说明。
    ShaderResolver* _resolver{nullptr};
    vector<unique_ptr<VariantEntry>> _variants;
    vector<unique_ptr<BytecodeEntry>> _bytecodes;
};

}  // namespace radray
