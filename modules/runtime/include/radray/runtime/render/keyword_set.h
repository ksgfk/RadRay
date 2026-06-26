#pragma once

#include <radray/runtime/shader_variant.h>

namespace radray::srp {

/// Keyword 集合。复用现有的 `ShaderVariantKey`(去重的 define 容器),
/// 它已提供 `Add` / `Merge` / `AppendSignature` / 相等比较与哈希。
/// 设计依据:rewrite_api_cache.md(key 容器保留,PixelShaderMode 策略另删)。
///
/// 两类 keyword 轴(对齐 URP),都用同一容器表达,区别只在【谁驱动】:
///  - PassKeywords  = URP `multi_compile`,管线驱动(阴影开关、灯光数等)。
///  - MaterialKeywords = URP `shader_feature`,material 驱动(_NORMALMAP、_ALPHATEST 等)。
/// 变体 key = (ShaderId, LightMode, PassKeywords ∪ MaterialKeywords)。
using KeywordSet = ShaderVariantKey;

}  // namespace radray::srp
