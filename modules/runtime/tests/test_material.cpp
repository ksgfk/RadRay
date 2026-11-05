#include <gtest/gtest.h>

#include <radray/runtime/material.h>

namespace radray {
namespace {

float FloatValue(Nullable<const MaterialParameterValue*> value) { return std::get<float>(*value.Get()); }

TEST(MaterialResourceTest, FindsProgramsByName) {
    MaterialResource resource;
    resource.Programs = {{"Depth", nullptr, nullptr, "VS", nullptr, ""}, {"Color", nullptr, nullptr, "VS", nullptr, "PS"}};
    EXPECT_EQ(resource.FindProgram("Color"), 1u);
    EXPECT_FALSE(resource.FindProgram("Shadow"));
    ASSERT_TRUE(resource.GetProgram(1));
    EXPECT_EQ(resource.GetProgram(1)->PixelEntry, "PS");
    EXPECT_FALSE(resource.GetProgram(2));
}

TEST(MaterialTest, CreateCopiesParameterDeclarations) {
    MaterialResource resource;
    vector<MaterialParameter> parameters{{"Tint", array<float, 4>{1, 0, 0, 1}}, {"Scale", 1.0f}};
    const auto material = Material::Create(resource, parameters);
    ASSERT_TRUE(material);
    parameters[1] = {"Changed", 3.0f};
    EXPECT_EQ(&material->GetResource(), &resource);
    EXPECT_EQ(material->FindParameter("Scale"), 1u);
    EXPECT_FALSE(material->FindParameter("Changed"));
    EXPECT_FLOAT_EQ(FloatValue(material->GetDefaultParameter(1)), 1.0f);
}

TEST(MaterialTest, CreateRejectsEmptyName) {
    MaterialResource resource;
    const array<MaterialParameter, 1> parameters{{{"", 1.0f}}};
    EXPECT_FALSE(Material::Create(resource, parameters));
}

TEST(MaterialTest, CreateRejectsDuplicateNames) {
    MaterialResource resource;
    const array<MaterialParameter, 2> parameters{{{"Scale", 1.0f}, {"Scale", 2.0f}}};
    EXPECT_FALSE(Material::Create(resource, parameters));
}

class MaterialInstanceTest : public testing::Test {
protected:
    MaterialResource Resource;
    const array<MaterialParameter, 3> Parameters{{{"Scale", 1.0f},
                                                  {"Texture", Nullable<render::TextureView*>{nullptr}},
                                                  {"Sampler", Nullable<render::Sampler*>{nullptr}}}};
    Material Definition{*Material::Create(Resource, Parameters)};
};

TEST_F(MaterialInstanceTest, ReadsDefaultsUntilOverridden) {
    MaterialInstance instance{Definition};
    EXPECT_FLOAT_EQ(FloatValue(instance.GetParameter(0)), 1.0f);
    ASSERT_TRUE(instance.SetParameter(0, 2.0f));
    EXPECT_FLOAT_EQ(FloatValue(instance.GetParameter(0)), 2.0f);
    EXPECT_FLOAT_EQ(FloatValue(Definition.GetDefaultParameter(0)), 1.0f);
}

TEST_F(MaterialInstanceTest, OverridesAreIndependentPerInstance) {
    MaterialInstance first{Definition};
    MaterialInstance second{Definition};
    ASSERT_TRUE(first.SetParameter(0, 2.0f));
    EXPECT_FLOAT_EQ(FloatValue(second.GetParameter(0)), 1.0f);
}

TEST_F(MaterialInstanceTest, SetRejectsTypeChangeAndKeepsOverride) {
    MaterialInstance instance{Definition};
    ASSERT_TRUE(instance.SetParameter(0, 2.0f));
    EXPECT_FALSE(instance.SetParameter(0, array<float, 4>{}));
    EXPECT_FLOAT_EQ(FloatValue(instance.GetParameter(0)), 2.0f);
}

TEST_F(MaterialInstanceTest, TextureAndSamplerAreDistinctTypes) {
    MaterialInstance instance{Definition};
    EXPECT_TRUE(instance.SetParameter(1, Nullable<render::TextureView*>{nullptr}));
    EXPECT_FALSE(instance.SetParameter(1, Nullable<render::Sampler*>{nullptr}));
    EXPECT_FALSE(instance.SetParameter(2, Nullable<render::TextureView*>{nullptr}));
}

TEST_F(MaterialInstanceTest, RejectsUnknownIndex) {
    MaterialInstance instance{Definition};
    EXPECT_FALSE(instance.GetParameter(3));
    EXPECT_FALSE(instance.SetParameter(3, 1.0f));
    EXPECT_FALSE(instance.ResetParameter(3));
}

TEST_F(MaterialInstanceTest, ResetRestoresDefault) {
    MaterialInstance instance{Definition};
    ASSERT_TRUE(instance.SetParameter(0, 2.0f));
    EXPECT_TRUE(instance.ResetParameter(0));
    EXPECT_FLOAT_EQ(FloatValue(instance.GetParameter(0)), 1.0f);
}

class MaterialRenderProxyTest : public MaterialInstanceTest {};

TEST_F(MaterialRenderProxyTest, UpdateFromInstanceCopiesResolvedValues) {
    MaterialInstance instance{Definition};
    ASSERT_TRUE(instance.SetParameter(0, 2.0f));
    MaterialRenderProxy proxy;
    proxy.Update(instance);
    EXPECT_EQ(proxy.GetResource().Get(), &Resource);
    EXPECT_FLOAT_EQ(FloatValue(proxy.GetParameter(0)), 2.0f);
    EXPECT_TRUE(std::holds_alternative<Nullable<render::Sampler*>>(*proxy.GetParameter(2).Get()));
}

TEST_F(MaterialRenderProxyTest, IgnoresInstanceEditsUntilNextUpdate) {
    MaterialInstance instance{Definition};
    MaterialRenderProxy proxy;
    proxy.Update(instance);
    ASSERT_TRUE(instance.SetParameter(0, 2.0f));
    EXPECT_FLOAT_EQ(FloatValue(proxy.GetParameter(0)), 1.0f);
    proxy.Update(instance);
    EXPECT_FLOAT_EQ(FloatValue(proxy.GetParameter(0)), 2.0f);
}

TEST_F(MaterialRenderProxyTest, SnapshotOutlivesSourceObjects) {
    MaterialResource resource;
    MaterialRenderProxy proxy;
    {
        const array<MaterialParameter, 1> parameters{{{"Scale", 1.0f}}};
        const auto material = Material::Create(resource, parameters);
        ASSERT_TRUE(material);
        MaterialInstance instance{*material};
        ASSERT_TRUE(instance.SetParameter(0, 0.5f));
        proxy.Update(instance);
    }
    EXPECT_EQ(proxy.GetResource().Get(), &resource);
    EXPECT_FLOAT_EQ(FloatValue(proxy.GetParameter(0)), 0.5f);
}

TEST_F(MaterialRenderProxyTest, UpdateReplacesWholeSnapshot) {
    MaterialResource firstResource;
    MaterialResource secondResource;
    const array<MaterialParameter, 2> firstParameters{{{"Scale", 1.0f}, {"Bias", 0.0f}}};
    const array<MaterialParameter, 1> secondParameters{{{"Glow", 3.0f}}};
    const auto first = Material::Create(firstResource, firstParameters);
    const auto second = Material::Create(secondResource, secondParameters);
    ASSERT_TRUE(first && second);
    MaterialRenderProxy proxy;
    proxy.Update(*first);
    proxy.Update(*second);
    EXPECT_EQ(proxy.GetResource().Get(), &secondResource);
    EXPECT_FLOAT_EQ(FloatValue(proxy.GetParameter(0)), 3.0f);
    EXPECT_FALSE(proxy.GetParameter(1));
}

}  // namespace
}  // namespace radray
