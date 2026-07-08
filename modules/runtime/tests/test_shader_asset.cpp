#include <gtest/gtest.h>

#include <radray/runtime/shader_asset.h>

using namespace radray;

namespace {

std::string_view SV(const char* s) noexcept {
    return std::string_view{s};
}

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
