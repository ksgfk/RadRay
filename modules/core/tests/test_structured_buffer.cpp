#include <gtest/gtest.h>

#include <cstring>
#include <optional>

#include <radray/basic_math.h>
#include <radray/structured_buffer.h>

using namespace radray;

namespace {

// gCB: float4 BaseColor @0, float4 Extra @16, float Scalar @32, size 48.
std::optional<StructuredBufferStorage> BuildConstantBuffer() {
    StructuredBufferStorage::Builder builder{};
    const StructuredBufferId cb = builder.AddType("CB", 48);
    const StructuredBufferId baseColor = builder.AddType("float4", 16);
    const StructuredBufferId scalar = builder.AddType("float", 4);
    builder.AddMemberForType(cb, baseColor, "BaseColor", 0);
    builder.AddMemberForType(cb, baseColor, "Extra", 16);
    builder.AddMemberForType(cb, scalar, "Scalar", 32);
    builder.AddRoot("gCB", cb);
    return builder.Build();
}

template <class T>
T ReadAt(std::span<const byte> data, size_t offset) {
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

}  // namespace

TEST(StructuredBufferTest, WritesFieldsAtReflectedOffsets) {
    auto storage = BuildConstantBuffer();
    ASSERT_TRUE(storage.has_value());
    auto root = storage->GetVar("gCB");
    ASSERT_TRUE(root.IsValid());

    ASSERT_TRUE(root.TrySetValue("Extra", Eigen::Vector4f{1.0f, 2.0f, 3.0f, 4.0f}));
    ASSERT_TRUE(root.TrySetValue("Scalar", 5.0f));

    const auto data = storage->GetData();
    ASSERT_EQ(data.size(), 48u);
    EXPECT_EQ(ReadAt<float>(data, 16), 1.0f);
    EXPECT_EQ(ReadAt<float>(data, 28), 4.0f);
    EXPECT_EQ(ReadAt<float>(data, 32), 5.0f);
    EXPECT_EQ(ReadAt<float>(data, 0), 0.0f);
}

TEST(StructuredBufferTest, UnknownNamesReturnInvalidViews) {
    auto storage = BuildConstantBuffer();
    ASSERT_TRUE(storage.has_value());

    EXPECT_FALSE(storage->GetVar("Missing").IsValid());
    auto root = storage->GetVar("gCB");
    EXPECT_FALSE(root.GetVar("Missing").IsValid());
    EXPECT_FALSE(root.TrySetValue("Missing", 1.0f));
}

TEST(StructuredBufferTest, TrySetValueRejectsSizeMismatch) {
    auto storage = BuildConstantBuffer();
    ASSERT_TRUE(storage.has_value());
    auto root = storage->GetVar("gCB");
    const vector<byte> before(storage->GetData().begin(), storage->GetData().end());

    EXPECT_FALSE(root.TrySetValue("BaseColor", 1.0f));
    EXPECT_FALSE(root.TrySetValue("Scalar", Eigen::Vector4f{1.0f, 2.0f, 3.0f, 4.0f}));

    const auto after = storage->GetData();
    EXPECT_TRUE(std::equal(before.begin(), before.end(), after.begin(), after.end()));
}

TEST(StructuredBufferTest, InvalidViewRejectsWrites) {
    StructuredBufferView view{};

    EXPECT_FALSE(view.IsValid());
    EXPECT_FALSE(view.TrySetValue(1.0f));
    EXPECT_FALSE(view.TrySetValue("Scalar", 1.0f));
}

TEST(StructuredBufferTest, CachedGlobalIdWritesSameField) {
    auto storage = BuildConstantBuffer();
    ASSERT_TRUE(storage.has_value());
    const StructuredBufferId scalarId = storage->GetVar("gCB").GetVar("Scalar").GetId();

    ASSERT_TRUE(StructuredBufferView(&storage.value(), scalarId).TrySetValue(7.0f));

    EXPECT_EQ(ReadAt<float>(storage->GetData(), 32), 7.0f);
    EXPECT_EQ(ReadAt<float>(storage->GetSpan(scalarId), 0), 7.0f);
}

TEST(StructuredBufferTest, ArrayElementsAreStridedByElementSize) {
    StructuredBufferStorage::Builder builder{};
    const StructuredBufferId block = builder.AddType("Block", 16);
    const StructuredBufferId scalar = builder.AddType("float", 4);
    builder.AddMemberForType(block, scalar, "Values", 0, 4);
    builder.AddRoot("gBlock", block);
    auto storage = builder.Build();
    ASSERT_TRUE(storage.has_value());
    auto values = storage->GetVar("gBlock").GetVar("Values");
    ASSERT_TRUE(values.IsValid());

    ASSERT_TRUE(values.GetArrayElement(2).TrySetValue(3.0f));

    EXPECT_FALSE(values.GetArrayElement(4).IsValid());
    EXPECT_EQ(values.GetArrayElement(2).GetGlobalOffset(), 8u);
    EXPECT_EQ(ReadAt<float>(storage->GetData(), 8), 3.0f);
    EXPECT_EQ(ReadAt<float>(storage->GetSpan(values.GetId(), 2), 0), 3.0f);
}

TEST(StructuredBufferTest, AlignmentPadsEachRootAndTotalSize) {
    StructuredBufferStorage::Builder builder{};
    const StructuredBufferId scalar = builder.AddType("float", 4);
    builder.AddRoot("a", scalar);
    builder.AddRoot("b", scalar);
    builder.SetAlignment(16);
    auto storage = builder.Build();
    ASSERT_TRUE(storage.has_value());

    EXPECT_EQ(storage->GetVar("a").GetGlobalOffset(), 0u);
    EXPECT_EQ(storage->GetVar("b").GetGlobalOffset(), 16u);
    EXPECT_EQ(storage->GetData().size(), 32u);
}

TEST(StructuredBufferTest, BuildFailsForNonPowerOfTwoAlignment) {
    StructuredBufferStorage::Builder builder{};
    builder.AddRoot("a", builder.AddType("float", 4));
    builder.SetAlignment(12);

    EXPECT_FALSE(builder.IsValid());
    EXPECT_FALSE(builder.Build().has_value());
}

TEST(StructuredBufferTest, BuildFailsWhenMemberExceedsParent) {
    StructuredBufferStorage::Builder builder{};
    const StructuredBufferId parent = builder.AddType("Parent", 8);
    const StructuredBufferId scalar = builder.AddType("float", 4);
    builder.AddMemberForType(parent, scalar, "Tail", 6);
    builder.AddRoot("p", parent);

    EXPECT_FALSE(builder.IsValid());
    EXPECT_FALSE(builder.Build().has_value());
}

TEST(StructuredBufferTest, BuildFailsWhenArrayMemberExceedsParent) {
    StructuredBufferStorage::Builder builder{};
    const StructuredBufferId parent = builder.AddType("Parent", 8);
    const StructuredBufferId scalar = builder.AddType("float", 4);
    builder.AddMemberForType(parent, scalar, "Values", 0, 3);
    builder.AddRoot("p", parent);

    EXPECT_FALSE(builder.Build().has_value());
}
