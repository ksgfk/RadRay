#include <gtest/gtest.h>

#include <radray/runtime/material_asset.h>
#include <radray/runtime/render_framework/material_property_block.h>

using namespace radray;

namespace {

ShaderAsset MakeShaderWithKeywords() {
    ShaderKeywordSet kw;
    kw.Add("_NORMALMAP");   // bit 0
    kw.Add("_EMISSION");    // bit 1
    kw.Add("_ALPHATEST");   // bit 2
    vector<ShaderPassDesc> passes;
    passes.push_back(ShaderPassDesc{.PassTag = "ForwardLit", .Source = ""});
    return ShaderAsset{std::move(kw), std::move(passes)};
}

}  // namespace

TEST(MaterialAssetTest, GetTypeIdAndProgramIndependence) {
    MaterialAsset m;
    EXPECT_EQ(m.GetTypeId(), runtime_type_id_v<MaterialAsset>);
    EXPECT_FALSE(m.GetTypeId().IsEmpty());
    EXPECT_EQ(m.GetShader().Get(), nullptr);
}

TEST(MaterialAssetTest, SetAndGetFloat) {
    MaterialAsset m;
    m.SetFloat("_Metallic", 0.75f);
    auto v = m.GetFloat("_Metallic");
    ASSERT_TRUE(v.has_value());
    EXPECT_FLOAT_EQ(v.value(), 0.75f);
    EXPECT_FALSE(m.GetFloat("_Missing").has_value());
}

TEST(MaterialAssetTest, SetAndGetVector) {
    MaterialAsset m;
    Eigen::Vector4f color{0.1f, 0.2f, 0.3f, 1.0f};
    m.SetVector("_BaseColor", color);
    auto v = m.GetVector("_BaseColor");
    ASSERT_TRUE(v.has_value());
    EXPECT_TRUE(v.value().isApprox(color));
    // 类型不匹配的 getter 返回 nullopt。
    EXPECT_FALSE(m.GetFloat("_BaseColor").has_value());
}

TEST(MaterialAssetTest, SetFloatOverwrites) {
    MaterialAsset m;
    m.SetFloat("_X", 1.0f);
    m.SetFloat("_X", 2.0f);
    EXPECT_FLOAT_EQ(m.GetFloat("_X").value(), 2.0f);
    EXPECT_EQ(m.GetProperties().size(), 1u);
}

TEST(MaterialAssetTest, SettingDifferentTypeReplacesValue) {
    MaterialAsset m;
    m.SetFloat("_P", 3.0f);
    m.SetVector("_P", Eigen::Vector4f{1, 1, 1, 1});
    EXPECT_FALSE(m.GetFloat("_P").has_value());
    EXPECT_TRUE(m.GetVector("_P").has_value());
    EXPECT_EQ(m.GetProperties().size(), 1u);
}

TEST(MaterialAssetTest, KeywordEnableDisable) {
    MaterialAsset m;
    EXPECT_FALSE(m.IsKeywordEnabled("_NORMALMAP"));

    m.EnableKeyword("_NORMALMAP");
    m.EnableKeyword("_EMISSION");
    EXPECT_TRUE(m.IsKeywordEnabled("_NORMALMAP"));
    EXPECT_TRUE(m.IsKeywordEnabled("_EMISSION"));
    ASSERT_EQ(m.GetEnabledKeywords().size(), 2u);
    EXPECT_EQ(m.GetEnabledKeywords()[0], "_NORMALMAP");
    EXPECT_EQ(m.GetEnabledKeywords()[1], "_EMISSION");

    // 重复启用是幂等的。
    m.EnableKeyword("_NORMALMAP");
    EXPECT_EQ(m.GetEnabledKeywords().size(), 2u);
}

TEST(MaterialAssetTest, KeywordDisableMaintainsOrder) {
    MaterialAsset m;
    m.EnableKeyword("A");
    m.EnableKeyword("B");
    m.EnableKeyword("C");
    m.DisableKeyword("B");

    ASSERT_EQ(m.GetEnabledKeywords().size(), 2u);
    EXPECT_EQ(m.GetEnabledKeywords()[0], "A");
    EXPECT_EQ(m.GetEnabledKeywords()[1], "C");
    EXPECT_FALSE(m.IsKeywordEnabled("B"));

    // 删除后再启用一个新 keyword, 索引应正确追加。
    m.EnableKeyword("D");
    ASSERT_EQ(m.GetEnabledKeywords().size(), 3u);
    EXPECT_EQ(m.GetEnabledKeywords()[2], "D");
    // 再删首个, 剩余顺序保持。
    m.DisableKeyword("A");
    ASSERT_EQ(m.GetEnabledKeywords().size(), 2u);
    EXPECT_EQ(m.GetEnabledKeywords()[0], "C");
    EXPECT_EQ(m.GetEnabledKeywords()[1], "D");
}

