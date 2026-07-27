#pragma once

#include <optional>
#include <string_view>

#include <radray/render/hlsl.h>
#include <radray/render/rhi.h>
#include <radray/render/spirv.h>

// 反射 -> RHI 绑定类型的映射, 以及 HLSL semantic 的归一化。
//
// 【为何独立成头】: 这套映射有两个使用者, 方向相反但必须完全一致 ——
//   - ValidateShaderReflection 用它把反射折叠成 ReflectedBinding 再与 manifest 比对;
//   - GenerateShaderAssetTemplate 用它把反射折叠成 manifest 的初始声明。
// 若两边各写一份, 生成器产出的模板就可能当场通不过校验器, 而这正是模板存在的意义所在。
// 放在 src/ 而非 include/: 它是 runtime 内部实现细节, 不属于 radrayruntime 的公开 ABI。

namespace radray {

/// 反射侧的一条绑定, 归一化到 RHI 词汇后与 manifest 比对或直接生成声明。
struct ReflectedBinding {
    std::string_view Name;
    uint32_t Group{0};
    uint32_t Binding{0};
    render::ShaderParameterBindingType Type{render::ShaderParameterBindingType::UNKNOWN};
    /// 0 表示 unbounded。manifest 必须给出实际容量, 故生成模板时要落到具体数字。
    uint32_t Count{1};
};

/// DXIL 的 D3D_SHADER_INPUT_TYPE -> RHI 绑定类型。
/// 注意 D3D 不区分 Buffer / TexelBuffer 的采样语义, 只能给出反射侧可判定的部分,
/// 因此 Texture 与 TexelBuffer、Buffer 与 RWBuffer 的细分交给 IsBufferDimension。
std::optional<render::ShaderParameterBindingType> MapHlslBindingType(
    const render::HlslInputBindDesc& bind) noexcept;

std::optional<render::ShaderParameterBindingType> MapSpirvBindingType(
    const render::SpirvResourceBinding& bind) noexcept;

/// 一条 DXIL 反射绑定 -> ReflectedBinding。类型无 RHI 对应物时返回 nullopt。
std::optional<ReflectedBinding> MakeReflectedBinding(
    const render::HlslInputBindDesc& bind) noexcept;

/// 一条 SPIRV 反射绑定 -> ReflectedBinding。调用方需自行跳过 PushConstant。
std::optional<ReflectedBinding> MakeReflectedBinding(
    const render::SpirvResourceBinding& bind) noexcept;

/// 去掉 HLSL semantic 名尾部的数字, 得到基名与索引 (DXC 会把 POSITION0 拆成
/// SemanticName="POSITION" + SemanticIndex=0, 但不同来源不一致, 统一归一化)。
void SplitSemantic(std::string_view raw, std::string_view& baseName, uint32_t& index) noexcept;

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept;

/// 系统值语义不由 vertex buffer 提供, 不参与 VertexInput 比对, 也不进生成的模板。
bool IsSystemSemantic(std::string_view baseName) noexcept;

/// semantic 的有效索引: 名字尾部带非零数字时取它, 否则取反射给的 SemanticIndex。
uint32_t EffectiveSemanticIndex(std::string_view rawName, uint32_t semanticIndex) noexcept;

}  // namespace radray
