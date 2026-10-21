#include <gtest/gtest.h>

#include <cstring>
#include <optional>

#include <radray/basic_math.h>
#include <radray/structured_buffer.h>

using namespace radray;

namespace {

// 建一个与 MaterialParameterLayout::CreateStorageTemplate 同形的存储:
// 单根 cbuffer,字段各为独立原子类型,按反射偏移落位。
//   offset 0:  float4 BaseColor
//   offset 16: float4 Extra
//   offset 32: float  Scalar
std::optional<StructuredBufferStorage> BuildCb() {
    StructuredBufferStorage::Builder builder{};
    StructuredBufferId cb = builder.AddType("CB", 48);
    StructuredBufferId f4a = builder.AddType("BaseColor", 16);
    StructuredBufferId f4b = builder.AddType("Extra", 16);
    StructuredBufferId f1 = builder.AddType("Scalar", 4);
    builder.AddMemberForType(cb, f4a, "BaseColor", 0);
    builder.AddMemberForType(cb, f4b, "Extra", 16);
    builder.AddMemberForType(cb, f1, "Scalar", 32);
    builder.AddRoot("gCB", cb);
    return builder.Build();
}

}  // namespace

// ① 按名写一个不存在的字段名 → TrySetValue 返回 false(而非静默吞掉)。
TEST(StructuredBufferTest, TrySetValueUnknownFieldFails) {
    auto storageOpt = BuildCb();
    ASSERT_TRUE(storageOpt.has_value());
    StructuredBufferStorage& storage = storageOpt.value();
    auto root = storage.GetVar("gCB");
    ASSERT_TRUE(root.IsValid());

    const Eigen::Vector4f v{1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_FALSE(root.TrySetValue("DoesNotExist", v));
    // 已知字段仍成功。
    EXPECT_TRUE(root.TrySetValue("BaseColor", v));
}

// ② 用与反射字段大小不符的类型写入 → TrySetValue 返回 false。
TEST(StructuredBufferTest, TrySetValueSizeMismatchFails) {
    auto storageOpt = BuildCb();
    ASSERT_TRUE(storageOpt.has_value());
    StructuredBufferStorage& storage = storageOpt.value();
    auto root = storage.GetVar("gCB");
    ASSERT_TRUE(root.IsValid());

    // 把 float(4B)写进 float4(16B)字段 → 尺寸不符,响亮失败。
    const float tooSmall = 1.0f;
    EXPECT_FALSE(root.TrySetValue("BaseColor", tooSmall));

    // 把 float4(16B)写进 float(4B)字段 → 尺寸不符,响亮失败。
    const Eigen::Vector4f tooBig{1.0f, 2.0f, 3.0f, 4.0f};
    EXPECT_FALSE(root.TrySetValue("Scalar", tooBig));

    // 匹配大小则成功。
    EXPECT_TRUE(root.TrySetValue("Scalar", 5.0f));
    EXPECT_TRUE(root.TrySetValue("BaseColor", tooBig));
}

// ③ 句柄缓存命中后写入结果与按名写入逐字节一致。
TEST(StructuredBufferTest, CachedHandleWriteMatchesByName) {
    auto storageByName = BuildCb();
    auto storageByHandle = BuildCb();
    ASSERT_TRUE(storageByName.has_value());
    ASSERT_TRUE(storageByHandle.has_value());

    const Eigen::Vector4f base{0.25f, 0.5f, 0.75f, 1.0f};
    const Eigen::Vector4f extra{-1.0f, -2.0f, -3.0f, -4.0f};
    const float scalar = 42.0f;

    // 路径 A:每次按名 GetVar 写入。
    {
        auto root = storageByName->GetVar("gCB");
        ASSERT_TRUE(root.GetVar("BaseColor").TrySetValue(base));
        ASSERT_TRUE(root.GetVar("Extra").TrySetValue(extra));
        ASSERT_TRUE(root.GetVar("Scalar").TrySetValue(scalar));
    }

    // 路径 B:解析一次取稳定 globalId 句柄,之后用句柄复用写入。
    {
        auto root = storageByHandle->GetVar("gCB");
        StructuredBufferId hBase = root.GetVar("BaseColor").GetId();
        StructuredBufferId hExtra = root.GetVar("Extra").GetId();
        StructuredBufferId hScalar = root.GetVar("Scalar").GetId();
        ASSERT_TRUE(StructuredBufferView(&storageByHandle.value(), hBase).TrySetValue(base));
        ASSERT_TRUE(StructuredBufferView(&storageByHandle.value(), hExtra).TrySetValue(extra));
        ASSERT_TRUE(StructuredBufferView(&storageByHandle.value(), hScalar).TrySetValue(scalar));
    }

    std::span<const byte> a = storageByName->GetData();
    std::span<const byte> b = storageByHandle->GetData();
    ASSERT_EQ(a.size(), b.size());
    EXPECT_EQ(std::memcmp(a.data(), b.data(), a.size()), 0);
}
