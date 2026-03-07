#include <cstdint>

#include <gtest/gtest.h>

#include <radray/render/common.h>

namespace radray::render {
namespace {

class FakeRootSignature final : public RootSignature {
public:
    explicit FakeRootSignature(BindingLayout layout) noexcept
        : _layout(std::move(layout)) {}

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    const BindingLayout& GetBindingLayout() const noexcept override { return _layout; }

private:
    bool _valid{true};
    string _name{};
    BindingLayout _layout{};
};

class FakeBindingSet final : public BindingSet {
public:
    using BindingSet::WritePushConstant;
    using BindingSet::WriteResource;
    using BindingSet::WriteSampler;

    explicit FakeBindingSet(RootSignature* rootSig) noexcept
        : _rootSig(rootSig) {}

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    RootSignature* GetRootSignature() const noexcept override { return _rootSig; }

    bool WriteResource(BindingParameterId id, ResourceView* view, uint32_t arrayIndex = 0) noexcept override {
        LastResourceId = id;
        LastResourceView = view;
        LastResourceArrayIndex = arrayIndex;
        return true;
    }

    bool WriteSampler(BindingParameterId id, Sampler* sampler, uint32_t arrayIndex = 0) noexcept override {
        LastSamplerId = id;
        LastSampler = sampler;
        LastSamplerArrayIndex = arrayIndex;
        return true;
    }

    bool WritePushConstant(BindingParameterId id, const void* data, uint32_t size) noexcept override {
        LastPushConstantId = id;
        LastPushConstantData = data;
        LastPushConstantSize = size;
        return true;
    }

    std::optional<BindingParameterId> LastResourceId{};
    ResourceView* LastResourceView{nullptr};
    uint32_t LastResourceArrayIndex{0};

    std::optional<BindingParameterId> LastSamplerId{};
    Sampler* LastSampler{nullptr};
    uint32_t LastSamplerArrayIndex{0};

    std::optional<BindingParameterId> LastPushConstantId{};
    const void* LastPushConstantData{nullptr};
    uint32_t LastPushConstantSize{0};

private:
    bool _valid{true};
    RootSignature* _rootSig{nullptr};
    string _name{};
};

class DummyTextureView final : public TextureView {
public:
    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

private:
    bool _valid{true};
    string _name{};
};

class DummySampler final : public Sampler {
public:
    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

private:
    bool _valid{true};
    string _name{};
};

BindingLayout MakeBindingLayoutForHelpers() {
    vector<BindingParameterLayout> parameters{};
    parameters.push_back(BindingParameterLayout{
        .Name = "Tex",
        .Id = BindingParameterId{0},
        .Kind = BindingParameterKind::Resource,
        .Stages = ShaderStage::Pixel,
        .Abi = ResourceBindingAbi{
            .SpaceOrSet = 0,
            .BindingOrRegister = 1,
            .Type = ResourceBindType::Texture,
            .Count = 1,
            .IsReadOnly = true,
        }});
    parameters.push_back(BindingParameterLayout{
        .Name = "Linear",
        .Id = BindingParameterId{1},
        .Kind = BindingParameterKind::Sampler,
        .Stages = ShaderStage::Pixel,
        .Abi = ResourceBindingAbi{
            .SpaceOrSet = 0,
            .BindingOrRegister = 2,
            .Type = ResourceBindType::Sampler,
            .Count = 1,
            .IsReadOnly = true,
        }});
    parameters.push_back(BindingParameterLayout{
        .Name = "Pc",
        .Id = BindingParameterId{2},
        .Kind = BindingParameterKind::PushConstant,
        .Stages = ShaderStage::Pixel,
        .Abi = PushConstantBindingAbi{
            .Offset = 0,
            .Size = sizeof(uint32_t),
        }});
    return BindingLayout{std::move(parameters)};
}

}  // namespace

TEST(BindingSetTest, NameHelpersResolveThroughRootSignatureLayout) {
    FakeRootSignature rootSig{MakeBindingLayoutForHelpers()};
    FakeBindingSet bindingSet{&rootSig};
    DummyTextureView texView{};
    DummySampler sampler{};
    const uint32_t pushConstantValue = 7;

    EXPECT_TRUE(bindingSet.WriteResource("Tex", &texView, 2));
    ASSERT_TRUE(bindingSet.LastResourceId.has_value());
    EXPECT_EQ(bindingSet.LastResourceId.value(), BindingParameterId{0});
    EXPECT_EQ(bindingSet.LastResourceView, &texView);
    EXPECT_EQ(bindingSet.LastResourceArrayIndex, 2u);

    EXPECT_TRUE(bindingSet.WriteSampler("Linear", &sampler, 1));
    ASSERT_TRUE(bindingSet.LastSamplerId.has_value());
    EXPECT_EQ(bindingSet.LastSamplerId.value(), BindingParameterId{1});
    EXPECT_EQ(bindingSet.LastSampler, &sampler);
    EXPECT_EQ(bindingSet.LastSamplerArrayIndex, 1u);

    EXPECT_TRUE(bindingSet.WritePushConstant("Pc", &pushConstantValue, sizeof(pushConstantValue)));
    ASSERT_TRUE(bindingSet.LastPushConstantId.has_value());
    EXPECT_EQ(bindingSet.LastPushConstantId.value(), BindingParameterId{2});
    EXPECT_EQ(bindingSet.LastPushConstantData, &pushConstantValue);
    EXPECT_EQ(bindingSet.LastPushConstantSize, sizeof(pushConstantValue));

    EXPECT_FALSE(bindingSet.WriteResource("Missing", &texView));
    EXPECT_FALSE(bindingSet.WriteSampler("Missing", &sampler));
    EXPECT_FALSE(bindingSet.WritePushConstant("Missing", &pushConstantValue, sizeof(pushConstantValue)));
}

}  // namespace radray::render
