#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <radray/types.h>
#include <radray/render/common.h>
#include <radray/render/gpu_resource.h>
#include <radray/render/shader_variant_cache.h>

namespace radray {

/// 一条材质常量输入 (供打包器消费)。Name 可以是:
/// - cbuffer 块名 (整块覆盖, 对应 SetConstantBlock / 旧的按块名 SetVector);
/// - cbuffer 块内字段名 (对应 Unity 的 SetFloat("_Color") 语义, 按反射偏移打包)。
/// Bytes 借用调用方存储, 仅在 Bind 调用期间有效。
struct MaterialConstantValue {
    std::string_view Name{};
    std::span<const byte> Bytes{};
};

/// 反射驱动的材质常量打包器 (对应 Unity 把散字段打进 UnityPerMaterial cbuffer 的那层)。
///
/// 问题背景:
/// - HLSL 里材质参数打包进 cbuffer 块; 但材质 API 是按【字段名】(SetFloat("_Color")) 写值。
/// - 底层 ShaderParameterTable 只按【块名】索引参数, 且 SetBytes 只认整块 root constant。
/// - 因此需要一层用 shader 反射把"字段名 -> (所属块, 块内偏移, 大小)"解析出来, 在 CPU 端
///   把散字段打进块的正确偏移, 再整块提交。
///
/// 设计要点:
/// - 块布局按【变体指针】缓存 (反射是 per-variant 的: keyword 变了布局可能变)。变体由
///   ShaderVariantCache 永生持有, 指针稳定, 可安全作为 key。
/// - 打包时区分块的绑定类型:
///   - Kind == Constant (push/root constant): 打包成整块后走 SetBytes。
///   - Kind == Resource (真实 cbuffer / CBV): 从 CBufferArena 分配 upload buffer,
///     memcpy 整块后走 SetResource(BufferViewUsage::CBuffer)。
/// - 材质字段可落在【任意非系统 cbuffer】: 打包器扫所有 cbuffer 块, 按名匹配填入,
///   reservedBlockNames (per-object / per-view 等系统块) 被跳过。
/// - 纯打包逻辑, 不拥有 GPU 资源; upload buffer 生命周期归传入的 arena。
class MaterialConstantBinder {
public:
    MaterialConstantBinder() noexcept = default;
    MaterialConstantBinder(const MaterialConstantBinder&) = delete;
    MaterialConstantBinder& operator=(const MaterialConstantBinder&) = delete;
    MaterialConstantBinder(MaterialConstantBinder&&) noexcept = default;
    MaterialConstantBinder& operator=(MaterialConstantBinder&&) noexcept = default;
    ~MaterialConstantBinder() noexcept = default;

    /// 把材质常量值按反射打包并绑定到参数表。
    /// - variant: 提供反射 (VS/PS 或 CS) 与 binding layout。
    /// - table:   目标参数表 (每 draw 独立)。
    /// - arena:   CBV 块的 upload buffer 来源 (per-flight 线性分配器)。
    /// - values:  材质常量输入 (块名 / 字段名混合)。
    /// - reservedBlockNames: 跳过的系统块名 (per-object / per-view 等, 由执行器单独填充)。
    /// 返回成功绑定的 cbuffer 块数 (至少有一个值命中的块)。
    uint32_t Bind(
        const render::CompiledShaderVariant& variant,
        render::ShaderParameterTable& table,
        render::CBufferArena& arena,
        std::span<const MaterialConstantValue> values,
        std::span<const std::string_view> reservedBlockNames) noexcept;

    /// 清空布局缓存 (变体缓存被清空 / 重编译后调用, 避免悬垂指针命中)。
    void ClearCache() noexcept;

private:
    /// 一个 cbuffer 块内字段的偏移布局。
    struct FieldLayout {
        string Name;
        uint32_t Offset{0};
        uint32_t Size{0};
    };

    /// 一个可承接材质常量的 cbuffer 块。
    struct BlockLayout {
        string Name;                                     // 块名 (= binding 名)
        render::ShaderParameterId Id{0};                 // 在 binding layout 中的参数 id
        uint32_t Size{0};                                // 整块字节数
        render::ShaderParameterKind Kind{render::ShaderParameterKind::UNKNOWN};  // Constant / Resource
        vector<FieldLayout> Fields;
    };

    /// 取变体的块布局 (命中缓存或首次从反射提取)。
    const vector<BlockLayout>& GetOrExtract(const render::CompiledShaderVariant& variant) noexcept;

    /// 从一个 Shader 的反射把 cbuffer 块累加进 out (按块名去重: 已存在则跳过)。
    static void AppendBlocksFromReflection(
        render::Shader* shader,
        render::ShaderBindingLayout* layout,
        vector<BlockLayout>& out) noexcept;

    unordered_map<const render::CompiledShaderVariant*, vector<BlockLayout>> _cache;
};

}  // namespace radray
