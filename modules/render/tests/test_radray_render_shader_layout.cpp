#include <radray/render/shader_layout.h>

#include "shader_contract_fixtures.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace radray::render {
namespace {

constexpr uint64_t kFixtureToolchainIdentity = 0x0000000001090212ull;

vector<byte> ReadBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    vector<byte> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return file.good() || file.eof() ? data : vector<byte>{};
}

std::optional<size_t> FindFixtureIndex(std::string_view name) {
    const auto fixtures = test::GetShaderContractFixtures();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        if (fixtures[index].Name == name) {
            return index;
        }
    }
    return std::nullopt;
}

vector<byte> ReadFixture(std::string_view name, shader::ShaderTarget target) {
    const string suffix = target == shader::ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin";
    return ReadBinary(
        std::filesystem::path{RADRAY_PROJECT_DIR} /
        "modules/render/tests/data/shader_artifacts" /
        (string{name} + suffix));
}

shader::ShaderArtifactDecodeOptions FixtureOptions(
    std::string_view name,
    shader::ShaderTarget target) {
    const std::optional<size_t> index = FindFixtureIndex(name);
    EXPECT_TRUE(index.has_value());
    return shader::ShaderArtifactDecodeOptions{
        .Target = target,
        .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index.value_or(0), target),
        .ExpectedToolchainIdentity = kFixtureToolchainIdentity};
}

ShaderLayoutSelector CBufferSelector(std::string_view name) {
    return ShaderLayoutSelector{
        .DeclarationName = string{name},
        .ExpectedLogicalResourceKind = shader::ShaderBindingKind::CBuffer};
}

