#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <gtest/gtest.h>
#include <radray/hash.h>
#include <radray/types.h>

namespace {

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
