#pragma once

#include <cstdint>
#include <optional>

#include <radray/render/common.h>

namespace radray::srp {

/// 一个 mesh pass 的权威输出合并状态。Depth/Blend/ColorWrites 由 pass 提供,
/// material 只贡献材质语义(twoSided → cull)与光栅化偏好。
/// 等价旧 `radray::MeshPassRenderState`,迁移到本框架命名空间。
/// 注意:DepthStencil.Format 会按当前 pass 的 depth 格式覆写,故预设里不关心 Format。
struct MeshPassRenderState {
    render::DepthStencilState DepthStencil{render::DepthStencilState::Default()};
    std::optional<render::BlendState> Blend{};
    render::ColorWrites ColorWriteMask{render::ColorWrite::All};
    uint32_t StencilRef{0};

    friend bool operator==(const MeshPassRenderState&, const MeshPassRenderState&) = default;

    /// 深度预通道(Pre-Z):depth Less + 写深度,无混合。配合 writeColor=false 使用。
    static MeshPassRenderState PreZ() noexcept {
        MeshPassRenderState s{};
        s.DepthStencil = render::DepthStencilState::Default();
        s.DepthStencil.DepthCompare = render::CompareFunction::Less;
        s.DepthStencil.DepthWriteEnable = true;
        return s;
    }

    /// 阴影投射(depth-only):depth LessEqual + 写深度 + 光栅深度偏移。配合 writeColor=false。
    static MeshPassRenderState Shadow(
        int32_t depthBias = 0,
        float slopeScaledBias = 0.0f,
        float depthBiasClamp = 0.0f) noexcept {
        MeshPassRenderState s{};
        s.DepthStencil = render::DepthStencilState::Default();
        s.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
        s.DepthStencil.DepthWriteEnable = true;
        s.DepthStencil.DepthBias = render::DepthBiasState{depthBias, slopeScaledBias, depthBiasClamp};
        return s;
    }

    /// 不透明基础通道(与 Pre-Z 配对):depth Equal + 不写深度,只写 RGB。
    static MeshPassRenderState OpaqueBase() noexcept {
        MeshPassRenderState s{};
        s.DepthStencil = render::DepthStencilState::Default();
        s.DepthStencil.DepthCompare = render::CompareFunction::Equal;
        s.DepthStencil.DepthWriteEnable = false;
        s.ColorWriteMask = render::ColorWrite::Color;
        return s;
    }

    /// 不透明单通道(无 Pre-Z):depth Less + 写深度,只写 RGB。
    static MeshPassRenderState Opaque() noexcept {
        MeshPassRenderState s{};
        s.DepthStencil = render::DepthStencilState::Default();
        s.DepthStencil.DepthCompare = render::CompareFunction::Less;
        s.DepthStencil.DepthWriteEnable = true;
        s.ColorWriteMask = render::ColorWrite::Color;
        return s;
    }

    /// 透明(alpha-over):depth LessEqual + 不写深度,src-alpha over 混合,只写 RGB。
    static MeshPassRenderState Transparent() noexcept {
        MeshPassRenderState s{};
        s.DepthStencil = render::DepthStencilState::Default();
        s.DepthStencil.DepthCompare = render::CompareFunction::LessEqual;
        s.DepthStencil.DepthWriteEnable = false;
        s.Blend = render::BlendState{
            .Color = {
                .Src = render::BlendFactor::SrcAlpha,
                .Dst = render::BlendFactor::OneMinusSrcAlpha,
                .Op = render::BlendOperation::Add},
            .Alpha = {
                .Src = render::BlendFactor::One,
                .Dst = render::BlendFactor::OneMinusSrcAlpha,
                .Op = render::BlendOperation::Add}};
        s.ColorWriteMask = render::ColorWrite::Color;
        return s;
    }
};

}  // namespace radray::srp
