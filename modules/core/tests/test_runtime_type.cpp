#include <gtest/gtest.h>

#include <radray/runtime_type.h>

namespace {

struct StableType {};

}  // namespace

namespace radray {

template <>
struct RuntimeTypeTrait<StableType> {
    static constexpr RuntimeTypeId value{
        0x11111111, 0x0001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
};

}  // namespace radray

using namespace radray;

TEST(RuntimeTypeIdTest, ReturnsSpecializedIdForCvRefQualifiedTypes) {
    constexpr RuntimeTypeId expected{
        0x11111111, 0x0001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

    EXPECT_EQ(runtime_type_id_v<StableType>, expected);
    EXPECT_EQ(runtime_type_id_v<const StableType&>, expected);
    EXPECT_EQ(runtime_type_id_v<volatile StableType&&>, expected);
}