TEST(MaterialAssetTest, DisableUnknownKeywordIsNoop) {
    MaterialAsset m;
    m.EnableKeyword("A");
    m.DisableKeyword("NOPE");
    EXPECT_EQ(m.GetEnabledKeywords().size(), 1u);
    EXPECT_TRUE(m.IsKeywordEnabled("A"));
}

TEST(MaterialAssetTest, EnabledKeywordsProjectThroughShader) {
    ShaderAsset shader = MakeShaderWithKeywords();
    MaterialAsset m;
    m.EnableKeyword("_ALPHATEST");  // shader bit 2
    m.EnableKeyword("_NORMALMAP");  // shader bit 0
    m.EnableKeyword("_UNDECLARED"); // 不在 shader keyword 表, 应被投影忽略

    // 通过 shader 的 keyword 表投影验证 material 记录的启用集是否正确参与投影。
    vector<std::string_view> enabled;
    for (const string& kw : m.GetEnabledKeywords()) {
        enabled.emplace_back(kw);
    }
    const uint64_t mask = shader.GetKeywords().Project(enabled);
    EXPECT_EQ(mask, 0b101ull);  // bit 0 + bit 2

    auto defines = shader.GetKeywords().ResolveDefines(mask);
    ASSERT_EQ(defines.size(), 2u);
    EXPECT_EQ(defines[0], "_NORMALMAP=1");
    EXPECT_EQ(defines[1], "_ALPHATEST=1");
}

TEST(MaterialAssetTest, BindingKeyUsesSnapshotValuesNotAllocationAddress) {
    MaterialAsset first;
    first.SetFloat("Roughness", 0.4f);
    first.SetVector("BaseColor", Eigen::Vector4f{0.1f, 0.2f, 0.3f, 1.0f});

    MaterialAsset sameValuesDifferentInsertionOrder;
    sameValuesDifferentInsertionOrder.SetVector(
        "BaseColor", Eigen::Vector4f{0.1f, 0.2f, 0.3f, 1.0f});
    sameValuesDifferentInsertionOrder.SetFloat("Roughness", 0.4f);

    const auto firstSnapshot = first.CreateSnapshot();
    const auto secondSnapshot = sameValuesDifferentInsertionOrder.CreateSnapshot();
    ASSERT_NE(firstSnapshot.get(), secondSnapshot.get());
    EXPECT_EQ(firstSnapshot->BindingKey, secondSnapshot->BindingKey);

    sameValuesDifferentInsertionOrder.SetFloat("Roughness", 0.8f);
    const auto changedSnapshot = sameValuesDifferentInsertionOrder.CreateSnapshot();
    EXPECT_NE(firstSnapshot->BindingKey, changedSnapshot->BindingKey);
}

TEST(MaterialAssetTest, ResolvesShaderDefaultThenOverrideAndClear) {
    AssetManager assets;
    vector<ShaderPropertyDesc> properties{
        ShaderPropertyDesc{
            .Name = "Roughness",
            .Kind = ShaderPropertyKind::Float,
            .DefaultValue = 0.35f}};
    auto shader = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>(ShaderKeywordSet{}, vector<ShaderPassDesc>{}, std::move(properties)));
    MaterialAsset material{shader};

    EXPECT_TRUE(material.HasProperty("Roughness"));
    EXPECT_FALSE(material.GetOverride("Roughness").has_value());
    ASSERT_TRUE(material.GetFloat("Roughness").has_value());
    EXPECT_FLOAT_EQ(*material.GetFloat("Roughness"), 0.35f);

    material.SetFloat("Roughness", 0.8f);
    ASSERT_TRUE(material.GetOverride("Roughness").has_value());
    EXPECT_FLOAT_EQ(*material.GetFloat("Roughness"), 0.8f);
    material.ClearOverride("Roughness");
    EXPECT_FALSE(material.GetOverride("Roughness").has_value());
    EXPECT_FLOAT_EQ(*material.GetFloat("Roughness"), 0.35f);
}

