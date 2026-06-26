#pragma once

#include <cstdint>
#include <string_view>

#include <radray/enum_flags.h>

namespace radray::srp {

/// 每个 pass 声明它本趟需要为每个 draw 准备哪些 per-object 数据。
/// 对应 `UnityEngine.Rendering.PerObjectData`。这是废掉旧 `ObjectConstants` god-struct
/// 的关键依据:per-object 需求归 pass,不归 material,也无跨 pass 统一结构。
/// 源码实证:forward 用完整集(RenderingUtils.cs:763),DepthOnlyPass 用 None(DepthOnlyPass.cs:71)。
enum class PerObjectData : uint32_t {
    None = 0,
    LightProbe = 1u << 0,
    Lightmaps = 1u << 1,
    LightData = 1u << 2,      ///< per-object light index 列表
    MotionVectors = 1u << 3,
    ReflectionProbes = 1u << 4,
};

constexpr std::string_view format_as(PerObjectData v) noexcept {
    switch (v) {
        case PerObjectData::None: return "None";
        case PerObjectData::LightProbe: return "LightProbe";
        case PerObjectData::Lightmaps: return "Lightmaps";
        case PerObjectData::LightData: return "LightData";
        case PerObjectData::MotionVectors: return "MotionVectors";
        case PerObjectData::ReflectionProbes: return "ReflectionProbes";
        default: return "Unknown";
    }
}

}  // namespace radray::srp

template <>
struct radray::is_flags<radray::srp::PerObjectData> : std::true_type {};

namespace radray::srp {
using PerObjectDataFlags = EnumFlags<PerObjectData>;
}  // namespace radray::srp
