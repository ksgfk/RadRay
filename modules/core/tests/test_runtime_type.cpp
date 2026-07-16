#include <gtest/gtest.h>
#include <radray/runtime_type.h>

namespace {

// 关系图(只走 RuntimeTypeTrait::Bases,不依赖 C++ 继承):
//
//        IShape        IColored
//          ^              ^
//          |              |
//        Base ------------+
//          ^
//          |
//       Derived
//
// Unrelated 与上面全无关系。
struct IShape {};
struct IColored {};
struct Base {};
struct Derived {};
struct Unrelated {};

}  // namespace

namespace radray {

template <>
struct RuntimeTypeTrait<IShape> {
    static constexpr RuntimeTypeId value{0x11111111, 0x0001, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
};
template <>
struct RuntimeTypeTrait<IColored> {
    static constexpr RuntimeTypeId value{0x11111111, 0x0002, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02};
};
template <>
struct RuntimeTypeTrait<Base> {
    static constexpr RuntimeTypeId value{0x11111111, 0x0003, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03};
    using Bases = std::tuple<IShape, IColored>;
};
template <>
struct RuntimeTypeTrait<Derived> {
    static constexpr RuntimeTypeId value{0x11111111, 0x0004, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04};
    using Bases = std::tuple<Base>;
};
template <>
struct RuntimeTypeTrait<Unrelated> {
    static constexpr RuntimeTypeId value{0x11111111, 0x0005, 0x4000, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05};
};

}  // namespace radray

using namespace radray;

// 自反:T is-a T。
static_assert(runtime_is_a_v<IShape, IShape>);
static_assert(runtime_is_a_v<Derived, Derived>);

// 直接基类。
static_assert(runtime_is_a_v<Base, IShape>);
static_assert(runtime_is_a_v<Base, IColored>);
static_assert(runtime_is_a_v<Derived, Base>);

// 间接(多级)基类。
static_assert(runtime_is_a_v<Derived, IShape>);
static_assert(runtime_is_a_v<Derived, IColored>);

// 反向不成立。
static_assert(!runtime_is_a_v<IShape, Base>);
static_assert(!runtime_is_a_v<Base, Derived>);

// 兄弟接口互不成立。
static_assert(!runtime_is_a_v<IShape, IColored>);

// 完全无关。
static_assert(!runtime_is_a_v<Derived, Unrelated>);
static_assert(!runtime_is_a_v<Unrelated, IShape>);

// 未声明 Bases 的类型只对自身成立。
static_assert(runtime_is_a_v<Unrelated, Unrelated>);
static_assert(runtime_is_a_v<IShape, IShape>);

TEST(RuntimeTypeIsA, CompileTimeRelationsHold) {
    // 编译期断言已覆盖行为;运行期镜像一份保证测试用例被计入。
    EXPECT_TRUE((runtime_is_a_v<Derived, IShape>));
    EXPECT_TRUE((runtime_is_a_v<Derived, IColored>));
    EXPECT_TRUE((runtime_is_a_v<Derived, Base>));
    EXPECT_FALSE((runtime_is_a_v<Base, Derived>));
    EXPECT_FALSE((runtime_is_a_v<IShape, IColored>));
    EXPECT_FALSE((runtime_is_a_v<Derived, Unrelated>));
}

TEST(RuntimeTypeIsA, RuntimeTypeInfoPreservesExactIdAndMatchesBases) {
    const RuntimeTypeInfo& info = runtime_type_info_v<Derived>;
    EXPECT_EQ(info.Id, runtime_type_id_v<Derived>);
    EXPECT_TRUE(info.IsA(runtime_type_id_v<Derived>));
    EXPECT_TRUE(info.IsA(runtime_type_id_v<Base>));
    EXPECT_TRUE(info.IsA(runtime_type_id_v<IShape>));
    EXPECT_TRUE(info.IsA(runtime_type_id_v<IColored>));
    EXPECT_FALSE(info.IsA(runtime_type_id_v<Unrelated>));
}
