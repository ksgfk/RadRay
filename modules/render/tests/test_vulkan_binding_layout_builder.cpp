#include <gtest/gtest.h>

#include <radray/render/backend/vulkan_impl.h>

namespace radray::render {
namespace {

class FakeShader final : public Shader {
public:
    FakeShader(
        ShaderStages stages,
        std::optional<ShaderReflectionDesc> reflection = std::nullopt) noexcept
        : _stages(stages),
          _reflection(std::move(reflection)) {}

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override {
        _valid = false;
        _stages = ShaderStage::UNKNOWN;
        _reflection.reset();
    }

    ShaderStages GetStages() const noexcept override { return _stages; }

    Nullable<const ShaderReflectionDesc*> GetReflection() const noexcept override {
        return _reflection.has_value()
                   ? Nullable<const ShaderReflectionDesc*>{&_reflection.value()}
                   : Nullable<const ShaderReflectionDesc*>{};
    }

private:
    bool _valid{true};
    ShaderStages _stages{ShaderStage::UNKNOWN};
    std::optional<ShaderReflectionDesc> _reflection{};
};

SpirvResourceBinding MakeSpirvBinding(
    std::string_view name,
    SpirvResourceKind kind,
    uint32_t set,
    uint32_t binding,
    uint32_t arraySize = 1,
    bool isUnbounded = false,
    bool readOnly = true,
    bool writeOnly = false,
    SpirvImageDim imageDim = SpirvImageDim::Dim2D,
    std::optional<uint32_t> hlslRegister = std::nullopt,
    std::optional<uint32_t> hlslSpace = std::nullopt) {
    SpirvResourceBinding resource{};
    resource.Name = string{name};
    resource.Kind = kind;
    resource.Set = set;
    resource.Binding = binding;
    resource.HlslRegister = hlslRegister;
    resource.HlslSpace = hlslSpace;
    resource.ArraySize = arraySize;
    resource.TypeIndex = 1;
    resource.ReadOnly = readOnly;
    resource.WriteOnly = writeOnly;
    resource.IsViewInHlsl = false;
    resource.IsUnboundedArray = isUnbounded;
    if (kind == SpirvResourceKind::SampledImage ||
        kind == SpirvResourceKind::SeparateImage ||
        kind == SpirvResourceKind::StorageImage) {
        resource.ImageInfo = SpirvImageInfo{
            .Dim = imageDim,
            .Arrayed = false,
            .Multisampled = false,
            .Depth = false,
            .SampledType = 1,
        };
    }
    return resource;
}

SpirvPushConstantRange MakePushConstant(std::string_view name, uint32_t offset, uint32_t size) {
    SpirvPushConstantRange range{};
    range.Name = string{name};
    range.Offset = offset;
    range.Size = size;
    range.TypeIndex = 1;
    return range;
}

SpirvShaderDesc MakeSpirvShaderDesc(
    std::initializer_list<SpirvResourceBinding> bindings,
    std::initializer_list<SpirvPushConstantRange> pushConstants = {}) {
    SpirvShaderDesc desc{};
    desc.ResourceBindings.assign(bindings.begin(), bindings.end());
    desc.ConstantRanges.assign(pushConstants.begin(), pushConstants.end());
    return desc;
}

}  // namespace

TEST(VulkanBindingLayoutBuilderTest, ReturnsEmptyLayoutForEmptyShaderList) {
    auto merged = vulkan::BuildMergedBindingLayoutVulkan({});
    ASSERT_TRUE(merged.has_value());
    EXPECT_TRUE(merged->Parameters.empty());
    EXPECT_TRUE(merged->VulkanParameters.empty());
    EXPECT_EQ(merged->DescriptorSetCount, 0u);
}

TEST(VulkanBindingLayoutBuilderTest, MergesStagesAssignsIdsAndBackendMetadata) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeSpirvShaderDesc(
            {
                MakeSpirvBinding("Linear", SpirvResourceKind::SeparateSampler, 0, 0),
                MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 1),
            },
            {
                MakePushConstant("Globals", 0, 16),
            })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc(
            {
                MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 1),
            },
            {
                MakePushConstant("Globals", 0, 16),
            })}};
    vector<Shader*> shaders{&ps, &vs};

    auto merged = vulkan::BuildMergedBindingLayoutVulkan(shaders);
    ASSERT_TRUE(merged.has_value());

    auto parameters = merged->Parameters;
    ASSERT_EQ(parameters.size(), 3u);

    EXPECT_EQ(parameters[0].Name, "Linear");
    EXPECT_EQ(parameters[0].Id, ShaderParameterId{0});
    EXPECT_EQ(parameters[0].Kind, ShaderParameterKind::Sampler);

    EXPECT_EQ(parameters[1].Name, "Albedo");
    EXPECT_EQ(parameters[1].Id, ShaderParameterId{1});
    EXPECT_EQ(parameters[1].Kind, ShaderParameterKind::Resource);
    EXPECT_EQ(parameters[1].Stages, ShaderStage::Vertex | ShaderStage::Pixel);
    EXPECT_EQ(parameters[1].Type, ResourceBindType::Texture);
    EXPECT_EQ(parameters[1].Count, 1u);

    EXPECT_EQ(parameters[2].Name, "Globals");
    EXPECT_EQ(parameters[2].Id, ShaderParameterId{2});
    EXPECT_EQ(parameters[2].Kind, ShaderParameterKind::Constant);
    EXPECT_EQ(parameters[2].Stages, ShaderStage::Vertex | ShaderStage::Pixel);
    EXPECT_EQ(parameters[2].ByteSize, 16u);

    ASSERT_EQ(merged->VulkanParameters.size(), parameters.size());
    EXPECT_EQ(merged->VulkanParameters[0].Id, ShaderParameterId{0});
    EXPECT_EQ(merged->VulkanParameters[0].DescriptorType, VK_DESCRIPTOR_TYPE_SAMPLER);
    EXPECT_EQ(merged->VulkanParameters[1].SetIndex, 0u);
    EXPECT_EQ(merged->VulkanParameters[1].BindingIndex, 1u);
    EXPECT_EQ(merged->VulkanParameters[1].DescriptorType, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    EXPECT_EQ(merged->VulkanParameters[2].Kind, ShaderParameterKind::Constant);
    EXPECT_EQ(merged->VulkanParameters[2].Size, 16u);
    EXPECT_EQ(merged->DescriptorSetCount, 1u);
}

