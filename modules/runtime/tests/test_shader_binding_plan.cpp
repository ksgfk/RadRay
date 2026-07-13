#include <gtest/gtest.h>

#include <radray/render/shader/hlsl.h>
#include <radray/runtime/shader_binding_plan.h>

using namespace radray;

namespace {

class FakeShader final : public render::Shader {
public:
    explicit FakeShader(render::HlslShaderDesc reflection)
        : _reflection(std::move(reflection)) {}

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    render::ShaderStages GetStages() const noexcept override {
        return render::ShaderStage::Vertex;
    }
    Nullable<const render::ShaderReflectionDesc*> GetReflection() const noexcept override {
        return &_reflection;
    }

private:
    render::ShaderReflectionDesc _reflection;
};

class FakePipelineLayout final : public render::PipelineLayout {
public:
    explicit FakePipelineLayout(
        vector<render::BindingGroupLayout> groups,
        vector<render::PushConstantRange> pushConstants = {})
        : _groups(std::move(groups)), _pushConstants(std::move(pushConstants)) {}

    bool IsValid() const noexcept override { return true; }
    void Destroy() noexcept override {}
    void SetDebugName(std::string_view) noexcept override {}

    vector<render::ShaderParameterInfo> GetParameters() const noexcept override {
        vector<render::ShaderParameterInfo> result;
        for (const auto& group : _groups) {
            for (const auto& entry : group.Entries) {
                if (!entry.IsStaticSampler) {
                    result.push_back(entry.Parameter);
                }
            }
        }
        return result;
    }

    Nullable<const render::ShaderParameterInfo*> FindParameter(
        std::string_view name) const noexcept override {
        for (const auto& group : _groups) {
            for (const auto& entry : group.Entries) {
                if (!entry.IsStaticSampler && entry.Parameter.Name == name) {
                    return &entry.Parameter;
                }
            }
        }
        return nullptr;
    }

    std::optional<render::ShaderBindingLocation> FindBindingLocation(
        std::string_view name) const noexcept override {
        for (const auto& group : _groups) {
            for (const auto& entry : group.Entries) {
                if (!entry.IsStaticSampler && entry.Parameter.Name == name) {
                    return render::ShaderBindingLocation{group.GroupIndex, entry.Binding};
                }
            }
        }
        return std::nullopt;
    }

    vector<render::BindingGroupLayout> GetBindingGroupLayouts() const noexcept override {
        return _groups;
    }
    vector<render::PushConstantRange> GetPushConstantRanges() const noexcept override {
        return _pushConstants;
    }

private:
    vector<render::BindingGroupLayout> _groups;
    vector<render::PushConstantRange> _pushConstants;
};

struct ReflectedField {
    string Name;
    uint32_t Offset{0};
    uint32_t Size{0};
};

render::HlslShaderDesc MakeCBufferReflection(
    std::string_view name,
    uint32_t group,
    uint32_t binding,
    uint32_t size,
    std::span<const ReflectedField> fields) {
    render::HlslShaderDesc reflection;
    render::HlslShaderBufferDesc block;
    block.Name = string{name};
    block.Type = render::HlslCBufferType::CBUFFER;
    block.Size = size;
    for (const ReflectedField& field : fields) {
        block.Variables.push_back(reflection.Variables.size());
        reflection.Variables.push_back(render::HlslShaderVariableDesc{
            .Name = field.Name,
            .StartOffset = field.Offset,
            .Size = field.Size});
    }
    reflection.ConstantBuffers.push_back(std::move(block));
    render::HlslInputBindDesc resource;
    resource.Name = string{name};
    resource.Type = render::HlslShaderInputType::CBUFFER;
    resource.BindPoint = binding;
    resource.BindCount = 1;
    resource.Space = group;
    resource.VkBinding = binding;
    resource.VkSet = group;
    reflection.BoundResources.push_back(std::move(resource));
    return reflection;
}

render::BindingGroupLayoutEntry MakeEntry(
    std::string_view name,
    uint32_t binding,
    render::ResourceBindType type,
    bool dynamic = false,
    uint32_t count = 1,
    bool readOnly = true) {
    return render::BindingGroupLayoutEntry{
        .Parameter = render::ShaderParameterInfo{
            .Name = string{name},
            .Kind = type == render::ResourceBindType::Sampler
                        ? render::ShaderParameterKind::Sampler
                        : render::ShaderParameterKind::Resource,
            .Stages = render::ShaderStage::Graphics,
            .Type = type,
            .Count = count,
            .ByteSize = type == render::ResourceBindType::CBuffer ? 80u : 0u,
            .IsReadOnly = readOnly},
        .Binding = binding,
        .HasDynamicOffset = dynamic};
}

CompiledShaderVariant MakeVariant(
    ShaderAsset& shader,
    FakeShader& reflectedShader,
    FakePipelineLayout& layout) {
    CompiledShaderVariant variant{};
    variant.VS = &reflectedShader;
    variant.Layout = &layout;
    variant.Key.ProgramId = shader.GetProgramId();
    variant.Key.PassIndex = 0;
    return variant;
}

}  // namespace

