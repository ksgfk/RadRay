#include <cstdint>

#include <gtest/gtest.h>

#include <radray/render/common.h>

namespace radray::render {
namespace {

class FakeRootSignature final : public RootSignature {
public:
    explicit FakeRootSignature(BindingLayout layout) noexcept
        : _layout(std::move(layout)) {
        vector<BindingParameterLayout> setLayout{};
        for (const auto& parameter : _layout.GetParameters()) {
            if (parameter.Kind == BindingParameterKind::PushConstant) {
                const auto& abi = std::get<PushConstantBindingAbi>(parameter.Abi);
                _pushConstantRanges.push_back(PushConstantRange{
                    .Name = parameter.Name,
                    .Id = parameter.Id,
                    .Stages = parameter.Stages,
                    .Offset = abi.Offset,
                    .Size = abi.Size,
                });
                continue;
            }
            setLayout.push_back(parameter);
        }
        _setLayouts.push_back(std::move(setLayout));
    }

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    const BindingLayout& GetBindingLayout() const noexcept override { return _layout; }

    uint32_t GetDescriptorSetCount() const noexcept override { return static_cast<uint32_t>(_setLayouts.size()); }

    std::span<const BindingParameterLayout> GetDescriptorSetLayout(DescriptorSetIndex set) const noexcept override {
        if (set.Value >= _setLayouts.size()) {
            return {};
        }
        return _setLayouts[set.Value];
    }

    std::span<const PushConstantRange> GetPushConstantRanges() const noexcept override {
        return _pushConstantRanges;
    }

private:
    bool _valid{true};
    string _name{};
    BindingLayout _layout{};
    vector<vector<BindingParameterLayout>> _setLayouts{};
    vector<PushConstantRange> _pushConstantRanges{};
};

class FakeDescriptorSet final : public DescriptorSet {
public:
    using DescriptorSet::WriteResource;
    using DescriptorSet::WriteSampler;

    explicit FakeDescriptorSet(RootSignature* rootSig, DescriptorSetIndex setIndex = DescriptorSetIndex{0}) noexcept
        : _rootSig(rootSig),
          _setIndex(setIndex) {}

    bool IsValid() const noexcept override { return _valid; }

    void Destroy() noexcept override { _valid = false; }

    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    RootSignature* GetRootSignature() const noexcept override { return _rootSig; }

    DescriptorSetIndex GetSetIndex() const noexcept override { return _setIndex; }

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

    std::optional<BindingParameterId> LastResourceId{};
    ResourceView* LastResourceView{nullptr};
    uint32_t LastResourceArrayIndex{0};

    std::optional<BindingParameterId> LastSamplerId{};
    Sampler* LastSampler{nullptr};
    uint32_t LastSamplerArrayIndex{0};

private:
    bool _valid{true};
    RootSignature* _rootSig{nullptr};
    DescriptorSetIndex _setIndex{0};
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
            .Set = DescriptorSetIndex{0},
            .Binding = 1,
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
            .Set = DescriptorSetIndex{0},
            .Binding = 2,
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

TEST(DescriptorSetTest, NameHelpersResolveThroughRootSignatureLayout) {
    FakeRootSignature rootSig{MakeBindingLayoutForHelpers()};
    FakeDescriptorSet descriptorSet{&rootSig};
    DummyTextureView texView{};
    DummySampler sampler{};

    EXPECT_TRUE(descriptorSet.WriteResource("Tex", &texView, 2));
    ASSERT_TRUE(descriptorSet.LastResourceId.has_value());
    EXPECT_EQ(descriptorSet.LastResourceId.value(), BindingParameterId{0});
    EXPECT_EQ(descriptorSet.LastResourceView, &texView);
    EXPECT_EQ(descriptorSet.LastResourceArrayIndex, 2u);

    EXPECT_TRUE(descriptorSet.WriteSampler("Linear", &sampler, 1));
    ASSERT_TRUE(descriptorSet.LastSamplerId.has_value());
    EXPECT_EQ(descriptorSet.LastSamplerId.value(), BindingParameterId{1});
    EXPECT_EQ(descriptorSet.LastSampler, &sampler);
    EXPECT_EQ(descriptorSet.LastSamplerArrayIndex, 1u);

    EXPECT_FALSE(descriptorSet.WriteResource("Missing", &texView));
    EXPECT_FALSE(descriptorSet.WriteSampler("Missing", &sampler));
}

TEST(RootSignatureTest, ExposesDescriptorSetLayoutsAndPushConstantRanges) {
    FakeRootSignature rootSig{MakeBindingLayoutForHelpers()};

    EXPECT_EQ(rootSig.GetDescriptorSetCount(), 1u);
    auto setLayout = rootSig.GetDescriptorSetLayout(DescriptorSetIndex{0});
    ASSERT_EQ(setLayout.size(), 2u);
    EXPECT_EQ(setLayout[0].Name, "Tex");
    EXPECT_EQ(setLayout[1].Name, "Linear");

    auto ranges = rootSig.GetPushConstantRanges();
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].Name, "Pc");
    EXPECT_EQ(ranges[0].Id, BindingParameterId{2});
    EXPECT_EQ(ranges[0].Offset, 0u);
    EXPECT_EQ(ranges[0].Size, sizeof(uint32_t));
}

}  // namespace radray::render
