#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <gtest/gtest.h>
#include <radray/hash.h>
#include <radray/types.h>

namespace {

constexpr size_t kConstexprHashCode = [] {
    radray::HashCode hash;
    hash.Add(size_t{1});
    hash.Add(size_t{2});
    return hash.ToHashCode();
}();
static_assert(kConstexprHashCode != 0);

constexpr size_t kConstexprCombinedHash = radray::HashCode::Combine(size_t{1}, size_t{2});
static_assert(kConstexprCombinedHash == kConstexprHashCode);

struct PodKey {
    uint64_t A;
    uint32_t B;
    uint32_t C;
    char Name[16];
};

PodKey MakeKey(uint64_t a, uint32_t b, uint32_t c, const char* name) noexcept {
    PodKey key{};  // 清零, 保证 padding 恒为 0
    key.A = a;
    key.B = b;
    key.C = c;
    std::strncpy(key.Name, name, sizeof(key.Name) - 1);
    return key;
}

}  // namespace

TEST(PodHashTest, SameValueSameHash) {
    PodKey k1 = MakeKey(42, 7, 9, "foo");
    PodKey k2 = MakeKey(42, 7, 9, "foo");
    radray::PodHasher<PodKey> hasher{};
    EXPECT_EQ(hasher(k1), hasher(k2));
}

TEST(PodHashTest, DifferentValueDifferentBytes) {
    PodKey k1 = MakeKey(42, 7, 9, "foo");
    PodKey k2 = MakeKey(42, 7, 10, "foo");
    radray::PodEqual<PodKey> eq{};
    EXPECT_FALSE(eq(k1, k2));
}

TEST(PodHashTest, EqualIsByteWise) {
    PodKey k1 = MakeKey(1, 2, 3, "bar");
    PodKey k2 = MakeKey(1, 2, 3, "bar");
    radray::PodEqual<PodKey> eq{};
    EXPECT_TRUE(eq(k1, k2));
}

TEST(PodHashTest, PaddingZeroedIsStable) {
    // 两个独立构造的相同逻辑 key, 未赋值的尾部字节应因 `PodKey{}` 清零而一致.
    PodKey k1 = MakeKey(100, 200, 300, "x");
    PodKey k2 = MakeKey(100, 200, 300, "x");
    EXPECT_EQ(std::memcmp(&k1, &k2, sizeof(PodKey)), 0);
}

TEST(PodHashTest, UsableAsUnorderedMapKey) {
    std::unordered_map<PodKey, int, radray::PodHasher<PodKey>, radray::PodEqual<PodKey>> map;
    map[MakeKey(1, 0, 0, "a")] = 1;
    map[MakeKey(2, 0, 0, "b")] = 2;
    map[MakeKey(1, 0, 0, "a")] = 3;  // 覆盖同 key
    EXPECT_EQ(map.size(), 2u);
    EXPECT_EQ(map[MakeKey(1, 0, 0, "a")], 3);
    EXPECT_EQ(map[MakeKey(2, 0, 0, "b")], 2);
}

TEST(HashCodeTest, MatchesHashDataForWordSequence) {
    const size_t values[] = {
        0x01234567u,
        0x89abcdefu,
        0x13579bdfu,
        0x2468ace0u,
        0x10203040u,
        0x50607080u,
        0xa0b0c0d0u,
        0xe0f00112u,
    };
    constexpr size_t valueCount = sizeof(values) / sizeof(values[0]);

    radray::HashCode hash;
    EXPECT_EQ(hash.ToHashCode(), radray::HashData(values, 0));
    for (size_t count = 1; count <= valueCount; ++count) {
        hash.Add(values[count - 1]);
        EXPECT_EQ(hash.ToHashCode(), radray::HashData(values, count * sizeof(size_t)));
    }
}

TEST(HashCodeTest, StaticCombineMatchesIncrementalHash) {
    radray::HashCode hash;
    hash.Add(size_t{0x12345678});
    hash.Add(size_t{0x9abcdef0});

    EXPECT_EQ(
        radray::HashCode::Combine(size_t{0x12345678}, size_t{0x9abcdef0}),
        hash.ToHashCode());
}
