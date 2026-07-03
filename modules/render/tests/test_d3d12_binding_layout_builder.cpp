#include <gtest/gtest.h>

#include <algorithm>

#include <radray/render/backend/d3d12_impl.h>

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

HlslInputBindDesc MakeHlslBinding(
    std::string_view name,
    HlslShaderInputType type,
    uint32_t bindPoint,
    uint32_t space = 0,
    uint32_t bindCount = 1,
    HlslSRVDimension dimension = HlslSRVDimension::TEXTURE2D,
    std::optional<uint32_t> vkBinding = std::nullopt,
    std::optional<uint32_t> vkSet = std::nullopt) {
    HlslInputBindDesc binding{};
    binding.Name = string{name};
    binding.Type = type;
    binding.BindPoint = bindPoint;
    binding.BindCount = bindCount;
    binding.Dimension = dimension;
    binding.Space = space;
    binding.VkBinding = vkBinding;
    binding.VkSet = vkSet;
    return binding;
}

HlslShaderBufferDesc MakeCBuffer(std::string_view name, uint32_t size, bool isViewInHlsl) {
    HlslShaderBufferDesc buffer{};
    buffer.Name = string{name};
    buffer.Type = HlslCBufferType::CBUFFER;
    buffer.Size = size;
    buffer.IsViewInHlsl = isViewInHlsl;
    return buffer;
}

HlslShaderDesc MakeHlslShaderDesc(
    std::initializer_list<HlslInputBindDesc> bindings,
    std::initializer_list<HlslShaderBufferDesc> cbuffers = {}) {
    HlslShaderDesc desc{};
    desc.BoundResources.assign(bindings.begin(), bindings.end());
    desc.ConstantBuffers.assign(cbuffers.begin(), cbuffers.end());
    return desc;
}

}  // namespace

TEST(D3D12BindingLayoutBuilderTest, ReturnsEmptyLayoutForEmptyShaderList) {
    auto merged = d3d12::BuildMergedBindingLayoutD3D12({});
    ASSERT_TRUE(merged.has_value());
    EXPECT_TRUE(merged->Parameters.empty());
    EXPECT_TRUE(merged->D3D12Parameters.empty());
}

TEST(D3D12BindingLayoutBuilderTest, MergesStagesAssignsIdsAndBackendMetadata) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeHlslShaderDesc(
            {
                MakeHlslBinding("Linear", HlslShaderInputType::SAMPLER, 0),
                MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 1),
                MakeHlslBinding("Globals", HlslShaderInputType::CBUFFER, 0),
            },
            {
                MakeCBuffer("Globals", 16, true),
            })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc(
            {
                MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 1),
                MakeHlslBinding("Globals", HlslShaderInputType::CBUFFER, 0),
            },
            {
                MakeCBuffer("Globals", 16, true),
            })}};
    vector<Shader*> shaders{&ps, &vs};

    auto merged = d3d12::BuildMergedBindingLayoutD3D12(shaders);
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

    ASSERT_EQ(merged->D3D12Parameters.size(), parameters.size());
    EXPECT_EQ(merged->D3D12Parameters[1].Id, ShaderParameterId{1});
    EXPECT_EQ(merged->D3D12Parameters[1].RegisterSpace, 0u);
    EXPECT_EQ(merged->D3D12Parameters[1].BindingIndex, 1u);
    EXPECT_EQ(merged->D3D12Parameters[1].ShaderRegister, 1u);
    EXPECT_EQ(merged->D3D12Parameters[2].Kind, ShaderParameterKind::Constant);
    EXPECT_EQ(merged->D3D12Parameters[2].PushConstantSize, 16u);
    EXPECT_EQ(merged->RegisterSpaceCount, 1u);
}

TEST(D3D12BindingLayoutBuilderTest, FailsWithoutReflectionMetadata) {
    FakeShader shader{ShaderStage::Vertex};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

TEST(D3D12BindingLayoutBuilderTest, FailsWithoutStageMetadata) {
    FakeShader shader{
        ShaderStage::UNKNOWN,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 1),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

TEST(D3D12BindingLayoutBuilderTest, FailsWhenNameMapsToDifferentAbi) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 1),
        })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 2),
        })}};
    vector<Shader*> shaders{&vs, &ps};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

TEST(D3D12BindingLayoutBuilderTest, FailsWhenAbiMapsToDifferentNames) {
    FakeShader vs{
        ShaderStage::Vertex,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 1),
        })}};
    FakeShader ps{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Diffuse", HlslShaderInputType::TEXTURE, 1),
        })}};
    vector<Shader*> shaders{&vs, &ps};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

TEST(D3D12BindingLayoutBuilderTest, BuildsBindlessSetFromUnboundedArray) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Buffers", HlslShaderInputType::STRUCTURED, 0, 0, 0, HlslSRVDimension::BUFFER),
        })}};
    vector<Shader*> shaders{&shader};
    auto merged = d3d12::BuildMergedBindingLayoutD3D12(shaders);
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged->Parameters.size(), 1u);
    const auto& parameter = merged->Parameters[0];
    EXPECT_TRUE(parameter.IsBindless);
    EXPECT_EQ(parameter.Count, 0u);
    EXPECT_EQ(parameter.Kind, ShaderParameterKind::BindlessArray);
    ASSERT_EQ(merged->D3D12Parameters.size(), 1u);
    EXPECT_TRUE(merged->D3D12Parameters[0].IsBindless);
    EXPECT_EQ(merged->D3D12Parameters[0].RegisterSpace, 0u);
    EXPECT_EQ(merged->D3D12Parameters[0].BindingIndex, 0u);
    EXPECT_EQ(merged->D3D12Parameters[0].BindlessSlotType, BindlessSlotType::BufferOnly);
}

TEST(D3D12BindingLayoutBuilderTest, FailsWhenBindlessSetMixesWithOrdinaryDescriptors) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Buffers", HlslShaderInputType::STRUCTURED, 0, 0, 0, HlslSRVDimension::BUFFER),
            MakeHlslBinding("Sampler0", HlslShaderInputType::SAMPLER, 1, 0),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

TEST(D3D12BindingLayoutBuilderTest, FailsWhenSetContainsMultipleBindlessParameters) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("BuffersA", HlslShaderInputType::STRUCTURED, 0, 0, 0, HlslSRVDimension::BUFFER),
            MakeHlslBinding("BuffersB", HlslShaderInputType::STRUCTURED, 1, 0, 0, HlslSRVDimension::BUFFER),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

TEST(D3D12BindingLayoutBuilderTest, AcceptsMatchingVkBindingMetadata) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 0, 1, 1, HlslSRVDimension::TEXTURE2D, 0, 1),
        })}};
    vector<Shader*> shaders{&shader};

    auto merged = d3d12::BuildMergedBindingLayoutD3D12(shaders);
    ASSERT_TRUE(merged.has_value());
    ASSERT_EQ(merged->Parameters.size(), 1u);
    EXPECT_EQ(merged->D3D12Parameters[0].RegisterSpace, 1u);
    EXPECT_EQ(merged->D3D12Parameters[0].BindingIndex, 0u);
    EXPECT_EQ(merged->D3D12Parameters[0].ShaderRegister, 0u);
    EXPECT_EQ(merged->RegisterSpaceCount, 2u);
}

TEST(D3D12BindingLayoutBuilderTest, FailsWhenVkBindingMetadataConflictsWithRegisterSpace) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Albedo", HlslShaderInputType::TEXTURE, 0, 0, 1, HlslSRVDimension::TEXTURE2D, 0, 1),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

}  // namespace radray::render
