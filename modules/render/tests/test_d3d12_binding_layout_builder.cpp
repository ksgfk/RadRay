#include <gtest/gtest.h>

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
    HlslSRVDimension dimension = HlslSRVDimension::TEXTURE2D) {
    HlslInputBindDesc binding{};
    binding.Name = string{name};
    binding.Type = type;
    binding.BindPoint = bindPoint;
    binding.BindCount = bindCount;
    binding.Dimension = dimension;
    binding.Space = space;
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
    EXPECT_TRUE(merged->Layout.GetParameters().empty());
    EXPECT_TRUE(merged->D3D12Parameters.empty());
}

TEST(D3D12BindingLayoutBuilderTest, MergesStagesAssignsIdsAndSupportsLookup) {
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

    auto parameters = merged->Layout.GetParameters();
    ASSERT_EQ(parameters.size(), 3u);

    EXPECT_EQ(parameters[0].Name, "Linear");
    EXPECT_EQ(parameters[0].Id, BindingParameterId{0});
    EXPECT_EQ(parameters[0].Kind, BindingParameterKind::Sampler);

    EXPECT_EQ(parameters[1].Name, "Albedo");
    EXPECT_EQ(parameters[1].Id, BindingParameterId{1});
    EXPECT_EQ(parameters[1].Kind, BindingParameterKind::Resource);
    EXPECT_EQ(parameters[1].Stages, ShaderStage::Vertex | ShaderStage::Pixel);
    EXPECT_EQ(std::get<ResourceBindingAbi>(parameters[1].Abi).BindingOrRegister, 1u);

    EXPECT_EQ(parameters[2].Name, "Globals");
    EXPECT_EQ(parameters[2].Id, BindingParameterId{2});
    EXPECT_EQ(parameters[2].Kind, BindingParameterKind::PushConstant);
    EXPECT_EQ(parameters[2].Stages, ShaderStage::Vertex | ShaderStage::Pixel);
    EXPECT_EQ(std::get<PushConstantBindingAbi>(parameters[2].Abi).Size, 16u);

    auto id = merged->Layout.FindParameterId("Albedo");
    ASSERT_TRUE(id.has_value());
    EXPECT_EQ(id.value(), BindingParameterId{1});

    auto parameter = merged->Layout.FindParameter(id.value());
    ASSERT_TRUE(parameter.HasValue());
    EXPECT_EQ(parameter.Get()->Name, "Albedo");

    ASSERT_EQ(merged->D3D12Parameters.size(), parameters.size());
    EXPECT_EQ(merged->D3D12Parameters[1].Id, BindingParameterId{1});
    EXPECT_EQ(merged->D3D12Parameters[1].ShaderRegister, 1u);
    EXPECT_EQ(merged->D3D12Parameters[2].Kind, BindingParameterKind::PushConstant);
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

TEST(D3D12BindingLayoutBuilderTest, FailsOnUnboundedArray) {
    FakeShader shader{
        ShaderStage::Pixel,
        ShaderReflectionDesc{MakeHlslShaderDesc({
            MakeHlslBinding("Textures", HlslShaderInputType::TEXTURE, 0, 0, 0),
        })}};
    vector<Shader*> shaders{&shader};
    EXPECT_FALSE(d3d12::BuildMergedBindingLayoutD3D12(shaders).has_value());
}

}  // namespace radray::render
