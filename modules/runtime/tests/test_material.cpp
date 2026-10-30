#include <gtest/gtest.h>

#include <limits>

#include <fmt/format.h>
#include <radray/runtime/scene.h>

namespace radray {
namespace {

TEST(MaterialSystem, DeclarationOwnsDefaultsAndRejectsAmbiguousNames) {
    MaterialResource resource;
    array<MaterialParameter, 2> parameters{{{"Tint", array<float, 4>{1, 0, 0, 1}}, {"Scale", 1.0f}}};
    const auto material = Material::Create(resource, parameters);
    ASSERT_TRUE(material);
    parameters[0].Name = "Changed";
    parameters[1].DefaultValue = 3.0f;
    EXPECT_EQ(material->FindParameter("Tint"), 0u);
    EXPECT_EQ(material->FindParameter("Scale"), 1u);
    EXPECT_FALSE(material->FindParameter("Changed"));
    EXPECT_FALSE(material->GetDefaultParameter(2));
    EXPECT_FLOAT_EQ(std::get<float>(*material->GetDefaultParameter(1)), 1.0f);
    EXPECT_EQ(&material->GetResource(), &resource);

    parameters[1].Name = parameters[0].Name;
    EXPECT_FALSE(Material::Create(resource, parameters));
    parameters[1].Name.clear();
    EXPECT_FALSE(Material::Create(resource, parameters));
    EXPECT_TRUE(Material::Create(resource, {}));
}

TEST(MaterialSystem, InstancesInheritAndOverrideIndependently) {
    MaterialResource resource;
    const array<MaterialParameter, 1> parameters{{{"Scale", 1.0f}}};
    const auto material = Material::Create(resource, parameters);
    ASSERT_TRUE(material);
    MaterialInstance first{*material};
    MaterialInstance second{*material};
    EXPECT_EQ(&first.GetMaterial(), &*material);
    EXPECT_FLOAT_EQ(std::get<float>(*first.GetParameter(0)), 1.0f);
    ASSERT_TRUE(first.SetParameter(0, 2.0f));
    EXPECT_FLOAT_EQ(std::get<float>(*first.GetParameter(0)), 2.0f);
    EXPECT_FLOAT_EQ(std::get<float>(*second.GetParameter(0)), 1.0f);
    EXPECT_FLOAT_EQ(std::get<float>(*material->GetDefaultParameter(0)), 1.0f);
    EXPECT_FALSE(first.SetParameter(0, array<float, 4>{}));
    EXPECT_FALSE(first.SetParameter(1, 7.0f));
    EXPECT_FALSE(first.ResetParameter(1));
    EXPECT_FALSE(first.GetParameter(1));
    EXPECT_FLOAT_EQ(std::get<float>(*first.GetParameter(0)), 2.0f);
    EXPECT_TRUE(first.ResetParameter(0));
    EXPECT_FLOAT_EQ(std::get<float>(*first.GetParameter(0)), 1.0f);
}

TEST(MaterialSystem, TextureAndSamplerAreDistinctParameterTypes) {
    MaterialResource resource;
    const array<MaterialParameter, 2> parameters{{{"Texture", Nullable<render::TextureView*>{nullptr}},
                                                  {"Sampler", Nullable<render::Sampler*>{nullptr}}}};
    const auto material = Material::Create(resource, parameters);
    ASSERT_TRUE(material);
    MaterialInstance instance{*material};
    EXPECT_TRUE(instance.SetParameter(0, Nullable<render::TextureView*>{nullptr}));
    EXPECT_FALSE(instance.SetParameter(0, Nullable<render::Sampler*>{nullptr}));
    MaterialRenderProxy proxy;
    proxy.Update(instance);
    EXPECT_TRUE(std::holds_alternative<Nullable<render::TextureView*>>(*proxy.GetParameter(0)));
    EXPECT_TRUE(std::holds_alternative<Nullable<render::Sampler*>>(*proxy.GetParameter(1)));
    EXPECT_FALSE(std::get<Nullable<render::TextureView*>>(*proxy.GetParameter(0)));
}

TEST(MaterialSystem, ProxyChangesOnlyOnExplicitSynchronization) {
    MaterialResource resource;
    const array<MaterialParameter, 1> parameters{{{"Scale", 1.0f}}};
    const auto material = Material::Create(resource, parameters);
    ASSERT_TRUE(material);
    MaterialInstance instance{*material};
    MaterialRenderProxy proxy;
    EXPECT_FALSE(proxy.GetResource());
    EXPECT_FALSE(proxy.GetParameter(0));
    proxy.Update(instance);
    EXPECT_EQ(proxy.GetResource().Get(), &resource);
    ASSERT_TRUE(instance.SetParameter(0, 2.0f));
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 1.0f);
    proxy.Update(instance);
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 2.0f);
    ASSERT_TRUE(instance.ResetParameter(0));
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 2.0f);
    proxy.Update(instance);
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 1.0f);
}

