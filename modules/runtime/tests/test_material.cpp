#include <gtest/gtest.h>
#include <cstring>
#include "test_asset_fakes.h"
#include <radray/basic_math.h>
#include <radray/runtime/shader_asset.h>

namespace radray {
namespace {
using namespace test;
class MaterialAssetTest : public testing::Test {
protected:
    LayoutDevice Device;
    AssetManager Assets;
    AssetRef<ShaderAsset> Shader;
    MaterialAssetDesc Desc;
    void SetUp() override {
        auto shader = ShaderAsset::Create(Device, MakeShaderDesc(MaterialLayout()));
        ASSERT_TRUE(shader);
        Shader = Assets.Emplace(std::move(*shader), {}, MakeAssetId("material-shader"));
        ASSERT_TRUE(Shader);
        Desc = MakeMaterialDesc(Shader, {{"Roughness", 0.25f}, {"Metallic", 0.75f}, {"Tint", array<float, 4>{1, 2, 3, 4}}},
                                {"Roughness", "Tint"}, "Constants");
    }
};
TEST_F(MaterialAssetTest, PreparesSubsetWithoutCreatingNativeLayouts) {
    const auto handle = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(handle);
    const auto material = handle.Get();
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
    ASSERT_TRUE(proxy.Update(Assets, handle.GetHandle()));
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
TEST_F(MaterialAssetTest, StagedBindingsUseFinalShaderRegardlessOfOrderOrInterveningCommit) {
    auto current = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(current);
    AssetGarbageBatch retired;
    const auto stageShader = [&](AssetTransaction& tx, bool swapped) {
        auto layout = MaterialLayout();
        if (swapped) std::swap(layout.Types[1].Offset, layout.Types[3].Offset);
        // Tint occupies the first 16 bytes when Roughness moves to offset 16.
        if (swapped) layout.Types[2].Offset = 28;
        auto shader = ShaderAsset::Create(Device, MakeShaderDesc(layout));
        return shader && tx.StageReplace(Shader, std::move(*shader));
    };
    const auto verify = [&]() {
        MaterialRenderProxy proxy;
        ASSERT_TRUE(proxy.Update(Assets, current.GetHandle()));
        array<byte, 32> bytes{};
        ASSERT_TRUE(current.Get()->GetBindingMap(Shader)->WriteConstants(proxy, bytes));
        float value{};
        std::memcpy(&value, bytes.data() + 16, sizeof(value));
        EXPECT_FLOAT_EQ(value, 0.25f);
        std::memcpy(&value, bytes.data(), sizeof(value));
        EXPECT_FLOAT_EQ(value, 1.0f);
    };
    for (int scenario = 0; scenario < 3; ++scenario) {
        AssetTransaction tx(Assets);
        const auto shader = Assets.Find(MakeAssetId("material-shader"));
        ASSERT_TRUE(shader);
        const auto dep = Assets.GetDependents(*shader);
        ASSERT_EQ(dep.size(), 1u);
        const auto target = Assets.AsHandle<MaterialAsset>(dep[0]);
        ASSERT_EQ(target, current.GetHandle());
        if (scenario == 0) ASSERT_TRUE(stageShader(tx, true));
        ASSERT_TRUE(StageMaterial(tx, target, Desc));
        if (scenario == 1) ASSERT_TRUE(stageShader(tx, true));
        if (scenario == 2) {
            AssetTransaction middle(Assets);
            ASSERT_TRUE(stageShader(middle, true));
            ASSERT_TRUE(StageMaterial(middle, current, Desc));
            ASSERT_TRUE(Assets.Commit(std::move(middle), retired).Succeeded);
        }
        ASSERT_TRUE(Assets.Commit(std::move(tx), retired).Succeeded);
        verify();
        AssetTransaction restore(Assets);
        ASSERT_TRUE(stageShader(restore, false));
        ASSERT_TRUE(StageMaterial(restore, current, Desc));
        ASSERT_TRUE(Assets.Commit(std::move(restore), retired).Succeeded);
        retired.Clear();
    }
}

TEST_F(MaterialAssetTest, BreakingShaderAndChangedParameterSignatureCannotPartiallyPublish) {
    auto material = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(material);
    AssetGarbageBatch retired;
    auto candidate = ShaderAsset::Create(Device, MakeShaderDesc(ResourceLayout()));
    ASSERT_TRUE(candidate);
    AssetTransaction missing(Assets);
    ASSERT_TRUE(missing.StageReplace(Shader, std::move(*candidate)));
    EXPECT_FALSE(Assets.Commit(std::move(missing), retired).Succeeded);
    auto changed = Desc;
    std::swap(changed.Parameters[0], changed.Parameters[1]);
    AssetTransaction rejected(Assets);
    ASSERT_TRUE(StageMaterial(rejected, material, changed));
    EXPECT_FALSE(Assets.Commit(std::move(rejected), retired).Succeeded);
    AssetTransaction invalid(Assets);
    changed.Families.clear();
    ASSERT_TRUE(StageMaterial(invalid, material, changed));
    EXPECT_FALSE(Assets.Commit(std::move(invalid), retired).Succeeded);
    EXPECT_TRUE(retired.Empty());
    EXPECT_EQ(Assets.GetRevision(Shader.Untyped()), 1u);
    EXPECT_EQ(Assets.GetRevision(material.Untyped()), 1u);
}

TEST_F(MaterialAssetTest, RejectsInvalidExplicitSubsetsAndPrograms) {
    auto check = [&](auto edit) { auto desc = Desc; edit(desc); EXPECT_FALSE(CreateMaterial(Assets, desc)); };
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
    EXPECT_TRUE(CreateMaterial(Assets, Desc));
}
TEST_F(MaterialAssetTest, DifferentFamiliesUseIndependentExplicitSubsets) {
    const auto second = CreateTestShader(Assets, Device, MakeShaderDesc(ResourceLayout()));
    ASSERT_TRUE(second);
    Desc.Parameters.push_back({"First", Nullable<gpu::TextureView*>{nullptr}});
    Desc.Programs.push_back({"Other", second.GetHandle(), 0, "VS", "PS"});
    Desc.Families.push_back({second.GetHandle(), {}, {"First"}});
    const auto handle = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(handle);
    const auto material = handle.Get();
    EXPECT_EQ(material->GetBindingMap(Shader)->GetConstantTargets().size(), 2u);
    EXPECT_TRUE(material->GetBindingMap(second.GetHandle())->GetConstantTargets().empty());
    EXPECT_EQ(material->GetBindingMap(second.GetHandle())->GetResourceTargets().size(), 1u);
}
TEST_F(MaterialAssetTest, InstanceOverridesAndSnapshotsAreIndependent) {
    const auto handle = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(handle);
    MaterialInstance first{handle.GetHandle()}, second{handle.GetHandle()};
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
    EXPECT_EQ(proxy.GetMaterial(), handle.GetHandle());
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 0.5f);
    ASSERT_TRUE(proxy.Update(Assets, first));
    EXPECT_FLOAT_EQ(std::get<float>(*proxy.GetParameter(0)), 0.25f);
}
TEST_F(MaterialAssetTest, MaterialKeepsItsShaderUntilCollected) {
    const auto weak = Shader.GetHandle();
    auto material = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(material);
    Shader.Reset();
    EXPECT_EQ(Assets.CollectGarbage(), 0u);
    EXPECT_TRUE(Assets.Get(weak));
    material.Reset();
    EXPECT_EQ(Assets.CollectGarbage(), 2u);
    EXPECT_FALSE(Assets.Get(weak));
}

TEST_F(MaterialAssetTest, HandlesAndMappingsSurviveRegistryGrowth) {
    const auto first = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(first);
    MaterialInstance instance{first.GetHandle()};
    for (size_t i = 0; i < 100; ++i) {
        ASSERT_TRUE(CreateTestShader(Assets, Device, MakeShaderDesc()));
        ASSERT_TRUE(CreateMaterial(Assets, Desc));
    }
    EXPECT_FLOAT_EQ(std::get<float>(*instance.GetParameter(Assets, 0)), 0.25f);
    EXPECT_TRUE(first.Get()->GetBindingMap(Shader));
    EXPECT_TRUE(Shader.Get()->FindBinding("Constants"));
}
TEST_F(MaterialAssetTest, DebugRejectsDifferentMaterialIdentity) {
#if RADRAY_IS_DEBUG
    const auto first = CreateMaterial(Assets, Desc), second = CreateMaterial(Assets, Desc);
    ASSERT_TRUE(first && second);
    const auto map = first.Get()->GetBindingMap(Shader);
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(Assets, second.GetHandle()));
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
    AssetManager assets;
    const auto shader = CreateTestShader(assets, device, MakeShaderDesc(ResourceLayout()));
    ASSERT_TRUE(shader);
    std::function<void()> destroyed;
    FakeTextureView view{{}, destroyed};
    auto desc = MakeMaterialDesc(shader.GetHandle(), {{"First", Nullable<gpu::TextureView*>{&view}}, {"Second", Nullable<gpu::TextureView*>{nullptr}}}, {"First", "Second"});
    const auto material = CreateMaterial(assets, desc);
    ASSERT_TRUE(material);
    const auto map = material.Get()->GetBindingMap(shader.GetHandle());
    MaterialInstance instance{material.GetHandle()};
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
        AssetManager assets;
        auto layout = ResourceLayout();
        layout.Groups[0].Bindings.resize(1);
        auto& binding = layout.Groups[0].Bindings[0].Binding;
        if (sampler) {
            binding.Kind = gpu::BindingKind::Sampler;
            binding.D3D12->RegisterType = gpu::BindingRegisterClass::Sampler;
            binding.ImmutableSampler = gpu::SamplerDescriptor{};
        } else
            binding.Count = 2;
        const auto shader = CreateTestShader(assets, device, MakeShaderDesc(layout));
        ASSERT_TRUE(shader);
        const MaterialParameterValue value = sampler ? MaterialParameterValue{Nullable<gpu::Sampler*>{nullptr}} : MaterialParameterValue{Nullable<gpu::TextureView*>{nullptr}};
        EXPECT_FALSE(CreateMaterial(assets, MakeMaterialDesc(shader.GetHandle(), {{"First", value}}, {"First"})));
    }
}
TEST(MaterialBindingMap, SkipsInactiveResourcesAndRejectsInactiveConstants) {
    LayoutDevice device;
    device.Backend = gpu::GpuBackend::Vulkan;
    AssetManager assets;
    auto desc = ResourceLayout();
    desc.Groups[0].Bindings[1].Binding.Vulkan.reset();
    const auto shader = CreateTestShader(assets, device, MakeShaderDesc(desc, device.Backend));
    ASSERT_TRUE(shader);
    const auto material = CreateMaterial(assets, MakeMaterialDesc(shader.GetHandle(), {{"Second", Nullable<gpu::TextureView*>{nullptr}}}, {"Second"}));
    ASSERT_TRUE(material);
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(assets, material.GetHandle()));
    ParameterProbe set;
    EXPECT_TRUE(material.Get()->GetBindingMap(shader.GetHandle())->BindResources(proxy, 0, set));
    EXPECT_TRUE(set.Written.empty());
    auto constants = MaterialLayout();
    constants.Groups[0].Bindings[0].Binding.Vulkan.reset();
    const auto other = CreateTestShader(assets, device, MakeShaderDesc(constants, device.Backend));
    ASSERT_TRUE(other);
    EXPECT_FALSE(CreateMaterial(assets, MakeMaterialDesc(other.GetHandle(), {{"Roughness", 1.0f}}, {"Roughness"}, "Constants")));
}