// multiple_cbuffers declares First and Second in different groups on both targets, which is the
// smallest artifact that can show whether the order the caller lists modifiers in leaks into the
// resolved value.
TEST(RadRayRenderShaderLayout, ModifierOrderDoesNotChangeTheResolvedLayout) {
    const vector<byte> dxilBlob = ReadFixture("multiple_cbuffers", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(dxilBlob.empty());
    const auto dxil = shader::DecodeDxilShaderArtifact(
        dxilBlob, FixtureOptions("multiple_cbuffers", shader::ShaderTarget::DXIL));
    ASSERT_TRUE(dxil.has_value());

    const D3D12BufferPlacementModifier first{
        .Selector = CBufferSelector("First"), .Placement = D3D12BufferPlacement::RootDescriptor};
    const D3D12BufferPlacementModifier second{
        .Selector = CBufferSelector("Second"), .Placement = D3D12BufferPlacement::RootDescriptor};

    D3D12TargetLayoutOptions forward;
    forward.BufferPlacements = {first, second};
    D3D12TargetLayoutOptions reversed;
    reversed.BufferPlacements = {second, first};
    const auto forwardLayout = ResolveD3D12Layout(dxil.value(), forward);
    const auto reversedLayout = ResolveD3D12Layout(dxil.value(), reversed);
    ASSERT_TRUE(forwardLayout.has_value());
    ASSERT_TRUE(reversedLayout.has_value());
    EXPECT_EQ(forwardLayout->Bindings, reversedLayout->Bindings);
    EXPECT_EQ(forwardLayout->Hash, reversedLayout->Hash);
    // Order-independence would be trivially true if the modifiers did nothing, so check that both
    // destinations actually moved.
    const auto plainLayout = ResolveD3D12Layout(dxil.value());
    ASSERT_TRUE(plainLayout.has_value());
    EXPECT_NE(plainLayout->Hash, forwardLayout->Hash);
    ASSERT_EQ(forwardLayout->Bindings.size(), 2u);
    for (const ResolvedD3D12Binding& binding : forwardLayout->Bindings) {
        EXPECT_EQ(binding.Placement, shader::ShaderBindingPlacement::RootDescriptor) << binding.Name;
    }

    const vector<byte> spirvBlob = ReadFixture("multiple_cbuffers", shader::ShaderTarget::SPIRV);
    ASSERT_FALSE(spirvBlob.empty());
    const auto spirv = shader::DecodeSpirvShaderArtifact(
        spirvBlob, FixtureOptions("multiple_cbuffers", shader::ShaderTarget::SPIRV));
    ASSERT_TRUE(spirv.has_value());

    const VulkanBufferDescriptorModifier firstDynamic{
        .Selector = CBufferSelector("First"),
        .Placement = VulkanBufferDescriptorPlacement::Dynamic};
    const VulkanBufferDescriptorModifier secondDynamic{
        .Selector = CBufferSelector("Second"),
        .Placement = VulkanBufferDescriptorPlacement::Dynamic};
    VulkanTargetLayoutOptions vulkanForward;
    vulkanForward.BufferDescriptors = {firstDynamic, secondDynamic};
    VulkanTargetLayoutOptions vulkanReversed;
    vulkanReversed.BufferDescriptors = {secondDynamic, firstDynamic};
    const auto vulkanForwardLayout = ResolveVulkanLayout(spirv.value(), vulkanForward);
    const auto vulkanReversedLayout = ResolveVulkanLayout(spirv.value(), vulkanReversed);
    ASSERT_TRUE(vulkanForwardLayout.has_value());
    ASSERT_TRUE(vulkanReversedLayout.has_value());
    EXPECT_EQ(vulkanForwardLayout->Bindings, vulkanReversedLayout->Bindings);
    EXPECT_EQ(vulkanForwardLayout->Hash, vulkanReversedLayout->Hash);
    const auto vulkanPlainLayout = ResolveVulkanLayout(spirv.value());
    ASSERT_TRUE(vulkanPlainLayout.has_value());
    EXPECT_NE(vulkanPlainLayout->Hash, vulkanForwardLayout->Hash);
    EXPECT_TRUE(vulkanPlainLayout->DynamicOffsetOrder.empty());
    // Dynamic offsets are packed in resolved (set, binding) order, so the caller's argument order
    // cannot decide which offset belongs to which descriptor.
    ASSERT_EQ(vulkanForwardLayout->DynamicOffsetOrder.size(), 2u);
    EXPECT_EQ(vulkanForwardLayout->DynamicOffsetOrder, vulkanReversedLayout->DynamicOffsetOrder);
    for (size_t index = 1; index < vulkanForwardLayout->DynamicOffsetOrder.size(); ++index) {
        const ResolvedVulkanBinding& previous =
            vulkanForwardLayout->Bindings[vulkanForwardLayout->DynamicOffsetOrder[index - 1]];
        const ResolvedVulkanBinding& current =
            vulkanForwardLayout->Bindings[vulkanForwardLayout->DynamicOffsetOrder[index]];
        EXPECT_LT(std::tie(previous.Set, previous.Binding), std::tie(current.Set, current.Binding));
    }
}

// One recipe states both targets' options, so resolving for one target must ignore the other's.
TEST(RadRayRenderShaderLayout, OtherTargetOptionsDoNotChangeThisTargetsIdentity) {
    const vector<byte> dxilBlob = ReadFixture("multiple_cbuffers", shader::ShaderTarget::DXIL);
    const vector<byte> spirvBlob = ReadFixture("multiple_cbuffers", shader::ShaderTarget::SPIRV);
    ASSERT_FALSE(dxilBlob.empty());
    ASSERT_FALSE(spirvBlob.empty());
    const auto dxil = shader::DecodeDxilShaderArtifact(
        dxilBlob, FixtureOptions("multiple_cbuffers", shader::ShaderTarget::DXIL));
    const auto spirv = shader::DecodeSpirvShaderArtifact(
        spirvBlob, FixtureOptions("multiple_cbuffers", shader::ShaderTarget::SPIRV));
    ASSERT_TRUE(dxil.has_value());
    ASSERT_TRUE(spirv.has_value());

    ShaderProgramLayoutRecipe recipe;
    recipe.D3D12.BufferPlacements.push_back(
        {.Selector = CBufferSelector("First"),
         .Placement = D3D12BufferPlacement::RootDescriptor});
    const auto d3d12Before = ResolveD3D12Layout(dxil.value(), recipe.D3D12);
    const auto vulkanBefore = ResolveVulkanLayout(spirv.value(), recipe.Vulkan);
    ASSERT_TRUE(d3d12Before.has_value());
    ASSERT_TRUE(vulkanBefore.has_value());

    recipe.Vulkan.BufferDescriptors.push_back(
        {.Selector = CBufferSelector("Second"),
         .Placement = VulkanBufferDescriptorPlacement::Dynamic});
    const auto d3d12After = ResolveD3D12Layout(dxil.value(), recipe.D3D12);
    ASSERT_TRUE(d3d12After.has_value());
    EXPECT_EQ(d3d12Before->Hash, d3d12After->Hash);

    const auto vulkanAfter = ResolveVulkanLayout(spirv.value(), recipe.Vulkan);
    ASSERT_TRUE(vulkanAfter.has_value());
    EXPECT_NE(vulkanBefore->Hash, vulkanAfter->Hash);
}

// The resolved layout is an owning value, so a cache can keep it after the artifact bytes are gone.
TEST(RadRayRenderShaderLayout, ResolvedLayoutOutlivesTheArtifactItCameFrom) {
    std::optional<ResolvedD3D12Layout> d3d12;
    std::optional<ResolvedVulkanLayout> vulkan;
    ResolvedLayoutHash d3d12Hash{};
    ResolvedLayoutHash vulkanHash{};
    {
        const vector<byte> dxilBlob = ReadFixture("shadow_static_sampler", shader::ShaderTarget::DXIL);
        const vector<byte> spirvBlob = ReadFixture("shadow_static_sampler", shader::ShaderTarget::SPIRV);
        ASSERT_FALSE(dxilBlob.empty());
        ASSERT_FALSE(spirvBlob.empty());
        const auto dxilArtifact = shader::DecodeDxilShaderArtifact(
            dxilBlob, FixtureOptions("shadow_static_sampler", shader::ShaderTarget::DXIL));
        const auto spirvArtifact = shader::DecodeSpirvShaderArtifact(
            spirvBlob, FixtureOptions("shadow_static_sampler", shader::ShaderTarget::SPIRV));
        ASSERT_TRUE(dxilArtifact.has_value());
        ASSERT_TRUE(spirvArtifact.has_value());
        d3d12 = ResolveD3D12Layout(dxilArtifact.value());
        vulkan = ResolveVulkanLayout(spirvArtifact.value());
        ASSERT_TRUE(d3d12.has_value());
        ASSERT_TRUE(vulkan.has_value());
        d3d12Hash = d3d12->Hash;
        vulkanHash = vulkan->Hash;
    }

    // Everything the layout needs is its own: names, the serialized carrier, and sampler state.
    EXPECT_EQ(d3d12->Hash, d3d12Hash);
    EXPECT_TRUE(d3d12->HasExplicitCarrier());
    ASSERT_FALSE(d3d12->Bindings.empty());
    const auto texture = d3d12->FindRecord("ShadowTexture");
    ASSERT_TRUE(texture.HasValue());
    EXPECT_EQ(texture.Get()->Kind, ShaderLayoutRecordKind::Descriptor);
    ASSERT_LT(texture.Get()->ResolvedIndex, d3d12->Bindings.size());
    EXPECT_EQ(d3d12->Bindings[texture.Get()->ResolvedIndex].Name, "ShadowTexture");

    EXPECT_EQ(vulkan->Hash, vulkanHash);
    ASSERT_EQ(vulkan->ImmutableSamplers.size(), 1u);
    EXPECT_EQ(vulkan->ImmutableSamplers[0].CompareEnable, 1u);
    const auto sampler = vulkan->FindRecord("ShadowSampler");
    ASSERT_TRUE(sampler.HasValue());
    ASSERT_LT(sampler.Get()->ResolvedIndex, vulkan->Bindings.size());
    EXPECT_EQ(vulkan->Bindings[sampler.Get()->ResolvedIndex].ImmutableSamplerIndex, 0u);
    EXPECT_FALSE(vulkan->FindRecord("NotDeclared").HasValue());
}

// A replacement is wholesale: a partial merge would make the resolved state depend on what the
// policy happened to publish.
TEST(RadRayRenderShaderLayout, ImmutableSamplerModifierReplacesThePublishedState) {
    const vector<byte> blob = ReadFixture("shadow_static_sampler", shader::ShaderTarget::SPIRV);
    ASSERT_FALSE(blob.empty());
    const auto artifact = shader::DecodeSpirvShaderArtifact(
        blob, FixtureOptions("shadow_static_sampler", shader::ShaderTarget::SPIRV));
    ASSERT_TRUE(artifact.has_value());

    const auto published = ResolveVulkanLayout(artifact.value());
    ASSERT_TRUE(published.has_value());
    ASSERT_EQ(published->ImmutableSamplers.size(), 1u);

    VulkanImmutableSamplerState replacement{};
    replacement.MagFilter = 0;
    replacement.MinFilter = 0;
    replacement.MipmapMode = 0;
    replacement.AddressModeU = 2;
    replacement.AddressModeV = 2;
    replacement.AddressModeW = 2;
    replacement.MaxAnisotropy = 1.0f;
    replacement.MaxLod = 4.0f;
    VulkanTargetLayoutOptions options;
    options.ImmutableSamplers.push_back(
        {.Selector = {.DeclarationName = "ShadowSampler",
                      .ExpectedLogicalResourceKind = shader::ShaderBindingKind::Sampler},
         .State = replacement});
    const auto replaced = ResolveVulkanLayout(artifact.value(), options);
    ASSERT_TRUE(replaced.has_value());
    // The descriptor already owned a state, so the modifier replaces it in place instead of adding
    // a second recipe for one descriptor.
    ASSERT_EQ(replaced->ImmutableSamplers.size(), 1u);
    EXPECT_EQ(replaced->ImmutableSamplers[0], replacement);
    EXPECT_NE(replaced->ImmutableSamplers[0], published->ImmutableSamplers[0]);
    // Sampler state is hashed inline, so two layouts that differ only in it cannot share an
    // identity.
    EXPECT_NE(replaced->Hash, published->Hash);
}

}  // namespace
}  // namespace radray::render
