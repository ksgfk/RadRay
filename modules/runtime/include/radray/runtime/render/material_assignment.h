#pragma once

#include <radray/types.h>
#include <radray/basic_math.h>
#include <radray/runtime/asset_manager.h>

namespace radray {

/// 一条按名的材质参数赋值(向量型)。用于把 per-使用点的材质值
/// 从资产加载层(如 glTF)携带到渲染组件,再打包进 space1 cbuffer。
/// 当前仅需 float4(glTF 的 5 个 Principled 参数均为 float4)。
struct MaterialParameterAssignment {
    string Name;
    Eigen::Vector4f Value{Eigen::Vector4f::Zero()};
};

/// 一条按名的材质贴图绑定。名字为 shader 反射出的贴图槽名(如 "gBaseColor")。
/// 从资产加载层(如 glTF)携到渲染组件,再绑入 StandardMaterial 的贴图槽。
/// Texture 为类型擦除弱句柄(通常指向 TextureAsset)。
struct MaterialTextureAssignment {
    string Name;
    StreamingAssetRefAny Texture;
};

}  // namespace radray
