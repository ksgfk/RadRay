#include "render_test_framework.h"

#include <gtest/gtest.h>
#include <fmt/format.h>

#include <radray/guid.h>
#include <radray/runtime/shader_variant_library.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>

namespace radray::render::test {
namespace {

// 一个带 keyword 与 material cbuffer 的最小 VS+PS pass。
// _TINT keyword 影响 PS 输出; MaterialParams 使用 Forward ABI 的 group 2。
constexpr std::string_view kLitPassSource = R"(
struct MaterialParams {
    float4 BaseColor;
};
[[vk::binding(0, 2)]] ConstantBuffer<MaterialParams> gMaterial : register(b0, space2);

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
        _layoutLibrary = make_unique<PipelineLayoutLibrary>(_ctx.GetDevicePtr());
        _cache = CreateShaderVariantLibrary(
            _ctx.GetDevicePtr(), _ctx.GetDxc(), _layoutLibrary.get());
        ASSERT_TRUE(_cache.HasValue());
        _variantCache = _cache.Release();
    }

    static unique_ptr<ShaderAsset> MakeLitShader() {
        return MakeLitShader(string{kLitPassSource});
    }

    static unique_ptr<ShaderAsset> MakeLitShader(string source) {
        ShaderKeywordSet kw;
        kw.Add("_TINT");  // bit 0
        vector<ShaderPassDesc> passes;
        passes.push_back(ShaderPassDesc{.PassTag = "ForwardLit", .Source = std::move(source)});
        return std::make_unique<ShaderAsset>(std::move(kw), std::move(passes));
    }

    ComputeTestContext _ctx{};
    unique_ptr<PipelineLayoutLibrary> _layoutLibrary{};
    Nullable<unique_ptr<ShaderVariantLibrary>> _cache{};
    unique_ptr<ShaderVariantLibrary> _variantCache{};
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
    EXPECT_EQ(baseVariant.Get()->Layout, tintedVariant.Get()->Layout);
    EXPECT_EQ(_layoutLibrary->Count(), 1u);
    EXPECT_EQ(_variantCache->Count(), 2u);

    // 相同 material 再解析应命中同一变体。
    auto baseAgain = base.ResolveVariant(*_variantCache, passIdx);
    ASSERT_TRUE(baseAgain.HasValue());
    EXPECT_EQ(baseVariant.Get(), baseAgain.Get());
    EXPECT_EQ(_variantCache->Count(), 2u);
}

TEST_P(MaterialVariantIntegrationTest, CanonicalLayoutDoesNotDependOnBindingNames) {
    AssetManager mgr;
    auto originalShader = mgr.AddReady<ShaderAsset>(Guid::NewGuid(), MakeLitShader());
    string renamedSource{kLitPassSource};
    constexpr std::string_view oldName = "gMaterial";
    constexpr std::string_view newName = "gRenamedMaterial";
    for (size_t offset = 0; (offset = renamedSource.find(oldName, offset)) != string::npos;) {
        renamedSource.replace(offset, oldName.size(), newName);
        offset += newName.size();
    }
    auto renamedShader = mgr.AddReady<ShaderAsset>(
        Guid::NewGuid(), MakeLitShader(std::move(renamedSource)));

    MaterialAsset original{originalShader};
    MaterialAsset renamed{renamedShader};
    auto originalVariant = original.ResolveVariant(*_variantCache, 0);
    auto renamedVariant = renamed.ResolveVariant(*_variantCache, 0);
    ASSERT_TRUE(originalVariant.HasValue()) << _ctx.JoinCapturedErrors();
    ASSERT_TRUE(renamedVariant.HasValue()) << _ctx.JoinCapturedErrors();
    EXPECT_EQ(originalVariant.Get()->Layout, renamedVariant.Get()->Layout);
    EXPECT_EQ(_layoutLibrary->Count(), 1u);
    EXPECT_EQ(
        FindShaderBindingLocation(*originalVariant.Get(), oldName),
        (ShaderBindingLocation{.Group = 2, .Binding = 0}));
    EXPECT_EQ(
        FindShaderBindingLocation(*renamedVariant.Get(), newName),
        (ShaderBindingLocation{.Group = 2, .Binding = 0}));
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