TEST(MaterialBindingMap, SupportsAllValueTypesAndAutoTransposesMatrix4x4) {
    const auto makeLayout = [](bool rowMajor) {
        auto desc = MatrixLayout();
        desc.Types.clear();
        desc.Types.push_back({.Name = "MaterialData", .Size = 144, .ElementSize = 144});
        desc.Types.push_back({.Name = "Transform", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Matrix, .ElementKind = shader::ShaderTypeKind::Matrix, .Offset = 0, .Size = 64, .ElementSize = 64, .MatrixStride = 16, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 4, .ColumnCount = 4, .RowMajor = rowMajor});
        desc.Types.push_back({.Name = "I4", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 64, .Size = 16, .ElementSize = 16, .ScalarKind = shader::ShaderScalarKind::SignedInteger, .RowCount = 1, .ColumnCount = 4});
        desc.Types.push_back({.Name = "U4", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 80, .Size = 16, .ElementSize = 16, .ScalarKind = shader::ShaderScalarKind::UnsignedInteger, .RowCount = 1, .ColumnCount = 4});
        desc.Types.push_back({.Name = "F3", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 96, .Size = 12, .ElementSize = 12, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 3});
        desc.Types.push_back({.Name = "F1", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Scalar, .ElementKind = shader::ShaderTypeKind::Scalar, .Offset = 108, .Size = 4, .ElementSize = 4, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 1});
        desc.Types.push_back({.Name = "F2", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 112, .Size = 8, .ElementSize = 8, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 2});
        desc.Types.push_back({.Name = "I1", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Scalar, .ElementKind = shader::ShaderTypeKind::Scalar, .Offset = 120, .Size = 4, .ElementSize = 4, .ScalarKind = shader::ShaderScalarKind::SignedInteger, .RowCount = 1, .ColumnCount = 1});
        desc.Types.push_back({.Name = "U1", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Scalar, .ElementKind = shader::ShaderTypeKind::Scalar, .Offset = 124, .Size = 4, .ElementSize = 4, .ScalarKind = shader::ShaderScalarKind::UnsignedInteger, .RowCount = 1, .ColumnCount = 1});
        desc.Types.push_back({.Name = "F4", .ParentIndex = 0, .Kind = shader::ShaderTypeKind::Vector, .ElementKind = shader::ShaderTypeKind::Vector, .Offset = 128, .Size = 16, .ElementSize = 16, .ScalarKind = shader::ShaderScalarKind::Float, .RowCount = 1, .ColumnCount = 4});
        desc.Groups[0].Bindings[0].BufferByteSize = 144;
        return desc;
    };

    LayoutDevice device;
    AssetManager assets;
    const auto rowMajorShader = CreateTestShader(assets, device, MakeShaderDesc(makeLayout(true)));
    const auto colMajorShader = CreateTestShader(assets, device, MakeShaderDesc(makeLayout(false)));
    ASSERT_TRUE(rowMajorShader && colMajorShader);

    Eigen::Matrix4f matrix;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            matrix(r, c) = static_cast<float>(r * 4 + c + 1);
        }
    }
    array<float, 16> asymmetric{};
    std::memcpy(asymmetric.data(), matrix.data(), sizeof(asymmetric));
    const vector<string> subset{"Transform", "I4", "U4", "F3", "F1", "F2", "I1", "U1", "F4"};
    MaterialAssetDesc matDesc;
    matDesc.Programs = {{"Row", rowMajorShader.GetHandle(), 0, "VS", "PS"}, {"Col", colMajorShader.GetHandle(), 0, "VS", "PS"}};
    matDesc.Parameters = {
        {"Transform", asymmetric},
        {"I4", array<int32_t, 4>{-1, 2, -3, 4}},
        {"U4", array<uint32_t, 4>{10u, 20u, 30u, 40u}},
        {"F3", array<float, 3>{1.5f, 2.5f, 3.5f}},
        {"F1", 0.75f},
        {"F2", array<float, 2>{4.25f, 5.25f}},
        {"I1", int32_t{-42}},
        {"U1", uint32_t{99u}},
        {"F4", array<float, 4>{6.0f, 7.0f, 8.0f, 9.0f}}};
    matDesc.Families = {
        {rowMajorShader.GetHandle(), "Constants", subset},
        {colMajorShader.GetHandle(), "Constants", subset}};
    const auto material = CreateMaterial(assets, matDesc);
    ASSERT_TRUE(material);

    const auto rowMap = material.Get()->GetBindingMap(rowMajorShader.GetHandle());
    const auto colMap = material.Get()->GetBindingMap(colMajorShader.GetHandle());
    ASSERT_TRUE(rowMap && colMap);
    EXPECT_TRUE(rowMap->GetConstantTargets()[0].TransposeMatrix4x4);
    EXPECT_FALSE(colMap->GetConstantTargets()[0].TransposeMatrix4x4);

    MaterialInstance instance{material.GetHandle()};
    // 类型不匹配覆盖应被拒绝。
    EXPECT_FALSE(instance.SetParameter(assets, 6, uint32_t{1u}));
    ASSERT_TRUE(instance.SetParameter(assets, 6, int32_t{-77}));
    MaterialRenderProxy proxy;
    ASSERT_TRUE(proxy.Update(assets, instance));

    array<byte, 144> rowBytes{};
    array<byte, 144> colBytes{};
    ASSERT_TRUE(rowMap->WriteConstants(proxy, rowBytes));
    ASSERT_TRUE(colMap->WriteConstants(proxy, colBytes));

    array<float, 16> outRow{};
    array<float, 16> outCol{};
    std::memcpy(outRow.data(), rowBytes.data(), 64);
    std::memcpy(outCol.data(), colBytes.data(), 64);
    EXPECT_EQ(outCol, asymmetric);
    for (uint32_t r = 0; r < 4; ++r) {
        for (uint32_t c = 0; c < 4; ++c) {
            EXPECT_FLOAT_EQ(outRow[r * 4 + c], asymmetric[c * 4 + r]);
            EXPECT_FLOAT_EQ(outRow[r * 4 + c], matrix(static_cast<int>(r), static_cast<int>(c)));
        }
    }
    // 校验非矩阵字段字节一致且覆盖生效。
    EXPECT_TRUE(std::equal(rowBytes.begin() + 64, rowBytes.end(), colBytes.begin() + 64));
    int32_t writtenI1 = 0;
    std::memcpy(&writtenI1, rowBytes.data() + 120, sizeof(writtenI1));
    EXPECT_EQ(writtenI1, -77);

    // 校验 signed/unsigned 不匹配以及错误矩阵步长/维度被 Create 拒绝。
    auto badSigned = matDesc;
    badSigned.Parameters[6].DefaultValue = uint32_t{1u};
    EXPECT_FALSE(CreateMaterial(assets, badSigned));

    auto badStrideLayout = makeLayout(true);
    badStrideLayout.Types.resize(2);
    badStrideLayout.Types[0].Size = 112;
    badStrideLayout.Types[0].ElementSize = 112;
    badStrideLayout.Types[1].Size = 112;
    badStrideLayout.Types[1].ElementSize = 112;
    badStrideLayout.Types[1].MatrixStride = 32;
    badStrideLayout.Groups[0].Bindings[0].BufferByteSize = 112;
    const auto badStrideShader = CreateTestShader(assets, device, MakeShaderDesc(badStrideLayout));
    ASSERT_TRUE(badStrideShader);
    EXPECT_FALSE(CreateMaterial(assets, MakeMaterialDesc(badStrideShader.GetHandle(), {{"Transform", asymmetric}}, {"Transform"}, "Constants")));
}
}  // namespace
}  // namespace radray
