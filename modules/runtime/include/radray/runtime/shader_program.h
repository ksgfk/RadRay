#pragma once

#include <optional>
#include <span>

// 【必须显式包含 rhi.h】本头持有活的 GPU 对象 (PipelineLayout), 属 render 层。
// 以前它靠 shader_manifest.h 传递地拿到 rhi.h; 格式层迁入 radrayshader 后那条链断了,
// 因为格式层现在只包含 shader_types.h (刻意不依赖任何 device 类型)。
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/shader/shader_manifest.h>

// pass 级 program: 把 "keyword -> 字节码" 那条链收敛成一次调用。
//
// == 三层的分工 ==
//
//   shader_manifest.h  格式层。manifest desc、变体域、产物索引、ShaderResolver、cook。
//                      不含 Asset —— tools/shader_cook 只需要这一层。
//   shader_program.h   对象层。ShaderPassProgram: 共享 PipelineLayout 引用 + 字节码缓存。
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
//          ├─ SharedPipelineLayout 【共享, 非独占】variant / target 无关, 加载期取得。
//          │                       按 binding 布局内容跨 pass / 跨资产去重, 见
//          │                       pipeline_layout_cache.h。
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
//
// == PipelineLayout 是共享的, 本层只持一份引用 ==
//
// layout 只由 binding 布局决定, 与 variant / target 无关, 且规模化后大量 pass 的布局
// 逐字节相同 (见 pipeline_layout_cache.h)。故它归 PipelineLayoutCache 按内容去重,
// 本层持 IntrusivePtr<SharedPipelineLayout> 一份引用, 归零时对象自毁。
//
// 因此 ShaderPipelineLayoutStorage 降级为【瞬态】: 它只在加载期把 manifest 打包成一份
// descriptor 喂给缓存, 缓存把内容拷进自己的 key, 之后 storage 即可丢弃。program 不再
// 存它 —— descriptor 的常驻后备存储是缓存 key, 由 SharedPipelineLayout 持有。

namespace radray {

namespace render {
class PipelineLayout;
}  // namespace render

class IRenderResourceRecycler;

/// 持有 render::PipelineLayoutDescriptor 所需的全部后备存储。
/// PipelineLayoutDescriptor 内部是 span, 必须有稳定的拥有者。move-only。
///
/// 【瞬态】: 只用于把 manifest 打包成一份 descriptor 喂给 PipelineLayoutCache, 之后即可
/// 丢弃。常驻后备存储是缓存 key (见 pipeline_layout_cache.h)。
class ShaderPipelineLayoutStorage {
public:
    ShaderPipelineLayoutStorage() noexcept = default;
    ShaderPipelineLayoutStorage(const ShaderPipelineLayoutStorage&) = delete;
    ShaderPipelineLayoutStorage& operator=(const ShaderPipelineLayoutStorage&) = delete;
    ShaderPipelineLayoutStorage(ShaderPipelineLayoutStorage&&) noexcept = default;
    ShaderPipelineLayoutStorage& operator=(ShaderPipelineLayoutStorage&&) noexcept = default;

    /// 返回的 descriptor 内 span 指向本对象, 本对象存活且未被移动期间有效。
    render::PipelineLayoutDescriptor Get() const noexcept;

    size_t GroupCount() const noexcept { return _sets.size(); }
    bool HasPushConstant() const noexcept { return _pushConstant.has_value(); }

private:
    friend ShaderPipelineLayoutStorage BuildPipelineLayoutStorage(const ShaderPassDesc& pass);

    /// 每组的 entry 列表。稳定地址由 unique_ptr 保证 (vector 扩容不移动内容)。
    vector<unique_ptr<vector<render::ShaderParameterSetLayoutEntryDescriptor>>> _entries;
    vector<render::ShaderParameterSetLayoutDescriptor> _sets;
    std::optional<render::PushConstantDescriptor> _pushConstant;
};

/// 持有 render::VertexInputState 所需的后备存储。move-only。
class ShaderVertexInputStorage {
public:
    ShaderVertexInputStorage() noexcept = default;
    ShaderVertexInputStorage(const ShaderVertexInputStorage&) = delete;
    ShaderVertexInputStorage& operator=(const ShaderVertexInputStorage&) = delete;
    ShaderVertexInputStorage(ShaderVertexInputStorage&&) noexcept = default;
    ShaderVertexInputStorage& operator=(ShaderVertexInputStorage&&) noexcept = default;

    render::VertexInputState Get() const noexcept;

private:
    friend ShaderVertexInputStorage BuildVertexInputStorage(const ShaderVertexInputDesc& desc);

    /// VertexAttribute::Semantic 是 string_view, 需要稳定的字符串后备存储。
    vector<unique_ptr<string>> _semantics;
    vector<render::VertexBufferLayout> _buffers;
    vector<render::VertexAttribute> _attributes;
};

/// 从 manifest 构建 pipeline layout。结果对所有 target 与所有 keyword variant 都相同。
ShaderPipelineLayoutStorage BuildPipelineLayoutStorage(const ShaderPassDesc& pass);

ShaderVertexInputStorage BuildVertexInputStorage(const ShaderVertexInputDesc& desc);

/// 直接构造 CreateShader 所需的 descriptor。
render::ShaderDescriptor MakeShaderDescriptor(const ShaderBytecode& bytecode) noexcept;

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
        IntrusivePtr<SharedPipelineLayout> pipelineLayout,
        std::optional<ShaderVertexInputStorage> vertexInput,
        ShaderResolver* resolver) noexcept;
    ShaderPassProgram(const ShaderPassProgram&) = delete;
    ShaderPassProgram& operator=(const ShaderPassProgram&) = delete;
    ~ShaderPassProgram() noexcept;

    const string& GetName() const noexcept { return _pass.Name; }
    /// Source 已展开为最终路径的 pass 副本 (见 MakeResolvablePass)。
    const ShaderPassDesc& GetDesc() const noexcept { return _pass; }
    const ShaderVariantDomain& GetDomain() const noexcept { return _domain; }

    /// 【共享对象】布局相同的其他 pass 会返回同一个指针, 见 pipeline_layout_cache.h。
    Nullable<render::PipelineLayout*> GetPipelineLayout() const noexcept {
        return _pipelineLayout.HasValue() ? _pipelineLayout->Get() : nullptr;
    }
    /// 供测试与诊断核对共享是否生效。
    const IntrusivePtr<SharedPipelineLayout>& GetSharedPipelineLayout() const noexcept { return _pipelineLayout; }

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
    ///
    /// 【recycler 当前未被用到】: 本层唯一的 GPU 对象是共享的 PipelineLayout, 它按引用
    /// 计数归零即销毁, 不走延迟释放 (理由见 pipeline_layout_cache.h)。参数保留是因为
    /// 签名由 Asset::OnUnload 的契约决定, 且本层将来可能持有独占的 GPU 对象。
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
    /// 【共享, 非独占】一份引用。布局相同的其他 program 持同一个对象, 归零时对象自毁。
    /// descriptor 的后备存储在 SharedPipelineLayout 的 key 里, 故本层无需另存。
    IntrusivePtr<SharedPipelineLayout> _pipelineLayout;
    std::optional<ShaderVertexInputStorage> _vertexInput;
    /// 借用而非拥有。持有者 (通常是 ShaderAsset) 必须保证 resolver 活得比本对象久 ——
    /// 见 shader_asset.h 里 ShaderAsset 的成员声明顺序说明。
    ShaderResolver* _resolver{nullptr};
    vector<unique_ptr<VariantEntry>> _variants;
    vector<unique_ptr<BytecodeEntry>> _bytecodes;
};

}  // namespace radray