TEST(MaterialAssetTest, RevisionChangesOnlyForSemanticMutations) {
    AssetManager assets;
    auto shader = assets.AddReady<ShaderAsset>(
        Guid::NewGuid(),
        make_unique<ShaderAsset>());
    auto texture = assets.AddReady<TextureAsset>(
        Guid::NewGuid(),
        make_unique<TextureAsset>());
    MaterialAsset material{shader};
    const uint64_t initial = material.GetRevision();

    material.SetFloat("Value", 1.0f);
    const uint64_t propertyRevision = material.GetRevision();
    EXPECT_GT(propertyRevision, initial);
    material.SetFloat("Value", 1.0f);
    material.ClearOverride("Missing");
    material.DisableKeyword("MISSING");
    material.SetRenderQueue(material.GetRenderQueue());
    material.SetRenderState(material.GetRenderState());
    EXPECT_EQ(material.GetRevision(), propertyRevision);

    material.EnableKeyword("FEATURE");
    const uint64_t keywordRevision = material.GetRevision();
    EXPECT_GT(keywordRevision, propertyRevision);
    material.EnableKeyword("FEATURE");
    EXPECT_EQ(material.GetRevision(), keywordRevision);
    material.DisableKeyword("FEATURE");
    EXPECT_GT(material.GetRevision(), keywordRevision);

    const uint64_t beforeQueue = material.GetRevision();
    material.SetRenderQueue(RenderQueue::Transparent);
    EXPECT_GT(material.GetRevision(), beforeQueue);
    const uint64_t queueRevision = material.GetRevision();
    material.SetRenderQueue(RenderQueue::Transparent);
    EXPECT_EQ(material.GetRevision(), queueRevision);

    MaterialRenderState state{};
    state.Cull = render::CullMode::None;
    material.SetRenderState(state);
    const uint64_t stateRevision = material.GetRevision();
    EXPECT_GT(stateRevision, queueRevision);
    material.SetRenderState(state);
    EXPECT_EQ(material.GetRevision(), stateRevision);

    material.SetTexture("Texture", texture);
    const uint64_t textureRevision = material.GetRevision();
    EXPECT_GT(textureRevision, stateRevision);
    material.SetTexture("Texture", texture);
    EXPECT_EQ(material.GetRevision(), textureRevision);

    render::SamplerDescriptor sampler{};
    sampler.AddressS = render::AddressMode::ClampToEdge;
    material.SetSampler("Sampler", sampler);
    const uint64_t samplerRevision = material.GetRevision();
    EXPECT_GT(samplerRevision, textureRevision);
    material.SetSampler("Sampler", sampler);
    EXPECT_EQ(material.GetRevision(), samplerRevision);

    material.SetShader(shader);
    EXPECT_EQ(material.GetRevision(), samplerRevision);
}

TEST(MaterialAssetTest, ShaderSwitchPreservesOverridesAndChangesDefaults) {
    AssetManager assets;
    auto makeShader = [&](float value) {
        vector<ShaderPropertyDesc> properties{ShaderPropertyDesc{
            .Name = "Value",
            .Kind = ShaderPropertyKind::Float,
            .DefaultValue = value}};
        return assets.AddReady<ShaderAsset>(
            Guid::NewGuid(),
            make_unique<ShaderAsset>(ShaderKeywordSet{}, vector<ShaderPassDesc>{}, std::move(properties)));
    };
    auto first = makeShader(1.0f);
    auto second = makeShader(2.0f);
    MaterialAsset material{first};
    material.SetFloat("Value", 3.0f);
    const uint64_t beforeSwitch = material.GetRevision();

    material.SetShader(second);
    EXPECT_GT(material.GetRevision(), beforeSwitch);
    EXPECT_FLOAT_EQ(*material.GetFloat("Value"), 3.0f);
    material.ClearOverride("Value");
    EXPECT_FLOAT_EQ(*material.GetFloat("Value"), 2.0f);
}

TEST(MaterialAssetTest, BindingKeyPreservesMaterialAndPropertyBlockLayerOrder) {
    MaterialAsset first;
    first.SetFloat("Value", 1.0f);
    MaterialPropertyBlock firstBlock;
    firstBlock.SetFloat("Value", 2.0f);

    MaterialAsset second;
    second.SetFloat("Value", 2.0f);
    MaterialPropertyBlock secondBlock;
    secondBlock.SetFloat("Value", 1.0f);

    EXPECT_NE(
        first.CreateSnapshot(&firstBlock)->BindingKey,
        second.CreateSnapshot(&secondBlock)->BindingKey);
}