TEST(VulkanBindingLayoutBuilderTest, FailsWithoutReflectionMetadata) {
    FakeShader shader{ShaderStage::Vertex};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, FailsWithoutStageMetadata) {
    FakeShader shader{
        ShaderStage::UNKNOWN,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 1),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, FailsWhenNameMapsToDifferentAbi) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 1),
        })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 2),
        })}};
    vector<Shader*> shaders{&vs, &ps};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, FailsWhenAbiMapsToDifferentNames) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 1),
        })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Diffuse", SpirvResourceKind::SeparateImage, 0, 1),
        })}};
    vector<Shader*> shaders{&vs, &ps};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, BuildsBindlessSetFromUnboundedArray) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Buffers", SpirvResourceKind::StorageBuffer, 0, 0, 0, true),
        })}};
    vector<Shader*> shaders{&shader};
    auto merged = vulkan::BuildMergedBindingLayoutVulkan(shaders);
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged->Parameters.size(), 1u);
    const auto& parameter = merged->Parameters[0];
    EXPECT_TRUE(parameter.IsBindless);
    EXPECT_EQ(parameter.Count, 0u);
    EXPECT_EQ(parameter.Kind, ShaderParameterKind::BindlessArray);
    ASSERT_EQ(merged->VulkanParameters.size(), 1u);
    EXPECT_TRUE(merged->VulkanParameters[0].IsBindless);
    EXPECT_EQ(merged->VulkanParameters[0].SetIndex, 0u);
    EXPECT_EQ(merged->VulkanParameters[0].BindingIndex, 0u);
    EXPECT_EQ(merged->VulkanParameters[0].BindlessSlotType, BindlessSlotType::BufferOnly);
}

TEST(VulkanBindingLayoutBuilderTest, FailsWhenBindlessSetMixesWithOrdinaryDescriptors) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Buffers", SpirvResourceKind::StorageBuffer, 0, 0, 0, true),
            MakeSpirvBinding("Linear", SpirvResourceKind::SeparateSampler, 0, 1),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, FailsWhenSetContainsMultipleBindlessParameters) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("BuffersA", SpirvResourceKind::StorageBuffer, 0, 0, 0, true),
            MakeSpirvBinding("BuffersB", SpirvResourceKind::StorageBuffer, 0, 1, 0, true),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, FailsWhenSameSetBindingUsesDifferentKindsOrTypes) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 0, 1),
        })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Linear", SpirvResourceKind::SeparateSampler, 0, 1),
        })}};
    vector<Shader*> shaders{&vs, &ps};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

TEST(VulkanBindingLayoutBuilderTest, AcceptsMatchingHlslRegisterMetadata) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 1, 0, 1, false, true, false, SpirvImageDim::Dim2D, 0, 1),
        })}};
    vector<Shader*> shaders{&shader};

    auto merged = vulkan::BuildMergedBindingLayoutVulkan(shaders);
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged->Parameters.size(), 1u);
    EXPECT_EQ(merged->VulkanParameters[0].SetIndex, 1u);
    EXPECT_EQ(merged->VulkanParameters[0].BindingIndex, 0u);
    EXPECT_EQ(merged->DescriptorSetCount, 2u);
}

TEST(VulkanBindingLayoutBuilderTest, FailsWhenHlslRegisterMetadataConflictsWithVkBinding) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeSpirvShaderDesc({
            MakeSpirvBinding("Albedo", SpirvResourceKind::SeparateImage, 1, 0, 1, false, true, false, SpirvImageDim::Dim2D, 0, 0),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(vulkan::BuildMergedBindingLayoutVulkan(shaders).has_value());
}

}  // namespace radray::render
