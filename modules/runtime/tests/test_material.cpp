#include <gtest/gtest.h>
#include <cstring>
#include "test_asset_fakes.h"

namespace radray {
namespace {
using namespace test;
class MaterialAssetTest : public testing::Test {
protected:
    LayoutDevice Device;
    AssetManager Assets{Device};
    AssetHandle<ShaderAsset> Shader;
    MaterialAssetDesc Desc;
    void SetUp() override {
        const auto shader = Assets.CreateShader(MakeShaderDesc(MaterialLayout()));
        ASSERT_TRUE(shader);
        Shader = *shader;
        Desc = MakeMaterialDesc(Shader, {{"Roughness", 0.25f}, {"Metallic", 0.75f}, {"Tint", array<float, 4>{1, 2, 3, 4}}},
                                {"Roughness", "Tint"}, "Constants");
    }
};
TEST_F(MaterialAssetTest, PreparesSubsetWithoutCreatingNativeLayouts) {
    const auto handle = Assets.CreateMaterial(Desc);
    ASSERT_TRUE(handle);
    const auto material = Assets.Get(*handle);
    EXPECT_EQ(material->FindProgram("Depth"), 1u);
    EXPECT_FALSE(material->FindProgram("Missing"));
    EXPECT_FALSE(material->GetProgram(2));
    const auto map = material->GetBindingMap(Shader);
    ASSERT_TRUE(map);
    ASSERT_EQ(map->GetConstantTargets().size(), 2u);
    EXPECT_EQ(map->GetConstantTargets()[1].ParameterIndex, 2u);
    EXPECT_EQ(Device.GroupCreates, 0u);
    EXPECT_EQ(Device.LayoutCreates, 0u);
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(Assets, *handle));
    array<byte, 32> bytes;
    bytes.fill(byte{0x5a});
    ASSERT_TRUE(map->WriteConstants(proxy, bytes));
    float roughness = 0;
    std::memcpy(&roughness, bytes.data(), 4);
    EXPECT_FLOAT_EQ(roughness, 0.25f);
    EXPECT_EQ(bytes[4], byte{0x5a});
    EXPECT_EQ(bytes[8], byte{0x5a});
    EXPECT_FALSE(map->WriteConstants(proxy, std::span{bytes}.first(16)));
}
TEST_F(MaterialAssetTest, RejectsInvalidExplicitSubsetsAndPrograms) {
    auto check = [&](auto edit) { auto desc = Desc; edit(desc); EXPECT_FALSE(Assets.CreateMaterial(desc)); };
    check([](auto& d) { d.Families.clear(); });
    check([](auto& d) { d.Families.push_back(d.Families[0]); });
    check([](auto& d) { d.Families[0].Shader = {}; });
    check([](auto& d) { d.Families[0].Parameters.push_back("Unknown"); });
    check([](auto& d) { d.Families[0].Parameters.push_back("Tint"); });
    check([](auto& d) { d.Parameters[2].Name = "MissingInShader"; d.Families[0].Parameters[1] = "MissingInShader"; });
    check([](auto& d) { d.Parameters[2].DefaultValue = 1.0f; });
    check([](auto& d) { d.Families[0].ConstantBlockName.clear(); });
    check([](auto& d) { d.Parameters[0].Name.clear(); });
    check([](auto& d) { d.Parameters.push_back(d.Parameters[0]); });
    check([](auto& d) { d.Programs[0].VariantIndex = 3; });
    check([](auto& d) { d.Programs[0].VertexEntry = "Wrong"; });
    check([](auto& d) { d.Programs[0].PixelEntry = "Wrong"; });
    check([](auto& d) { d.Programs[1].Name = d.Programs[0].Name; });
    Desc.Families[0].Parameters.clear();
    Desc.Families[0].ConstantBlockName.clear();
    EXPECT_TRUE(Assets.CreateMaterial(Desc));
}
TEST_F(MaterialAssetTest, DifferentFamiliesUseIndependentExplicitSubsets) {
    const auto second = Assets.CreateShader(MakeShaderDesc(ResourceLayout()));
    ASSERT_TRUE(second);
    Desc.Parameters.push_back({"First", Nullable<gpu::TextureView*>{nullptr}});
    Desc.Programs.push_back({"Other", *second, 0, "VS", "PS"});
    Desc.Families.push_back({*second, {}, {"First"}});
    const auto handle = Assets.CreateMaterial(Desc);
    ASSERT_TRUE(handle);
    const auto material = Assets.Get(*handle);
    EXPECT_EQ(material->GetBindingMap(Shader)->GetConstantTargets().size(), 2u);
    EXPECT_TRUE(material->GetBindingMap(*second)->GetConstantTargets().empty());
    EXPECT_EQ(material->GetBindingMap(*second)->GetResourceTargets().size(), 1u);
}
TEST_F(MaterialAssetTest, InstanceOverridesAndSnapshotsAreIndependent) {
    const auto handle = Assets.CreateMaterial(Desc);
    ASSERT_TRUE(handle);
    MaterialInstance first{*handle}, second{*handle};
    ASSERT_TRUE(first.SetParameter(Assets, 0, 0.5f));
    EXPECT_FALSE(first.SetParameter(Assets, 0, array<float, 4>{}));
    EXPECT_FALSE(first.SetParameter(Assets, 99, 0.0f));
    EXPECT_FALSE(first.GetParameter(Assets, 99));
    EXPECT_FALSE(first.ResetParameter(Assets, 99));
    EXPECT_FLOAT_EQ(std::get<float>(*second.GetParameter(Assets, 0)), 0.25f);
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(Assets, first));
    ASSERT_TRUE(first.ResetParameter(Assets, 0));
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 0.5f);
    EXPECT_FLOAT_EQ(std::get<float>(*first.GetParameter(Assets, 0)), 0.25f);
    EXPECT_FALSE(proxy.Update(Assets, AssetHandle<MaterialAsset>{}));
    EXPECT_EQ(proxy.GetMaterial(), *handle);
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 0.5f);
    ASSERT_TRUE(proxy.Update(Assets, first));
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 0.25f);
}
TEST_F(MaterialAssetTest, HandlesAndMappingsSurviveRegistryGrowth) {
    const auto first = Assets.CreateMaterial(Desc);
    ASSERT_TRUE(first);
    MaterialInstance instance{*first};
    for (size_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(Assets.CreateShader(MakeShaderDesc()));
        ASSERT_TRUE(Assets.CreateMaterial(Desc));
    }
    EXPECT_FLOAT_EQ(std::get<float>(*instance.GetParameter(Assets, 0)), 0.25f);
    EXPECT_TRUE(Assets.Get(*first)->GetBindingMap(Shader));
    EXPECT_TRUE(Assets.Get(Shader)->FindBinding("Constants"));
}
TEST_F(MaterialAssetTest, DebugRejectsDifferentMaterialIdentity) {
#if RADRAY_IS_DEBUG
    const auto first = Assets.CreateMaterial(Desc), second = Assets.CreateMaterial(Desc);
    ASSERT_TRUE(first && second);
    const auto map = Assets.Get(*first)->GetBindingMap(Shader);
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(Assets, *second));
    array<byte, 32> bytes{};
    EXPECT_DEATH(map->WriteConstants(proxy, bytes), "");