TEST(ShaderBindingResolverTest, SupportsArbitraryGroupsAndGroupHoles) {
    const ReflectedField fields[] = {{"Tint", 0, 16}};
    FakeShader reflected{MakeCBufferReflection("MaterialData", 2, 4, 16, fields)};
    FakePipelineLayout layout{{
        render::BindingGroupLayout{
            .GroupIndex = 7,
            .Entries = {MakeEntry("Albedo", 1, render::ResourceBindType::Texture)}},
        render::BindingGroupLayout{
            .GroupIndex = 2,
            .Entries = {MakeEntry("MaterialData", 4, render::ResourceBindType::CBuffer)}}}};
    ShaderAsset shader{ShaderKeywordSet{}, {ShaderPassDesc{.PassTag = "Forward"}}};
    CompiledShaderVariant variant = MakeVariant(shader, reflected, layout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* plan = library.GetOrCreate(shader, 0, variant).Get();
    ASSERT_NE(plan, nullptr);
    ASSERT_TRUE(plan->Valid) << plan->Error;
    ASSERT_EQ(plan->Groups.size(), 2u);
    EXPECT_EQ(plan->Groups[0].Group, 2u);
    EXPECT_EQ(plan->Groups[1].Group, 7u);
    EXPECT_EQ(plan->Groups[0].Frequency, ShaderBindingFrequency::Material);
    EXPECT_EQ(plan->Groups[1].Frequency, ShaderBindingFrequency::Material);

    EXPECT_EQ(library.GetMissCount(), 1u);
    EXPECT_EQ(library.GetHitCount(), 0u);
    EXPECT_EQ(library.GetOrCreate(shader, 0, variant).Get(), plan);
    EXPECT_EQ(library.GetHitCount(), 1u);
}

TEST(ShaderBindingResolverTest, ClassifiesMixedScopeFieldsInOneCBuffer) {
    const ReflectedField fields[] = {
        {"ObjectToWorld", 0, 64},
        {"Tint", 64, 16}};
    FakeShader reflected{MakeCBufferReflection("Mixed", 4, 3, 80, fields)};
    FakePipelineLayout layout{{render::BindingGroupLayout{
        .GroupIndex = 4,
        .Entries = {MakeEntry("Mixed", 3, render::ResourceBindType::CBuffer, true)}}}};
    ShaderPassDesc pass{.PassTag = "Forward"};
    pass.ParameterSources = {
        ShaderParameterSourceDesc{
            .Name = "Mixed.ObjectToWorld",
            .Scope = ShaderParameterScope::Object,
            .ProviderName = "ObjectToWorld"},
        ShaderParameterSourceDesc{
            .Name = "Mixed.Tint",
            .Scope = ShaderParameterScope::Material,
            .ProviderName = "Tint"}};
    ShaderAsset shader{ShaderKeywordSet{}, {std::move(pass)}};
    CompiledShaderVariant variant = MakeVariant(shader, reflected, layout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* plan = library.GetOrCreate(shader, 0, variant).Get();
    ASSERT_NE(plan, nullptr);
    ASSERT_TRUE(plan->Valid) << plan->Error;
    ASSERT_EQ(plan->Entries.size(), 1u);
    ASSERT_EQ(plan->Entries[0].Fields.size(), 2u);
    EXPECT_EQ(plan->Entries[0].Frequency, ShaderBindingFrequency::Object);
    EXPECT_EQ(plan->Groups[0].Frequency, ShaderBindingFrequency::Object);
    EXPECT_EQ(plan->Entries[0].Fields[0].Source.Scope, ShaderParameterScope::Object);
    EXPECT_EQ(plan->Entries[0].Fields[1].Source.Scope, ShaderParameterScope::Material);
    EXPECT_EQ(plan->Groups[0].DynamicEntryIndices.size(), 1u);
}

TEST(ShaderBindingResolverTest, GroupUsesMostFrequentSource) {
    const ReflectedField fields[] = {{"Tint", 0, 16}};
    FakeShader reflected{MakeCBufferReflection("MaterialData", 9, 0, 16, fields)};
    FakePipelineLayout layout{{render::BindingGroupLayout{
        .GroupIndex = 9,
        .Entries = {
            MakeEntry("MaterialData", 0, render::ResourceBindType::CBuffer),
            MakeEntry("GlobalTexture", 1, render::ResourceBindType::Texture)}}}};
    ShaderPassDesc pass{.PassTag = "Forward"};
    pass.ParameterSources.push_back(ShaderParameterSourceDesc{
        .Name = "GlobalTexture",
        .Scope = ShaderParameterScope::Pass});
    ShaderAsset shader{ShaderKeywordSet{}, {std::move(pass)}};
    CompiledShaderVariant variant = MakeVariant(shader, reflected, layout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* plan = library.GetOrCreate(shader, 0, variant).Get();
    ASSERT_TRUE(plan != nullptr && plan->Valid) << (plan != nullptr ? plan->Error : "null");
    EXPECT_EQ(plan->Groups[0].Frequency, ShaderBindingFrequency::Pass);
}

TEST(ShaderBindingResolverTest, RejectsWholeBlockAndFieldSourceOverlap) {
    const ReflectedField fields[] = {{"Tint", 0, 16}};
    FakeShader reflected{MakeCBufferReflection("Mixed", 3, 2, 16, fields)};
    FakePipelineLayout layout{{render::BindingGroupLayout{
        .GroupIndex = 3,
        .Entries = {MakeEntry("Mixed", 2, render::ResourceBindType::CBuffer)}}}};
    ShaderPassDesc pass{.PassTag = "Forward"};
    pass.ParameterSources = {
        ShaderParameterSourceDesc{
            .Name = "Mixed",
            .Scope = ShaderParameterScope::Object},
        ShaderParameterSourceDesc{
            .Name = "Mixed.Tint",
            .Scope = ShaderParameterScope::Material}};
    ShaderAsset shader{ShaderKeywordSet{}, {std::move(pass)}};
    CompiledShaderVariant variant = MakeVariant(shader, reflected, layout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* plan = library.GetOrCreate(shader, 0, variant).Get();
    ASSERT_NE(plan, nullptr);
    EXPECT_FALSE(plan->Valid);
    EXPECT_EQ(plan->ErrorGroup, 3u);
    EXPECT_EQ(plan->ErrorBinding, 2u);
}

TEST(ShaderBindingResolverTest, AllowsMaterialWholeBlockAndMaterialFieldSources) {
    const ReflectedField fields[] = {{"Tint", 0, 16}};
    FakeShader reflected{MakeCBufferReflection("MaterialData", 3, 2, 16, fields)};
    FakePipelineLayout layout{{render::BindingGroupLayout{
        .GroupIndex = 3,
        .Entries = {MakeEntry("MaterialData", 2, render::ResourceBindType::CBuffer)}}}};
    ShaderPassDesc pass{.PassTag = "Forward"};
    pass.ParameterSources = {
        ShaderParameterSourceDesc{
            .Name = "MaterialData",
            .Scope = ShaderParameterScope::Material,
            .ProviderName = "BlockDefaults"},
        ShaderParameterSourceDesc{
            .Name = "MaterialData.Tint",
            .Scope = ShaderParameterScope::Material,
            .ProviderName = "TintOverride"}};
    ShaderAsset shader{ShaderKeywordSet{}, {std::move(pass)}};
    CompiledShaderVariant variant = MakeVariant(shader, reflected, layout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* plan = library.GetOrCreate(shader, 0, variant).Get();
    ASSERT_NE(plan, nullptr);
    ASSERT_TRUE(plan->Valid) << plan->Error;
    ASSERT_EQ(plan->Entries.size(), 1u);
    ASSERT_EQ(plan->Entries[0].Fields.size(), 1u);
    EXPECT_EQ(plan->Entries[0].Source.ProviderName, "BlockDefaults");
    EXPECT_EQ(plan->Entries[0].Fields[0].Source.ProviderName, "TintOverride");
}

TEST(ShaderBindingResolverTest, RejectsInvalidReflectedFieldRanges) {
    const ReflectedField fields[] = {{"OutOfBounds", 12, 8}};
    FakeShader reflected{MakeCBufferReflection("Invalid", 1, 4, 16, fields)};
    FakePipelineLayout layout{{render::BindingGroupLayout{
        .GroupIndex = 1,
        .Entries = {MakeEntry("Invalid", 4, render::ResourceBindType::CBuffer)}}}};
    ShaderAsset shader{ShaderKeywordSet{}, {ShaderPassDesc{.PassTag = "Forward"}}};
    CompiledShaderVariant variant = MakeVariant(shader, reflected, layout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* plan = library.GetOrCreate(shader, 0, variant).Get();
    ASSERT_NE(plan, nullptr);
    EXPECT_FALSE(plan->Valid);
    EXPECT_EQ(plan->ErrorGroup, 1u);
    EXPECT_EQ(plan->ErrorBinding, 4u);
}

TEST(ShaderBindingResolverTest, RejectsArraysAndWritableTextures) {
    FakeShader reflected{render::HlslShaderDesc{}};
    FakePipelineLayout arrayLayout{{render::BindingGroupLayout{
        .GroupIndex = 5,
        .Entries = {MakeEntry("ArrayTexture", 6, render::ResourceBindType::Texture, false, 2)}}}};
    ShaderAsset shader{ShaderKeywordSet{}, {ShaderPassDesc{.PassTag = "Forward"}}};
    CompiledShaderVariant arrayVariant = MakeVariant(shader, reflected, arrayLayout);
    ShaderBindingPlanLibrary library;

    const ShaderBindingPlan* arrayPlan = library.GetOrCreate(shader, 0, arrayVariant).Get();
    ASSERT_NE(arrayPlan, nullptr);
    EXPECT_FALSE(arrayPlan->Valid);
    EXPECT_EQ(arrayPlan->ErrorGroup, 5u);
    EXPECT_EQ(arrayPlan->ErrorBinding, 6u);

    FakePipelineLayout writableLayout{{render::BindingGroupLayout{
        .GroupIndex = 8,
        .Entries = {MakeEntry(
            "StorageTexture", 1, render::ResourceBindType::Texture, false, 1, false)}}}};
    CompiledShaderVariant writableVariant = MakeVariant(shader, reflected, writableLayout);
    writableVariant.Key.SourceVersion = 1;
    const ShaderBindingPlan* writablePlan = library.GetOrCreate(shader, 0, writableVariant).Get();
    ASSERT_NE(writablePlan, nullptr);
    EXPECT_FALSE(writablePlan->Valid);
    EXPECT_EQ(writablePlan->ErrorGroup, 8u);
    EXPECT_EQ(writablePlan->ErrorBinding, 1u);
}

TEST(ShaderBindingResolverTest, RejectsBuffersBindlessAndPushConstants) {
    FakeShader reflected{render::HlslShaderDesc{}};
    ShaderAsset shader{ShaderKeywordSet{}, {ShaderPassDesc{.PassTag = "Forward"}}};
    ShaderBindingPlanLibrary library;

    FakePipelineLayout bufferLayout{{render::BindingGroupLayout{
        .GroupIndex = 2,
        .Entries = {MakeEntry("StorageBuffer", 3, render::ResourceBindType::RWBuffer)}}}};
    CompiledShaderVariant bufferVariant = MakeVariant(shader, reflected, bufferLayout);
    bufferVariant.Key.SourceVersion = 1;
    const ShaderBindingPlan* bufferPlan = library.GetOrCreate(shader, 0, bufferVariant).Get();
    ASSERT_NE(bufferPlan, nullptr);
    EXPECT_FALSE(bufferPlan->Valid);
    EXPECT_EQ(bufferPlan->ErrorGroup, 2u);
    EXPECT_EQ(bufferPlan->ErrorBinding, 3u);

    render::BindingGroupLayoutEntry bindlessEntry{};
    bindlessEntry.Binding = 0;
    bindlessEntry.Parameter = render::ShaderParameterInfo{
        .Name = "BindlessResources",
        .Kind = render::ShaderParameterKind::BindlessArray,
        .Stages = render::ShaderStage::Pixel,
        .Type = render::ResourceBindType::Buffer,
        .Count = 0,
        .IsBindless = true};
    FakePipelineLayout bindlessLayout{{render::BindingGroupLayout{
        .GroupIndex = 6,
        .Entries = {std::move(bindlessEntry)}}}};
    CompiledShaderVariant bindlessVariant = MakeVariant(shader, reflected, bindlessLayout);
    bindlessVariant.Key.SourceVersion = 2;
    const ShaderBindingPlan* bindlessPlan = library.GetOrCreate(shader, 0, bindlessVariant).Get();
    ASSERT_NE(bindlessPlan, nullptr);
    EXPECT_FALSE(bindlessPlan->Valid);
    EXPECT_EQ(bindlessPlan->ErrorGroup, 6u);
    EXPECT_EQ(bindlessPlan->ErrorBinding, 0u);

    FakePipelineLayout pushLayout{
        {},
        {render::PushConstantRange{
            .Name = "MaterialPush",
            .Group = 0,
            .Binding = 0,
            .Stages = render::ShaderStage::Pixel,
            .Offset = 0,
            .Size = 16}}};
    CompiledShaderVariant pushVariant = MakeVariant(shader, reflected, pushLayout);
    pushVariant.Key.SourceVersion = 3;
    const ShaderBindingPlan* pushPlan = library.GetOrCreate(shader, 0, pushVariant).Get();
    ASSERT_NE(pushPlan, nullptr);
    EXPECT_FALSE(pushPlan->Valid);
    EXPECT_NE(pushPlan->Error.find("push constant"), string::npos);
}
