#include <radray/hash.h>

#include <gtest/gtest.h>

namespace radray {
namespace {

size_t HashOf(std::initializer_list<size_t> values, size_t seed = 0) {
    HashCode hash{seed};
    for (size_t value : values) hash.Add(value);
    return hash.ToHashCode();
}

constexpr size_t kConstexprHash = [] {
    HashCode hash;
    hash.Add(size_t{1});
    hash.Add(size_t{2});
    return hash.ToHashCode();
}();
static_assert(HashCode::Combine(size_t{1}, size_t{2}) == kConstexprHash);

TEST(HashTest, HashCodeIsDeterministicAndOrderSensitive) {
    EXPECT_EQ(HashOf({1, 2, 3}), HashOf({1, 2, 3}));
    EXPECT_NE(HashOf({1, 2, 3}), HashOf({3, 2, 1}));
    EXPECT_NE(HashOf({1, 2, 3}), HashOf({1, 2, 3}, 7));
}

TEST(HashTest, HashCodeMatchesHashDataOverTheSameWords) {
    const size_t words[] = {0x01234567u, 0x89abcdefu, 0x13579bdfu, 0x2468ace0u, 0x10203040u, 0x50607080u, 0xa0b0c0d0u, 0xe0f00112u};
    HashCode hash;
    EXPECT_EQ(hash.ToHashCode(), HashData(words, 0));

    for (size_t count = 1; count <= std::size(words); ++count) {
        hash.Add(words[count - 1]);
        EXPECT_EQ(hash.ToHashCode(), HashData(words, count * sizeof(size_t))) << count;
    }
}

TEST(HashTest, HashCodeAddsNonIntegralValuesThroughStdHash) {
    HashCode byValue;
    byValue.Add(std::string_view{"radray"});
    HashCode byHash;
    byHash.Add(std::hash<std::string_view>{}("radray"));

    EXPECT_EQ(byValue.ToHashCode(), byHash.ToHashCode());
}

TEST(HashTest, StringHashSupportsHeterogeneousLookup) {
    unordered_map<string, int, StringHash, StringEqual> map;
    map.emplace("alpha", 1);

    EXPECT_NE(map.find(std::string_view{"alpha"}), map.end());
    EXPECT_EQ(map.find(std::string_view{"beta"}), map.end());
}

struct PodKey {
    uint64_t A;
    uint32_t B;
    uint32_t C;
};

TEST(HashTest, PodHasherAndEqualWorkAsMapKey) {
    unordered_map<PodKey, int, PodHasher<PodKey>, PodEqual<PodKey>> map;

    map[PodKey{1, 2, 3}] = 1;
    map[PodKey{1, 2, 4}] = 2;
    map[PodKey{1, 2, 3}] = 3;

    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ((map[PodKey{1, 2, 3}]), 3);
}

TEST(HashTest, HashData128DependsOnInput) {
    const char first[] = "first";
    const char second[] = "second";

    EXPECT_EQ(HashData128(first, sizeof(first)), HashData128(first, sizeof(first)));
    EXPECT_NE(HashData128(first, sizeof(first)), HashData128(second, sizeof(second)));
}

}  // namespace
}  // namespace radray
