#include <type_traits>

#include <gtest/gtest.h>

#include <radray/runtime_type.h>

#if !defined(__cpp_rtti) && !defined(_CPPRTTI)
#error "RadRay C++ consumers must be compiled with RTTI enabled."
#endif

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

static_assert(std::is_same_v<RuntimeTypeId, Guid>);
static_assert(runtime_type_id_v<StableType> == RuntimeTypeTrait<StableType>::value);
static_assert(runtime_type_id_v<const StableType> == runtime_type_id_v<StableType>);
static_assert(runtime_type_id_v<volatile StableType> == runtime_type_id_v<StableType>);
static_assert(runtime_type_id_v<const volatile StableType&> == runtime_type_id_v<StableType>);
static_assert(runtime_type_id_v<StableType&&> == runtime_type_id_v<StableType>);

TEST(RuntimeTypeIdTest, PreservesFixedSpecializationAndNormalizesQualifiers) {
    constexpr RuntimeTypeId expected{
        0x11111111, 0x0001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};

    EXPECT_EQ(runtime_type_id_v<StableType>, expected);
    EXPECT_EQ(runtime_type_id_v<const StableType&>, expected);
    EXPECT_EQ(runtime_type_id_v<volatile StableType&&>, expected);
}
