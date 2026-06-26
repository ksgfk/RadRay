#include <gtest/gtest.h>

#include <radray/runtime/render/shader.h>
#include <radray/runtime/render/shader_variant_cache.h>
#include <radray/runtime/render/material.h>
#include <radray/runtime/render/renderer.h>

using namespace radray;
using namespace radray::srp;

namespace {

Shader MakeForwardShadowShader() {
    Shader sh{ShaderId{1}, "test"};
    ShaderPassSource fwd;
    fwd.ShaderPath = "test.hlsl";
    fwd.VsEntry = "VSMain";
    fwd.PsEntry = "PSMain";
    fwd.Tags.Tags = {{"LightMode", "UniversalForward"}};
    sh.AddPass(std::move(fwd));

    ShaderPassSource shadow;
    shadow.ShaderPath = "test.hlsl";
    shadow.VsEntry = "VSMain";
    shadow.PsEntry = std::nullopt;  // depth-only
    shadow.Tags.Tags = {{"LightMode", "ShadowCaster"}};
    sh.AddPass(std::move(shadow));
    return sh;
}

}  // namespace

TEST(SrpShaderTest, HasPassAndGetSource) {
    Shader sh = MakeForwardShadowShader();
    EXPECT_TRUE(sh.HasPass("UniversalForward"));
    EXPECT_TRUE(sh.HasPass("ShadowCaster"));
    EXPECT_FALSE(sh.HasPass("DepthOnly"));

    const ShaderPassSource* fwd = sh.GetPassSource("UniversalForward");
    ASSERT_NE(fwd, nullptr);
    ASSERT_TRUE(fwd->PsEntry.has_value());
    EXPECT_EQ(*fwd->PsEntry, "PSMain");

    const ShaderPassSource* shadow = sh.GetPassSource("ShadowCaster");
    ASSERT_NE(shadow, nullptr);
    EXPECT_FALSE(shadow->PsEntry.has_value());  // depth-only,无 PS
}

TEST(SrpShaderTest, ResolveTagByPriority) {
    Shader sh = MakeForwardShadowShader();
    std::string_view out;

    // wanted[0] 命中即返回。
    WantedLightModes wanted{"UniversalForward", "SRPDefaultUnlit"};
    EXPECT_TRUE(sh.ResolveTag(wanted, &out));
    EXPECT_EQ(out, "UniversalForward");

    // 高优先级不命中,落到次优先级。
    WantedLightModes wanted2{"SRPDefaultUnlit", "ShadowCaster"};
    EXPECT_TRUE(sh.ResolveTag(wanted2, &out));
    EXPECT_EQ(out, "ShadowCaster");

    // 全不命中 → relevance 失败。
    WantedLightModes wanted3{"DepthOnly", "Meta"};
    EXPECT_FALSE(sh.ResolveTag(wanted3, &out));
}

TEST(SrpShaderTest, VariantCacheRelevanceMiss) {
    // gpu=nullptr:任何编译都失败,但"shader 没这个 lightMode 的 pass"应在编译前就静默返回 nullptr。
    Shader sh = MakeForwardShadowShader();
    ShaderVariantCache cache{nullptr};
    KeywordSet kw;
    // shader 没有 "DepthOnly" pass → relevance 失败,返回 nullptr,且不应进有效缓存。
    EXPECT_EQ(cache.Get(sh, "DepthOnly", kw), nullptr);
}

namespace {

// 最简 Material/Renderer 桩,验证抽象接口可被实现。
class StubMaterial : public Material {
public:
    explicit StubMaterial(Shader* sh) : _shader(sh) {}
    Shader* GetShader() const override { return _shader; }
    BlendMode GetBlendMode() const override { return BlendMode::Masked; }
    render::DescriptorSet* GetDescriptorSet(render::RootSignature*) const override { return nullptr; }

private:
    Shader* _shader;
};

class StubRenderer : public Renderer {
public:
    explicit StubRenderer(Material* m) : _mat(m) {}
    MeshBatchElement BatchElement() const override { return {}; }
    const render::VertexBufferLayout& GetVertexLayout() const override {
        static render::VertexBufferLayout kEmpty{};
        return kEmpty;
    }
    const Eigen::Matrix4f& WorldMatrix() const override { return _world; }
    Material* GetMaterial() const override { return _mat; }

private:
    Material* _mat;
    Eigen::Matrix4f _world{Eigen::Matrix4f::Identity()};
};

}  // namespace

TEST(SrpDataLayerTest, StubInterfacesWire) {
    Shader sh = MakeForwardShadowShader();
    StubMaterial mat{&sh};
    StubRenderer rend{&mat};

    EXPECT_EQ(rend.GetMaterial(), &mat);
    EXPECT_EQ(mat.GetShader(), &sh);
    EXPECT_EQ(mat.GetBlendMode(), BlendMode::Masked);
    EXPECT_EQ(mat.Queue(), RenderQueue::AlphaTest);  // Masked → AlphaTest 队列
    EXPECT_TRUE(rend.IsVisible());
    EXPECT_TRUE(rend.CastsShadow());
}