TEST(MaterialSystem, ProxyOutlivesSourceAndDropsOldParametersWhenRebound) {
    MaterialResource firstResource;
    MaterialResource secondResource;
    MaterialRenderProxy proxy;
    {
        const array<MaterialParameter, 2> parameters{{{"Scale", 1.0f}, {"Bias", 0.0f}}};
        const auto material = Material::Create(firstResource, parameters);
        ASSERT_TRUE(material);
        MaterialInstance instance{*material};
        ASSERT_TRUE(instance.SetParameter(1, 0.5f));
        proxy.Update(instance);
    }
    EXPECT_EQ(proxy.GetResource().Get(), &firstResource);
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(1)), 0.5f);
    const array<MaterialParameter, 1> parameters{{{"NewScale", 3.0f}}};
    const auto replacement = Material::Create(secondResource, parameters);
    ASSERT_TRUE(replacement);
    proxy.Update(*replacement);
    EXPECT_EQ(proxy.GetResource().Get(), &secondResource);
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 3.0f);
    EXPECT_FALSE(proxy.GetParameter(1));
}

TEST(MaterialSystem, SceneHandlesSurviveSnapshotRelocation) {
    MaterialResource resource;
    const array<MaterialParameter, 1> parameters{{{"Scale", 1.0f}}};
    const auto material = Material::Create(resource, parameters);
    ASSERT_TRUE(material);
    MaterialInstance instance{*material};
    Scene scene;
    const auto first = scene.Materials.Emplace();
    for (size_t index = 0; index < 64; ++index) scene.Materials.Emplace();
    const auto retained = scene.Materials.Emplace();
    scene.Materials.Get(retained).Update(instance);
    scene.Materials.Reserve(512);
    scene.Materials.Destroy(first);
    ASSERT_TRUE(scene.Materials.IsAlive(retained));
    EXPECT_FLOAT_EQ(std::get<float>(*scene.Materials.Get(retained).GetParameter(0)), 1.0f);
    EXPECT_EQ(scene.Materials.Get(retained).GetResource().Get(), &resource);
}

TEST(MaterialSystem, ProgramNamesResolveWithinOneResource) {
    MaterialTagTable tags;
    const auto surface = tags.Intern("Surface");
    const auto opaque = tags.Intern("Opaque");
    ASSERT_TRUE(surface && opaque);
    MaterialResource resource;
    resource.Properties.Tags = MaterialTagSet{tags};
    ASSERT_TRUE(resource.Properties.Tags.Set(*surface, *opaque));
    resource.Properties.RenderState.Cull = render::CullMode::None;
    resource.Properties.RenderState.Stencil = render::StencilState::Default();
    resource.Properties.RenderState.StencilReference = 7;
    resource.Programs = {{"Depth", nullptr, nullptr, "VS", nullptr, ""},
                         {"Color", nullptr, nullptr, "VS", nullptr, "PS"}};
    EXPECT_EQ(resource.FindProgram("Depth"), 0u);
    EXPECT_EQ(resource.FindProgram("Color"), 1u);
    EXPECT_FALSE(resource.FindProgram("Shadow"));
    ASSERT_TRUE(resource.GetProgram(1));
    EXPECT_EQ(resource.GetProgram(1)->PixelEntry, "PS");
    EXPECT_FALSE(resource.GetProgram(2));
    const auto copy = resource;
    EXPECT_TRUE(copy.Properties.Tags.Matches(*surface, *opaque));
    EXPECT_EQ(copy.Properties.Tags.GetTable().Get(), &tags);
    EXPECT_EQ(copy.Properties.RenderState.Cull, render::CullMode::None);
    ASSERT_TRUE(copy.Properties.RenderState.Stencil);
    EXPECT_EQ(copy.Properties.RenderState.StencilReference, 7);
}

