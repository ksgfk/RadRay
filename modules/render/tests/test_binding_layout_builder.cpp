#include <utility>

#include <gtest/gtest.h>

#include <radray/render/common.h>

namespace radray::render {
namespace {

class TestPipelineLayout final : public PipelineLayout {
public:
    TestPipelineLayout(
        vector<ShaderParameterInfo> parameters,
        vector<BindingGroupLayout> groups,
        vector<PushConstantRange> pushConstants) noexcept
        : _parameters(std::move(parameters)),
          _groups(std::move(groups)),
          _pushConstants(std::move(pushConstants)) {}

    bool IsValid() const noexcept override { return _valid; }
    void Destroy() noexcept override { _valid = false; }
    void SetDebugName(std::string_view name) noexcept override { _name = string{name}; }

    vector<ShaderParameterInfo> GetParameters() const noexcept override { return _parameters; }

    Nullable<const ShaderParameterInfo*> FindParameter(std::string_view name) const noexcept override {
        for (const ShaderParameterInfo& parameter : _parameters) {
            if (parameter.Name == name) {
                return &parameter;
            }
        }
        return nullptr;
    }

    std::optional<ShaderBindingLocation> FindBindingLocation(std::string_view name) const noexcept override {
        for (const BindingGroupLayout& group : _groups) {
            for (const BindingGroupLayoutEntry& entry : group.Entries) {
                if (!entry.IsStaticSampler && entry.Parameter.Name == name) {
                    return ShaderBindingLocation{group.GroupIndex, entry.Binding};
                }
            }
        }
        return std::nullopt;
    }

    vector<BindingGroupLayout> GetBindingGroupLayouts() const noexcept override { return _groups; }
    vector<PushConstantRange> GetPushConstantRanges() const noexcept override { return _pushConstants; }

private:
    vector<ShaderParameterInfo> _parameters;
    vector<BindingGroupLayout> _groups;
    vector<PushConstantRange> _pushConstants;
    bool _valid{true};
    string _name;
};

ShaderParameterInfo MakeParameter(
    std::string_view name,
    ShaderParameterKind kind,
    ResourceBindType type,
    ShaderStages stages,
    uint32_t byteSize = 0) {
    return ShaderParameterInfo{
        .Name = string{name},
        .Kind = kind,
        .Stages = stages,
        .Type = type,
        .Count = 1,
        .ByteSize = byteSize};
}

TestPipelineLayout MakeLayout() {
    ShaderParameterInfo perObject = MakeParameter(
        "PerObject", ShaderParameterKind::Resource, ResourceBindType::CBuffer, ShaderStage::Vertex, 64);
    ShaderParameterInfo albedo = MakeParameter(
        "Albedo", ShaderParameterKind::Resource, ResourceBindType::Texture, ShaderStage::Pixel);
    ShaderParameterInfo linear = MakeParameter(
        "Linear", ShaderParameterKind::Sampler, ResourceBindType::Sampler, ShaderStage::Pixel);
    ShaderParameterInfo constants = MakeParameter(
        "ImGuiConstants", ShaderParameterKind::Constant, ResourceBindType::UNKNOWN, ShaderStage::Vertex, 16);

    vector<ShaderParameterInfo> parameters{perObject, albedo, constants};
    vector<BindingGroupLayout> groups{
        BindingGroupLayout{
            .GroupIndex = 0,
            .Entries = {BindingGroupLayoutEntry{
                .Parameter = perObject,
                .Binding = 0,
                .HasDynamicOffset = true}}},
        BindingGroupLayout{
            .GroupIndex = 2,
            .Entries = {
                BindingGroupLayoutEntry{.Parameter = albedo, .Binding = 1},
                BindingGroupLayoutEntry{
                    .Parameter = linear,
                    .Binding = 5,
                    .IsStaticSampler = true}}}};
    vector<PushConstantRange> pushConstants{PushConstantRange{
        .Name = "ImGuiConstants",
        .Group = 3,
        .Binding = 0,
        .Stages = ShaderStage::Vertex,
        .Offset = 0,
        .Size = 16}};
    return TestPipelineLayout{
        std::move(parameters), std::move(groups), std::move(pushConstants)};
}

}  // namespace

TEST(PipelineLayoutTest, ResolvesPublicParametersByName) {
    TestPipelineLayout layout = MakeLayout();

    auto* albedo = layout.FindParameter("Albedo").Get();
    ASSERT_NE(albedo, nullptr);
    EXPECT_EQ(albedo->Type, ResourceBindType::Texture);
    EXPECT_EQ(layout.FindBindingLocation("Albedo"), (ShaderBindingLocation{2, 1}));
    EXPECT_FALSE(layout.FindParameter("Missing").HasValue());
    EXPECT_FALSE(layout.FindBindingLocation("Linear").has_value());
}

TEST(PipelineLayoutTest, ExposesExplicitGroupAndPushConstantLayouts) {
    TestPipelineLayout layout = MakeLayout();

    vector<BindingGroupLayout> groups = layout.GetBindingGroupLayouts();
    ASSERT_EQ(groups.size(), 2u);
    ASSERT_EQ(groups[0].Entries.size(), 1u);
    EXPECT_EQ(groups[0].GroupIndex, 0u);
    EXPECT_EQ(groups[0].Entries[0].Binding, 0u);
    EXPECT_TRUE(groups[0].Entries[0].HasDynamicOffset);
    ASSERT_EQ(groups[1].Entries.size(), 2u);
    EXPECT_TRUE(groups[1].Entries[1].IsStaticSampler);

    vector<PushConstantRange> ranges = layout.GetPushConstantRanges();
    ASSERT_EQ(ranges.size(), 1u);
    EXPECT_EQ(ranges[0].Name, "ImGuiConstants");
    EXPECT_EQ(ranges[0].Group, 3u);
    EXPECT_EQ(ranges[0].Size, 16u);
}

}  // namespace radray::render
