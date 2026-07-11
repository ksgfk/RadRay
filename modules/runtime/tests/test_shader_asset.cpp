#include <gtest/gtest.h>

#include <radray/runtime/shader_asset.h>

using namespace radray;

namespace {

std::string_view SV(const char* s) noexcept {
    return std::string_view{s};
}

class FakePipelineLayout final : public render::PipelineLayout {
public:
    explicit FakePipelineLayout(vector<render::BindingGroupLayout> groups) noexcept
        : _groups(std::move(groups)) {}

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

    Nullable<const render::ShaderParameterInfo*> FindParameter(std::string_view name) const noexcept override {
        for (const auto& group : _groups) {
            for (const auto& entry : group.Entries) {
                if (!entry.IsStaticSampler && entry.Parameter.Name == name) {
                    return &entry.Parameter;
                }
            }
        }
        return nullptr;
    }

    std::optional<render::ShaderBindingLocation> FindBindingLocation(std::string_view name) const noexcept override {
        for (const auto& group : _groups) {
            for (const auto& entry : group.Entries) {
                if (!entry.IsStaticSampler && entry.Parameter.Name == name) {
                    return render::ShaderBindingLocation{group.GroupIndex, entry.Binding};
                }
            }
        }
        return std::nullopt;
    }

    vector<render::BindingGroupLayout> GetBindingGroupLayouts() const noexcept override { return _groups; }
    vector<render::PushConstantRange> GetPushConstantRanges() const noexcept override { return {}; }

private:
    vector<render::BindingGroupLayout> _groups;
};

FakePipelineLayout MakeDynamicObjectLayout() {
    render::ShaderParameterInfo parameter{
        .Name = "gPerObject",
        .Kind = render::ShaderParameterKind::Resource,
        .Stages = render::ShaderStage::Vertex,
        .Type = render::ResourceBindType::CBuffer,
        .Count = 1,
        .ByteSize = 64};
    return FakePipelineLayout{{render::BindingGroupLayout{
        .GroupIndex = 0,
        .Entries = {render::BindingGroupLayoutEntry{
            .Parameter = parameter,
            .Binding = 1,
            .HasDynamicOffset = true}}}}};
}

class CapturingVariantLibrary final : public ShaderVariantLibrary {
public:
    Nullable<const CompiledShaderVariant*> Find(const ShaderVariantKey&) const noexcept override {
        return nullptr;
    }

    Nullable<const CompiledShaderVariant*> GetOrCreate(
        const ShaderVariantDescriptor& desc) noexcept override {
        Masks.push_back(desc.KeywordBitmask);
        return &_variant;
    }

    void Clear() noexcept override { Masks.clear(); }
    uint32_t Count() const noexcept override { return static_cast<uint32_t>(Masks.size()); }
    ShaderVariantLibraryStats GetStats() const noexcept override { return {}; }

    vector<uint64_t> Masks;

private:
    CompiledShaderVariant _variant{};
};

}  // namespace

TEST(ShaderKeywordSetTest, AddAssignsSequentialBits) {
    ShaderKeywordSet kw;
    auto a = kw.Add("FEATURE_A");
    auto b = kw.Add("FEATURE_B");
    auto c = kw.Add("FEATURE_C");
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    ASSERT_TRUE(c.has_value());
    EXPECT_EQ(a.value(), 0u);
    EXPECT_EQ(b.value(), 1u);
    EXPECT_EQ(c.value(), 2u);
    EXPECT_EQ(kw.Count(), 3u);
}

TEST(ShaderKeywordSetTest, AddRejectsDuplicate) {
    ShaderKeywordSet kw;
    ASSERT_TRUE(kw.Add("FEATURE_A").has_value());
    EXPECT_FALSE(kw.Add("FEATURE_A").has_value());
    EXPECT_EQ(kw.Count(), 1u);
}

TEST(ShaderKeywordSetTest, AddRejectsBeyondLimit) {
    ShaderKeywordSet kw;
    for (uint32_t i = 0; i < ShaderKeywordSet::kMaxKeywords; ++i) {
        ASSERT_TRUE(kw.Add(fmt::format("KW_{}", i)).has_value());
    }
    EXPECT_EQ(kw.Count(), ShaderKeywordSet::kMaxKeywords);
    EXPECT_FALSE(kw.Add("KW_OVERFLOW").has_value());
    EXPECT_EQ(kw.Count(), ShaderKeywordSet::kMaxKeywords);
}

TEST(ShaderKeywordSetTest, IndexOf) {
    ShaderKeywordSet kw;
    kw.Add("A");
    kw.Add("B");
    EXPECT_EQ(kw.IndexOf("A").value(), 0u);
    EXPECT_EQ(kw.IndexOf("B").value(), 1u);
    EXPECT_FALSE(kw.IndexOf("MISSING").has_value());
}