#endif
}
class ParameterProbe final : public gpu::ShaderParameterSet {
public:
    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    bool Set(uint32_t id, uint32_t, gpu::ShaderParameterValue) noexcept override {
        Written.push_back(id);
        return !Fail;
    }
    bool FlushWrites() noexcept override {
        ADD_FAILURE();
        return false;
    }
    void ResetBindingsForReuse() noexcept override { ADD_FAILURE(); }
    vector<uint32_t> Written;
    bool Fail{false};
};
TEST(MaterialBindingMap, PreflightsResourcesAndReportsPartialSetFailure) {
    LayoutDevice device;
    AssetManager assets{device};
    const auto shader = assets.CreateShader(MakeShaderDesc(ResourceLayout()));
    ASSERT_TRUE(shader);
    std::function<void()> destroyed;
    FakeTextureView view{{}, destroyed};
    auto desc = MakeMaterialDesc(*shader, {{"First", Nullable<gpu::TextureView*>{&view}}, {"Second", Nullable<gpu::TextureView*>{nullptr}}}, {"First", "Second"});
    const auto material = assets.CreateMaterial(desc);
    ASSERT_TRUE(material);
    const auto map = assets.Get(*material)->GetBindingMap(*shader);
    MaterialInstance instance{*material};
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(assets, instance));
    ParameterProbe set;
    EXPECT_FALSE(map->BindResources(proxy, 0, set));
    EXPECT_TRUE(set.Written.empty());
    EXPECT_FALSE(instance.SetParameter(assets, 1, Nullable<gpu::Sampler*>{nullptr}));
    ASSERT_TRUE(instance.SetParameter(assets, 1, Nullable<gpu::TextureView*>{&view}));
    ASSERT_TRUE(proxy.Update(assets, instance));
    set.Fail = true;
    EXPECT_FALSE(map->BindResources(proxy, 0, set));
    EXPECT_EQ(set.Written, vector<uint32_t>{0});
    set.Fail = false;
    set.Written.clear();
    EXPECT_TRUE(map->BindResources(proxy, 0, set));
    EXPECT_EQ(set.Written, (vector<uint32_t>{0, 1}));
    EXPECT_FALSE(map->BindResources(proxy, 9, set));
}
TEST(MaterialBindingMap, RejectsArraysAndImmutableSamplerParameters) {
    for (bool sampler : {false, true}) {
        LayoutDevice device;
        AssetManager assets{device};
        auto layout = ResourceLayout();
        layout.Groups[0].Bindings.resize(1);
        auto& binding = layout.Groups[0].Bindings[0].Binding;
        if (sampler) {
            binding.Kind = gpu::BindingKind::Sampler;
            binding.D3D12->RegisterType = gpu::BindingRegisterClass::Sampler;
            binding.ImmutableSampler = gpu::SamplerDescriptor{};
        } else
            binding.Count = 2;
        const auto shader = assets.CreateShader(MakeShaderDesc(layout));
        ASSERT_TRUE(shader);
        const MaterialParameterValue value = sampler ? MaterialParameterValue{Nullable<gpu::Sampler*>{nullptr}} : MaterialParameterValue{Nullable<gpu::TextureView*>{nullptr}};
        EXPECT_FALSE(assets.CreateMaterial(MakeMaterialDesc(*shader, {{"First", value}}, {"First"})));
    }
}
TEST(MaterialBindingMap, SkipsInactiveResourcesAndRejectsInactiveConstants) {
    LayoutDevice device;
    device.Backend = gpu::RenderBackend::Vulkan;
    AssetManager assets{device};
    auto desc = ResourceLayout();
    desc.Groups[0].Bindings[1].Binding.Vulkan.reset();
    const auto shader = assets.CreateShader(MakeShaderDesc(desc, device.Backend));
    ASSERT_TRUE(shader);
    const auto material = assets.CreateMaterial(MakeMaterialDesc(*shader, {{"Second", Nullable<gpu::TextureView*>{nullptr}}}, {"Second"}));
    ASSERT_TRUE(material);
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(assets, *material));
    ParameterProbe set;
    EXPECT_TRUE(assets.Get(*material)->GetBindingMap(*shader)->BindResources(proxy, 0, set));
    EXPECT_TRUE(set.Written.empty());
    auto constants = MaterialLayout();
    constants.Groups[0].Bindings[0].Binding.Vulkan.reset();
    const auto other = assets.CreateShader(MakeShaderDesc(constants, device.Backend));
    ASSERT_TRUE(other);
    EXPECT_FALSE(assets.CreateMaterial(MakeMaterialDesc(*other, {{"Roughness", 1.0f}}, {"Roughness"}, "Constants")));
}
}  // namespace
}  // namespace radray
