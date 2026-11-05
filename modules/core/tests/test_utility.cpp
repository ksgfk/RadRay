#include <radray/utility.h>

#include <cstring>

#include <gtest/gtest.h>

namespace radray {
namespace {

struct Base {
    virtual ~Base() noexcept = default;
};

struct Derived final : Base {
    int Value{5};
};

vector<byte> BytesOf(const vector<uint32_t>& words) {
    vector<byte> bytes(words.size() * sizeof(uint32_t));
    std::memcpy(bytes.data(), words.data(), bytes.size());
    return bytes;
}

TEST(UtilityTest, ByteToDWORDKeepsByteOrderAndZeroPadsTail) {
    const vector<uint8_t> input{1, 2, 3, 4, 5, 6};

    const vector<uint32_t> words = ByteToDWORD(input);

    ASSERT_EQ(words.size(), 2u);
    const vector<byte> expected{byte{1}, byte{2}, byte{3}, byte{4}, byte{5}, byte{6}, byte{0}, byte{0}};
    EXPECT_EQ(BytesOf(words), expected);
}

TEST(UtilityTest, ByteToDWORDOfEmptyInputIsEmpty) {
    EXPECT_TRUE(ByteToDWORD({}).empty());
}

TEST(UtilityTest, StaticCastUniquePtrTransfersOwnership) {
    unique_ptr<Base> base = make_unique<Derived>();
    Base* raw = base.get();

    const unique_ptr<Derived> derived = StaticCastUniquePtr<Derived>(std::move(base));

    EXPECT_EQ(base, nullptr);  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(derived.get(), raw);
    EXPECT_EQ(derived->Value, 5);
}

}  // namespace
}  // namespace radray
