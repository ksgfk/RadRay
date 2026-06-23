#include <gtest/gtest.h>

#include <radray/runtime/gpu_system.h>
#include <radray/runtime/renderer/scene_renderer.h>
#include <radray/runtime/shader_variant.h>

using namespace radray;

// 独立项 D 验收:类型化的 PixelShaderMode 与原先两个布尔尾参【逐组合等价】。
// 原始派生(scene_renderer 改前):
//   need        = writeColor || masked
//   alphaClipOnly = !writeColor && masked
// ResolvePixelShaderMode + NeedsPixelShader + IsAlphaClipOnly 必须复刻同一真值表。
namespace {

struct LegacyPsFlags {
    bool Need{false};
    bool AlphaClipOnly{false};
};

LegacyPsFlags LegacyDerive(bool writeColor, bool masked) {
    return LegacyPsFlags{
        .Need = writeColor || masked,
        .AlphaClipOnly = !writeColor && masked};
}

}  // namespace

TEST(PixelShaderModeTest, MatchesLegacyBooleanDerivation) {
    for (bool writeColor : {false, true}) {
        for (bool masked : {false, true}) {
            const LegacyPsFlags legacy = LegacyDerive(writeColor, masked);
            const PixelShaderMode mode = ResolvePixelShaderMode(writeColor, masked);
            EXPECT_EQ(NeedsPixelShader(mode), legacy.Need)
                << "writeColor=" << writeColor << " masked=" << masked;
            EXPECT_EQ(IsAlphaClipOnly(mode), legacy.AlphaClipOnly)
                << "writeColor=" << writeColor << " masked=" << masked;
        }
    }
}

TEST(PixelShaderModeTest, ModeSemanticsAreConsistent) {
    // None:不绑像素着色器。
    EXPECT_FALSE(NeedsPixelShader(PixelShaderMode::None));
    EXPECT_FALSE(IsAlphaClipOnly(PixelShaderMode::None));
    // AlphaClipOnly:绑像素着色器,但走 depth(alpha-clip)入口。
    EXPECT_TRUE(NeedsPixelShader(PixelShaderMode::AlphaClipOnly));
    EXPECT_TRUE(IsAlphaClipOnly(PixelShaderMode::AlphaClipOnly));
    // FullColor:绑完整颜色像素着色器。
    EXPECT_TRUE(NeedsPixelShader(PixelShaderMode::FullColor));
    EXPECT_FALSE(IsAlphaClipOnly(PixelShaderMode::FullColor));
}

// 编译期固化真值表(constexpr 路径),回归时直接编译失败而非运行期。
static_assert(ResolvePixelShaderMode(true, false) == PixelShaderMode::FullColor);
static_assert(ResolvePixelShaderMode(true, true) == PixelShaderMode::FullColor);
static_assert(ResolvePixelShaderMode(false, false) == PixelShaderMode::None);
static_assert(ResolvePixelShaderMode(false, true) == PixelShaderMode::AlphaClipOnly);

// 独立项 E 验收:标准 render state 预设与各 pass 原先手搓的状态【逐字段一致】。
// 这些预设是纯 POD 构造(无 GPU 依赖),故可 headless 验证。
// 下面每个 case 复刻 examples/gltf_viewer 改造前的手写状态,再与预设比对。

