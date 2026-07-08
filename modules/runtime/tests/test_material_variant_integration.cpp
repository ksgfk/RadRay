#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <radray/guid.h>
#include <radray/render/shader_variant_cache.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/runtime/render_framework/sampler_cache.h>

namespace radray::render::test {
namespace {

// 一个带 keyword 与 cbuffer property 的最小 VS+PS pass。
// _TINT keyword 影响 PS 输出; MaterialParams cbuffer 承载 _BaseColor property。
constexpr std::string_view kLitPassSource = R"(
struct MaterialParams {
    float4 BaseColor;
};
// push/root constant: ApplyProperties 的 float/vector 走 SetBytes, 对应 push/root constant,
// 因此 property cbuffer 必须声明为 push_constant 才能跨 D3D12/Vulkan 一致工作。
[[vk::push_constant]] ConstantBuffer<MaterialParams> gMaterial : register(b0, space0);

struct VSOut {
    float4 Pos : SV_POSITION;
    float2 UV : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.UV = uv;
    o.Pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET {
    float4 c = gMaterial.BaseColor;
#ifdef _TINT
    c.rgb *= float3(2.0, 1.0, 0.5);
#endif
    return c;
}
)";

class MaterialVariantIntegrationTest : public ::testing::TestWithParam<TestBackend> {
protected:
    void SetUp() override {
        string reason;
        if (!_ctx.Initialize(this->GetParam(), &reason)) {
            GTEST_SKIP() << fmt::format("Init failed on {}: {}", format_as(this->GetParam()), reason);
        }
        _cache = CreateShaderVariantCache(
            _ctx.GetDevicePtr(), _ctx.GetDxc(), _ctx.GetShaderBindingLayoutCache());
        ASSERT_TRUE(_cache.HasValue());
        _variantCache = _cache.Release();
    }

    static unique_ptr<ShaderAsset> MakeLitShader() {
        ShaderKeywordSet kw;
        kw.Add("_TINT");  // bit 0
        vector<ShaderPassDesc> passes;
        passes.push_back(ShaderPassDesc{.PassTag = "ForwardLit", .Source = string{kLitPassSource}});
        return std::make_unique<ShaderAsset>(std::move(kw), std::move(passes));
    }

    ComputeTestContext _ctx{};
    Nullable<unique_ptr<ShaderVariantCache>> _cache{};
    unique_ptr<ShaderVariantCache> _variantCache{};
};

TEST_P(MaterialVariantIntegrationTest, MaterialResolvesGraphicsVariant) {
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(), MakeLitShader());

    auto passIdx = shaderRef->FindPassByTag("ForwardLit");
    ASSERT_TRUE(passIdx.has_value());

    MaterialAsset material{shaderRef};

    _ctx.ClearCapturedErrors();
    auto variant = material.ResolveVariant(*_variantCache, passIdx.value());
    ASSERT_TRUE(variant.HasValue()) << _ctx.JoinCapturedErrors();
    ASSERT_NE(variant.Get()->VS, nullptr);
    ASSERT_NE(variant.Get()->PS, nullptr);
    EXPECT_NE(variant.Get()->Layout, nullptr);
    // 变体 shader 应被赋予稳定身份 (可参与 PSO 缓存)。
    EXPECT_FALSE(variant.Get()->VS->GetGuid().IsEmpty());
    EXPECT_FALSE(variant.Get()->PS->GetGuid().IsEmpty());
    EXPECT_NE(variant.Get()->VS->GetGuid(), variant.Get()->PS->GetGuid());
    EXPECT_EQ(_variantCache->Count(), 1u);
}

TEST_P(MaterialVariantIntegrationTest, KeywordChangesVariant) {
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(), MakeLitShader());
    const auto passIdx = shaderRef->FindPassByTag("ForwardLit").value();

    MaterialAsset base{shaderRef};
    MaterialAsset tinted{shaderRef};
    tinted.EnableKeyword("_TINT");

    _ctx.ClearCapturedErrors();
    auto baseVariant = base.ResolveVariant(*_variantCache, passIdx);
    ASSERT_TRUE(baseVariant.HasValue()) << _ctx.JoinCapturedErrors();
    auto tintedVariant = tinted.ResolveVariant(*_variantCache, passIdx);
    ASSERT_TRUE(tintedVariant.HasValue()) << _ctx.JoinCapturedErrors();

    // 不同 keyword 集 -> 不同变体对象。
    EXPECT_NE(baseVariant.Get(), tintedVariant.Get());
    EXPECT_NE(baseVariant.Get()->PS, tintedVariant.Get()->PS);
    EXPECT_NE(baseVariant.Get()->PS->GetGuid(), tintedVariant.Get()->PS->GetGuid());
    EXPECT_EQ(_variantCache->Count(), 2u);

    // 相同 material 再解析应命中同一变体。
    auto baseAgain = base.ResolveVariant(*_variantCache, passIdx);
    ASSERT_TRUE(baseAgain.HasValue());
    EXPECT_EQ(baseVariant.Get(), baseAgain.Get());
    EXPECT_EQ(_variantCache->Count(), 2u);
}

TEST_P(MaterialVariantIntegrationTest, ApplyPropertiesWritesConstantBuffer) {
    AssetManager mgr;
    auto shaderRef = mgr.AddReady<ShaderAsset>(Guid::NewGuid(), MakeLitShader());
    const auto passIdx = shaderRef->FindPassByTag("ForwardLit").value();

    MaterialAsset material{shaderRef};
    material.SetVector("gMaterial", Eigen::Vector4f{0.25f, 0.5f, 0.75f, 1.0f});
    // 一个 shader 中不存在的 property, 应被 ApplyProperties 忽略 (不计入成功数)。
    material.SetFloat("_DoesNotExist", 3.14f);

    _ctx.ClearCapturedErrors();
    auto variant = material.ResolveVariant(*_variantCache, passIdx);
    ASSERT_TRUE(variant.HasValue()) << _ctx.JoinCapturedErrors();

    string reason;
    auto tableOpt = _ctx.CreateShaderParameterTable(variant.Get()->Layout, &reason);
    ASSERT_TRUE(tableOpt.HasValue()) << fmt::format("CreateShaderParameterTable failed: {}", reason);
    auto table = tableOpt.Release();

    SamplerCache samplerCache{_ctx.GetDevicePtr()};
    const uint32_t applied = material.ApplyProperties(*table, samplerCache);
    // 只有 gMaterial (cbuffer) 应被接受; _DoesNotExist 被忽略。
    EXPECT_EQ(applied, 1u);
}

INSTANTIATE_TEST_SUITE_P(
    RenderBackends,
    MaterialVariantIntegrationTest,
    ::testing::ValuesIn(GetEnabledTestBackends()),
    [](const ::testing::TestParamInfo<TestBackend>& info) {
        return string{fmt::format("{}", info.param)};
    });

}  // namespace
}  // namespace radray::render::test