TEST(MaterialTags, InternOwnsTextDeduplicatesAndDistinguishesCase) {
    MaterialTagTable table;
    EXPECT_FALSE(table.Intern(""));
    EXPECT_FALSE(table.Find(""));
    EXPECT_FALSE(table.GetText({}));
    EXPECT_FALSE(table.GetText({1}));
    string source = "Surface";
    const auto surface = table.Intern(source);
    ASSERT_TRUE(surface);
    source[0] = 'X';
    EXPECT_EQ(table.Intern("Surface"), surface);
    EXPECT_EQ(table.Find("Surface"), surface);
    EXPECT_NE(table.Intern("surface"), surface);
    EXPECT_EQ(table.GetText(*surface), "Surface");
    EXPECT_FALSE(table.Find(source));
    EXPECT_FALSE(table.GetText({std::numeric_limits<uint32_t>::max()}));
}

TEST(MaterialTags, IdsSurvivePoolAndSortedIndexGrowth) {
    MaterialTagTable table;
    vector<MaterialTagId> ids;
    for (int i = 99; i >= 0; --i) {
        const auto id = table.Intern(fmt::format("tag-{:03}-long-enough-to-grow-the-pool", i));
        ASSERT_TRUE(id);
        ids.push_back(*id);
    }
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto text = fmt::format("tag-{:03}-long-enough-to-grow-the-pool", 99 - i);
        EXPECT_EQ(table.GetText(ids[i]), text);
        EXPECT_EQ(table.Find(text), ids[i]);
        EXPECT_EQ(table.Intern(text), ids[i]);
    }
    EXPECT_FALSE(table.Find("tag-050"));
    EXPECT_FALSE(table.Find("zzz"));
}

TEST(MaterialTags, InternAcceptsSubstringsOfItsOwnPool) {
    MaterialTagTable table;
    const auto first = table.Intern("SurfaceOpaque");
    ASSERT_TRUE(first);
    const auto view = table.GetText(*first);
    ASSERT_TRUE(view);
    const auto suffix = table.Intern(view->substr(7));
    ASSERT_TRUE(suffix);
    EXPECT_EQ(table.GetText(*suffix), "Opaque");
    EXPECT_EQ(table.GetText(*first), "SurfaceOpaque");
}

TEST(MaterialTags, SetsReplaceRejectInvalidIdsAndOwnTheirPairs) {
    MaterialTagTable table;
    const auto opaque = table.Intern("Opaque");
    const auto masked = table.Intern("Masked");
    ASSERT_TRUE(opaque && masked);
    MaterialTagSet unbound;
    EXPECT_FALSE(unbound.Set(*opaque, *masked));
    EXPECT_FALSE(unbound.Find(*opaque));
    EXPECT_FALSE(unbound.Matches({}, {}));
    MaterialTagSet tags{table};
    vector<MaterialTagId> keys;
    for (int i = 0; i < 9; ++i) {
        const auto key = table.Intern(fmt::format("Key{}", i));
        ASSERT_TRUE(key);
        keys.push_back(*key);
        ASSERT_TRUE(tags.Set(*key, *opaque));
        if (i == 2) {
            auto inlineCopy = tags;
            ASSERT_TRUE(inlineCopy.Set(keys[0], *masked));
            EXPECT_TRUE(tags.Matches(keys[0], *opaque));
            EXPECT_TRUE(inlineCopy.Matches(keys[0], *masked));
        }
    }
    auto copy = tags;
    ASSERT_TRUE(copy.Set(keys[0], *masked));
    EXPECT_EQ(copy.GetTags().size(), keys.size());
    EXPECT_TRUE(tags.Matches(keys[0], *opaque));
    EXPECT_FALSE(copy.Matches(keys[0], *opaque));
    EXPECT_EQ(copy.Find(keys[0]), masked);
    EXPECT_FALSE(copy.Find(*masked));
    EXPECT_FALSE(copy.Set({}, *opaque));
    EXPECT_FALSE(copy.Set(keys[0], {}));
    EXPECT_FALSE(copy.Set({std::numeric_limits<uint32_t>::max()}, *opaque));
    EXPECT_FALSE(copy.Set(keys[0], {std::numeric_limits<uint32_t>::max()}));
    EXPECT_EQ(copy.Find(keys[0]), masked);
    MaterialTagSet assigned;
    assigned = copy;
    auto moved = std::move(copy);
    EXPECT_EQ(moved.GetTable().Get(), &table);
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto expected = i == 0 ? *masked : *opaque;
        EXPECT_TRUE(moved.Matches(keys[i], expected));
        EXPECT_TRUE(assigned.Matches(keys[i], expected));
    }
}

}  // namespace
}  // namespace radray