TEST(ShaderKeywordSetTest, ProjectComputesBitmask) {
    ShaderKeywordSet kw;
    kw.Add("A");  // bit 0
    kw.Add("B");  // bit 1
    kw.Add("C");  // bit 2

    EXPECT_EQ(kw.Project(std::span<const std::string_view>{}), 0ull);

    std::string_view ac[] = {SV("A"), SV("C")};
    EXPECT_EQ(kw.Project(ac), 0b101ull);

    std::string_view all[] = {SV("A"), SV("B"), SV("C")};
    EXPECT_EQ(kw.Project(all), 0b111ull);
}

TEST(ShaderKeywordSetTest, ProjectIgnoresUnknownKeywords) {
    ShaderKeywordSet kw;
    kw.Add("A");  // bit 0
    kw.Add("B");  // bit 1

    std::string_view mixed[] = {SV("A"), SV("UNKNOWN"), SV("B")};
    EXPECT_EQ(kw.Project(mixed), 0b11ull);

    std::string_view onlyUnknown[] = {SV("NOPE")};
    EXPECT_EQ(kw.Project(onlyUnknown), 0ull);
}

TEST(ShaderKeywordSetTest, ResolveDefines) {
    ShaderKeywordSet kw;
    kw.Add("FEATURE_A");  // bit 0
    kw.Add("FEATURE_B");  // bit 1
    kw.Add("FEATURE_C");  // bit 2

    auto none = kw.ResolveDefines(0);
    EXPECT_TRUE(none.empty());

    auto ac = kw.ResolveDefines(0b101);
    ASSERT_EQ(ac.size(), 2u);
    EXPECT_EQ(ac[0], "FEATURE_A=1");
    EXPECT_EQ(ac[1], "FEATURE_C=1");
}

TEST(ShaderKeywordSetTest, ProjectResolveRoundTrip) {
    ShaderKeywordSet kw;
    kw.Add("X");
    kw.Add("Y");
    kw.Add("Z");
    std::string_view enabled[] = {SV("Z"), SV("X")};
    const uint64_t mask = kw.Project(enabled);
    auto defines = kw.ResolveDefines(mask);
    ASSERT_EQ(defines.size(), 2u);
    // ResolveDefines 按 bit 序输出 (与加入顺序一致), 不随 enabled 顺序变化。
    EXPECT_EQ(defines[0], "X=1");
    EXPECT_EQ(defines[1], "Z=1");
}

TEST(ShaderAssetTest, HasNonEmptyProgramId) {
    ShaderAsset a;
    ShaderAsset b;
    EXPECT_FALSE(a.GetProgramId().IsEmpty());
    EXPECT_FALSE(b.GetProgramId().IsEmpty());
    // 每个 ShaderAsset 有独立身份。
    EXPECT_NE(a.GetProgramId(), b.GetProgramId());
}

TEST(ShaderAssetTest, GetTypeId) {
    ShaderAsset a;
    EXPECT_EQ(a.GetTypeId(), runtime_type_id_v<ShaderAsset>);
    EXPECT_FALSE(a.GetTypeId().IsEmpty());
}

TEST(ShaderAssetTest, FindPassByTag) {
    ShaderKeywordSet kw;
    vector<ShaderPassDesc> passes;
    passes.push_back(ShaderPassDesc{.PassTag = "ForwardLit", .Source = ""});
    passes.push_back(ShaderPassDesc{.PassTag = "ShadowCaster", .Source = ""});
    ShaderAsset asset{std::move(kw), std::move(passes)};

    EXPECT_EQ(asset.FindPassByTag("ForwardLit").value(), 0u);
    EXPECT_EQ(asset.FindPassByTag("ShadowCaster").value(), 1u);
    EXPECT_FALSE(asset.FindPassByTag("DoesNotExist").has_value());
}

TEST(ShaderAssetTest, AppliesPassLocalKeywordMask) {
    ShaderKeywordSet keywords;
    keywords.Add("A");
    keywords.Add("B");
    vector<ShaderPassDesc> passes;
    ShaderPassDesc forward{.PassTag = "Forward", .Source = ""};
    forward.VariantKeywordMask = uint64_t{1} << 0u;
    passes.push_back(std::move(forward));
    ShaderPassDesc shadow{.PassTag = "Shadow", .Source = ""};
    shadow.VariantKeywordMask = 0;
    passes.push_back(std::move(shadow));
    ShaderAsset asset{std::move(keywords), std::move(passes)};
    CapturingVariantLibrary library;
    const std::string_view enabled[] = {"A", "B"};

    ASSERT_TRUE(asset.GetOrCreateVariant(library, 0, enabled).HasValue());
    ASSERT_TRUE(asset.GetOrCreateVariant(library, 1, enabled).HasValue());
    ASSERT_EQ(library.Masks.size(), 2u);
    EXPECT_EQ(library.Masks[0], 0b01u);
    EXPECT_EQ(library.Masks[1], 0u);
}