namespace {

// 复刻 gltf_viewer Pre-Z pass 改前手写状态(gltf_viewer.cpp:533-535)。
MeshPassRenderState BuildPreZByHand() {
    MeshPassRenderState s{};
    render::DepthStencilState depthState = render::DepthStencilState::Default();
    depthState.DepthCompare = render::CompareFunction::Less;
    depthState.DepthWriteEnable = true;
    s.DepthStencil = depthState;
    return s;
}

// 复刻 shadow caster pass 改前手写状态(gltf_viewer.cpp:364-367,448-451)。
MeshPassRenderState BuildShadowByHand(int32_t depthBias, float slopeBias) {
    MeshPassRenderState s{};
    render::DepthStencilState depthState = render::DepthStencilState::Default();
    depthState.DepthCompare = render::CompareFunction::LessEqual;
    depthState.DepthWriteEnable = true;
    depthState.DepthBias = render::DepthBiasState{depthBias, slopeBias, 0.0f};
    s.DepthStencil = depthState;
    return s;
}

// 复刻 BasePass 改前手写状态(gltf_viewer.cpp:606-608,621)。
MeshPassRenderState BuildOpaqueBaseByHand() {
    MeshPassRenderState s{};
    render::DepthStencilState depthState = render::DepthStencilState::Default();
    depthState.DepthCompare = render::CompareFunction::Equal;
    depthState.DepthWriteEnable = false;
    s.DepthStencil = depthState;
    s.ColorWriteMask = render::ColorWrite::Color;
    return s;
}

// 复刻 TransparentPass 改前手写状态(gltf_viewer.cpp:688-714)。
MeshPassRenderState BuildTransparentByHand() {
    MeshPassRenderState s{};
    render::DepthStencilState depthState = render::DepthStencilState::Default();
    depthState.DepthCompare = render::CompareFunction::LessEqual;
    depthState.DepthWriteEnable = false;
    render::BlendState alphaOverBlend{
        .Color = {
            .Src = render::BlendFactor::SrcAlpha,
            .Dst = render::BlendFactor::OneMinusSrcAlpha,
            .Op = render::BlendOperation::Add},
        .Alpha = {
            .Src = render::BlendFactor::One,
            .Dst = render::BlendFactor::OneMinusSrcAlpha,
            .Op = render::BlendOperation::Add}};
    s.DepthStencil = depthState;
    s.Blend = alphaOverBlend;
    s.ColorWriteMask = render::ColorWrite::Color;
    return s;
}

}  // namespace

TEST(RenderStatePresetTest, PreZMatchesHandBuilt) {
    EXPECT_EQ(MeshPassRenderState::PreZ(), BuildPreZByHand());
}

TEST(RenderStatePresetTest, ShadowMatchesHandBuilt) {
    // gltf_viewer 当前 ShadowRasterDepthBias=0, ShadowRasterSlopeBias=0.0f。
    EXPECT_EQ(MeshPassRenderState::Shadow(0, 0.0f), BuildShadowByHand(0, 0.0f));
    // 非零偏移也应一致(保证参数透传)。
    EXPECT_EQ(MeshPassRenderState::Shadow(2, 1.5f), BuildShadowByHand(2, 1.5f));
}

TEST(RenderStatePresetTest, OpaqueBaseMatchesHandBuilt) {
    EXPECT_EQ(MeshPassRenderState::OpaqueBase(), BuildOpaqueBaseByHand());
}

TEST(RenderStatePresetTest, TransparentMatchesHandBuilt) {
    EXPECT_EQ(MeshPassRenderState::Transparent(), BuildTransparentByHand());
}

// 预设之间必须互不相等(否则说明某个预设退化成了另一个)。
TEST(RenderStatePresetTest, PresetsAreDistinct) {
    EXPECT_NE(MeshPassRenderState::PreZ(), MeshPassRenderState::OpaqueBase());
    EXPECT_NE(MeshPassRenderState::PreZ(), MeshPassRenderState::Transparent());
    EXPECT_NE(MeshPassRenderState::OpaqueBase(), MeshPassRenderState::Transparent());
    EXPECT_NE(MeshPassRenderState::Shadow(), MeshPassRenderState::PreZ());
}

// Transparent 必须设了 Blend;不透明路径必须没设 Blend(optional 为空)。
TEST(RenderStatePresetTest, OnlyTransparentEnablesBlend) {
    EXPECT_FALSE(MeshPassRenderState::PreZ().Blend.has_value());
    EXPECT_FALSE(MeshPassRenderState::Shadow().Blend.has_value());
    EXPECT_FALSE(MeshPassRenderState::OpaqueBase().Blend.has_value());
    EXPECT_TRUE(MeshPassRenderState::Transparent().Blend.has_value());
}
