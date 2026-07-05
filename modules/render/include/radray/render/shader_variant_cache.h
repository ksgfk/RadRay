#pragma once

#include <optional>

#include <radray/guid.h>
#include <radray/render/common.h>
#include <radray/render/shader/hlsl.h>

namespace radray::render {

// 一个 shader 变体的 POD key.
// - ProgramId: 变体所属 program 的调用方分配身份 (通常由 runtime 的 ShaderAsset 持有).
//              render 层不理解 keyword 名字, 仅用它区分不同 program.
// - PassIndex: program 内的 pass 序号 (一个 Shader 可含多个 pass).
// - Bitmask:   启用 keyword 的位掩码, 由调用方投影得到. render 层视其为不透明判别位.
// 所有字段为标量, 构造时 `Key{}` 清零保证 padding 恒为 0,
// 可安全用于 PodHasher (byte-wise xxHash) 与 PodEqual (memcmp).
struct ShaderVariantKey {
    Guid ProgramId;
    uint32_t PassIndex;
    uint32_t _pad{0};
    uint64_t Bitmask;
};

static_assert(std::is_trivially_copyable_v<ShaderVariantKey>, "ShaderVariantKey must be trivially copyable");

// 变体内单个 stage 的编译描述. render 层只做编译, 不解析 keyword,
// 已解析的 "-D" 宏 (形如 "NAME=1") 由 ShaderVariantDescriptor::Defines 统一提供.
struct ShaderVariantStageDesc {
    std::string_view Source{};
    std::string_view EntryPoint{};
    ShaderStage Stage{ShaderStage::UNKNOWN};
};

// 编译并缓存一个变体所需的全部信息.
// - Defines: 调用方按启用 keyword 投影出的宏 token 列表 (每项形如 "NAME=1"),
//            miss 时原样喂给 DXC 的 -D. render 层不检查其与 Bitmask 的一致性.
// - Stages:  该变体的所有 stage (VS+PS 或单个 CS 等), 共用同一组 Defines.
struct ShaderVariantDescriptor {
    Guid ProgramId{};
    uint32_t PassIndex{0};
    uint64_t KeywordBitmask{0};
    std::span<std::string_view> Defines{};
    // include 搜索目录 (原样喂给 DXC 的 -I). 用于解析 shader 源中的 #include.
    // render 层不理解目录内容, 仅透传; 生命周期须覆盖 GetOrCreate 调用。
    std::span<std::string_view> Includes{};
    std::span<const ShaderVariantStageDesc> Stages{};
    std::span<const StaticSamplerDescriptor> StaticSamplers{};
    HlslShaderModel SM{HlslShaderModel::SM60};
    bool IsOptimize{false};
};

// 编译好的变体. 指针非拥有, 生命周期由 ShaderVariantCache 持有.
// 每个 Shader 由缓存在创建时 SetGuid(NewGuid()) 赋予稳定身份,
// 因此可直接参与 PSO 缓存 key.
struct CompiledShaderVariant {
    Shader* VS{nullptr};
    Shader* PS{nullptr};
    Shader* CS{nullptr};
    ShaderBindingLayout* Layout{nullptr};
};

// 从 ShaderVariantDescriptor 构造 POD key.
// 失败情形 (返回 nullopt 并记录错误):
//   - ProgramId 为 Empty (未经上层分配身份)
std::optional<ShaderVariantKey> BuildShaderVariantKey(const ShaderVariantDescriptor& desc) noexcept;

// shader 变体缓存: 相同 (ProgramId, PassIndex, KeywordBitmask) 返回同一编译结果.
// miss 时: 逐 stage 编译 (Vulkan 走 SPIR-V) -> 反射 -> CreateShader -> SetGuid(NewGuid())
//          -> 用全部 stage shader 走 ShaderBindingLayoutCache 得到 layout.
class ShaderVariantCache : public RenderBase {
public:
    virtual ~ShaderVariantCache() noexcept = default;

    RenderObjectTags GetTag() const noexcept final { return RenderObjectTag::UNKNOWN; }

    virtual Nullable<const CompiledShaderVariant*> GetOrCreate(const ShaderVariantDescriptor& desc) noexcept = 0;

    virtual void Clear() noexcept = 0;

    virtual uint32_t Count() const noexcept = 0;
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_DXC

namespace radray::render {

class Dxc;

// 创建 shader 变体缓存. 与 PSO 缓存不同, 变体缓存需要 DXC 编译器与 layout 缓存,
// 因此以自由函数形式提供 (仅在 DXC 启用时可用), 不污染 Device / common.h.
// device / dxc / layoutCache 生命周期须覆盖返回的缓存.
Nullable<unique_ptr<ShaderVariantCache>> CreateShaderVariantCache(
    Device* device,
    Dxc* dxc,
    ShaderBindingLayoutCache* layoutCache) noexcept;

}  // namespace radray::render

#endif