TEST(ShaderVariantKeyTest, IncludesCanonicalBindingPolicy) {
    const ShaderVariantStageDesc stage{
        .Source = "void VSMain() {}",
        .EntryPoint = "VSMain",
        .Stage = render::ShaderStage::Vertex};
    ShaderVariantDescriptor desc{};
    desc.ProgramId = Guid::NewGuid();
    desc.Stages = std::span{&stage, 1};

    const render::DynamicBufferBinding dynamicA{.Group = 1, .Binding = 2};
    desc.DynamicBufferBindings = std::span{&dynamicA, 1};
    const auto dynamicKey = BuildShaderVariantKey(desc, render::RenderBackend::D3D12);
    ASSERT_TRUE(dynamicKey.has_value());

    desc.DynamicBufferBindings = {};
    const auto ordinaryKey = BuildShaderVariantKey(desc, render::RenderBackend::D3D12);
    ASSERT_TRUE(ordinaryKey.has_value());
    EXPECT_NE(dynamicKey.value(), ordinaryKey.value());

    const render::PushConstantBinding push{.Group = 0, .Binding = 0};
    desc.PushConstantBindings = std::span{&push, 1};
    const auto pushKey = BuildShaderVariantKey(desc, render::RenderBackend::D3D12);
    ASSERT_TRUE(pushKey.has_value());
    EXPECT_NE(pushKey.value(), ordinaryKey.value());

    const render::DynamicBufferBinding bindingsA[] = {
        {.Group = 2, .Binding = 4},
        {.Group = 0, .Binding = 1}};
    const render::DynamicBufferBinding bindingsB[] = {
        {.Group = 0, .Binding = 1},
        {.Group = 2, .Binding = 4}};
    desc.PushConstantBindings = {};
    desc.DynamicBufferBindings = bindingsA;
    const auto orderedA = BuildShaderVariantKey(desc, render::RenderBackend::Vulkan);
    desc.DynamicBufferBindings = bindingsB;
    const auto orderedB = BuildShaderVariantKey(desc, render::RenderBackend::Vulkan);
    ASSERT_TRUE(orderedA.has_value());
    ASSERT_TRUE(orderedB.has_value());
    EXPECT_EQ(orderedA.value(), orderedB.value());
}

TEST(ShaderInterfaceSchemaTest, ValidatesDynamicCBufferAndOptionalBindings) {
    FakePipelineLayout layout = MakeDynamicObjectLayout();
    ShaderInterfaceSchema schema{};
    schema.Bindings.push_back(ShaderInterfaceBinding{
        .Name = "gPerObject",
        .Group = 0,
        .Binding = 1,
        .Kind = render::ShaderParameterKind::Resource,
        .Type = render::ResourceBindType::CBuffer,
        .Count = 1,
        .Stages = render::ShaderStage::Vertex,
        .HasDynamicOffset = true});
    schema.Bindings.push_back(ShaderInterfaceBinding{
        .Name = "gOptionalTexture",
        .Group = 1,
        .Binding = 0,
        .Kind = render::ShaderParameterKind::Resource,
        .Type = render::ResourceBindType::Texture,
        .Count = 1,
        .Stages = render::ShaderStage::Pixel,
        .Required = false});

    string error;
    EXPECT_TRUE(ValidateShaderInterfaceSchema(schema, layout, &error)) << error;
}

TEST(ShaderBindingLocationTest, UsesDeclaredLocationWhenReflectionOmitsBinding) {
    CompiledShaderVariant variant{};
    variant.DeclaredBindings.push_back(CompiledShaderVariant::DeclaredBinding{
        .Name = "gBaseColorMap",
        .Location = render::ShaderBindingLocation{.Group = 2, .Binding = 1}});

    const auto location = FindShaderBindingLocation(variant, "gBaseColorMap");
    ASSERT_TRUE(location.has_value());
    EXPECT_EQ(location->Group, 2u);
    EXPECT_EQ(location->Binding, 1u);
}

TEST(ShaderInterfaceSchemaTest, RejectsAbiMismatchAndUndeclaredBinding) {
    FakePipelineLayout layout = MakeDynamicObjectLayout();
    ShaderInterfaceSchema mismatch{};
    mismatch.Bindings.push_back(ShaderInterfaceBinding{
        .Name = "gPerObject",
        .Group = 0,
        .Binding = 1,
        .Kind = render::ShaderParameterKind::Resource,
        .Type = render::ResourceBindType::Texture,
        .Count = 1,
        .Stages = render::ShaderStage::Vertex,
        .HasDynamicOffset = true});
    EXPECT_FALSE(ValidateShaderInterfaceSchema(mismatch, layout));

    ShaderInterfaceSchema empty{};
    EXPECT_FALSE(ValidateShaderInterfaceSchema(empty, layout));
    empty.AllowAdditionalBindings = true;
    EXPECT_TRUE(ValidateShaderInterfaceSchema(empty, layout));
}
