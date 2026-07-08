#pragma once

#include <optional>

#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/render/common.h>
#include <radray/runtime/material_asset.h>

namespace radray {

/// 对应 Unity 的 MaterialPropertyBlock: per-primitive 的材质参数覆盖。
///
/// 设计要点:
/// - 不修改共享 MaterialAsset (模板)。覆盖值只在【组件生成快照】时叠加到模板属性之上,
///   共享材质保持不变, 可被多个 primitive 复用而互不污染 (对齐 Unity 语义)。
/// - 覆盖粒度与 MaterialAsset 完全一致: 复用 MaterialPropertyValue (float / vector /
///   常量块 / texture / sampler)。名字可以是 cbuffer 块名 (整块) 或块内字段名 (按偏移打包,
///   由 MaterialConstantBinder 在绑定时解析)。
/// - 只覆盖 property, 不涉及 shader / keyword / renderQueue (与 Unity 一致: PropertyBlock
///   不能改 shader 变体)。若需完整独立材质实例, 应另做 MaterialInstance 层。
/// - 内部维护单调递增的版本号: 每次写入自增。组件据此做 O(1) 脏判定, 无需调用方手动标脏。
/// - 纯 CPU 值容器, 不依赖 render device, 可 headless 测试。
class MaterialPropertyBlock {
public:
    MaterialPropertyBlock() noexcept = default;

    bool IsEmpty() const noexcept { return _overrides.empty(); }

    /// 覆盖版本号 (每次写入 / 清除自增)。组件比对此值决定是否重建快照。
    uint64_t GetVersion() const noexcept { return _version; }

    void Clear() noexcept;

    // ─── 覆盖写入 (语义同 MaterialAsset::SetXxx, 但作用于本 block) ───
    void SetFloat(std::string_view name, float value) noexcept;
    void SetVector(std::string_view name, const Eigen::Vector4f& value) noexcept;
    /// 设置一整块常量数据 (Name 为 cbuffer 块名)。
    void SetConstantBlock(std::string_view name, const void* data, size_t size) noexcept;
    /// 通过资产引用设置纹理 (默认全量 SRV)。
    void SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture) noexcept;
    /// 通过资产引用 + 子 view 描述设置纹理 (非默认 view)。
    void SetTexture(std::string_view name, StreamingAssetRef<TextureAsset> texture, const TextureSubViewDesc& sub) noexcept;
    /// 设置采样器 (存 descriptor 值)。
    void SetSampler(std::string_view name, const render::SamplerDescriptor& desc) noexcept;

    /// 移除一个覆盖 (该 property 回落到 MaterialAsset 模板值)。
    void ClearProperty(std::string_view name) noexcept;

    /// 查覆盖值。未覆盖返回 nullopt。
    std::optional<MaterialPropertyValue> GetOverride(std::string_view name) const noexcept;

    const unordered_map<string, MaterialPropertyValue>& GetOverrides() const noexcept { return _overrides; }

private:
    unordered_map<string, MaterialPropertyValue> _overrides;
    uint64_t _version{0};
};

}  // namespace radray
