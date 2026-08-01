#pragma once

#include <optional>
#include <span>

// 【必须显式包含 rhi.h】本头持有活的 GPU 对象 (PipelineLayout), 属 render 层。
// 格式层迁入 radrayshader 后不再传递地带来 rhi.h (它只包含 shader_types.h)。
#include <radray/render/rhi.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/pipeline_layout_cache.h>
#include <radray/shader/shader_manifest.h>

// shader 对象层: pass 级 program, 把 "keyword -> 字节码" 那条链收敛成一次调用。
//
// 三层分工与归属关系、为何刻意不缓存 render::Shader、PipelineLayout 为何共享:
// docs/architecture/shader-pipeline.md

namespace radray {

namespace render {
class PipelineLayout;
}  // namespace render

/// 持有 render::PipelineLayoutDescriptor 所需的全部后备存储。
/// PipelineLayoutDescriptor 内部是 span, 必须有稳定的拥有者。move-only。
///
/// 【瞬态】只用于把 manifest 打包成一份 descriptor 喂给 PipelineLayoutCache, 之后即可
/// 丢弃 —— 缓存把内容归一化进自己的 key, 不引用本对象。
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
/// 【字节码缓存必须在这层】ShaderResolver::Resolve 不缓存字节码, 每次调用都重新读 blob
/// 或重新 JIT。缓存是两级的 (按 artifact key 去重 + 按 variant 索引), 因为 stage 投影
/// 使多个变体共用同一份字节码。
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
    /// 【失败不写变体条目】任一 stage 失败即整体返回 nullptr, 已解析的字节码留在一级
    /// 缓存但不产生半个变体 —— 否则下次命中会拿到一个缺 stage 的变体。
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

    /// 放开 GPU 资源。由 ShaderAsset::OnUnload 调用。
    /// 【不需要延迟销毁】本层唯一的 GPU 对象是共享 PipelineLayout, 仍在录制中的 PSO
    /// 各自持有一份引用, 故放开自己那份是安全的。见 pipeline_layout_cache.h。
    void ReleaseRenderResources() noexcept;

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
    /// 【共享, 非独占】布局相同的其他 program 持同一个对象, 归零时对象自毁。
    IntrusivePtr<SharedPipelineLayout> _pipelineLayout;
    std::optional<ShaderVertexInputStorage> _vertexInput;
    /// 借用而非拥有。持有者 (通常是 ShaderAsset) 必须保证 resolver 活得比本对象久。
    ShaderResolver* _resolver{nullptr};
    vector<unique_ptr<VariantEntry>> _variants;
    vector<unique_ptr<BytecodeEntry>> _bytecodes;
};

}  // namespace radray
