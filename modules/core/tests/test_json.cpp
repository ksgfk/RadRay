#include <gtest/gtest.h>

#include <string_view>

#include <radray/json.h>
#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

namespace radray {
namespace {

TEST(JsonTest, ParsedStringsUseConfiguredAllocatorAndSurviveDocumentMove) {
    auto document = JsonDocument::Parse(R"({"payload":"allocator-owned string"})");
    ASSERT_TRUE(document.has_value());
    JsonDocument moved = std::move(*document);
    const auto value = moved.Root()["payload"].AsString();
    EXPECT_EQ(value, "allocator-owned string");
#ifdef RADRAY_ENABLE_MIMALLOC
    EXPECT_TRUE(mi_any_heap_contains(value.data()));
#endif
}

TEST(JsonTest, LargeEscapedOutputSurvivesWriterMoveAndRepeatedWrites) {
    const string payload(32768, '\n');
    JsonWriter original;
    original.RootObject().AddString("payload", payload);
    JsonWriter writer = std::move(original);
    for (bool pretty : {false, true}) {
        const auto text = writer.Write(pretty);
        ASSERT_TRUE(text.has_value());
        auto document = JsonDocument::Parse(*text);
        ASSERT_TRUE(document.has_value());
        EXPECT_EQ(document->Root()["payload"].AsString(), payload);
    }
}

TEST(JsonTest, SupportsNonNullTerminatedStringViews) {
    constexpr std::string_view stringKey{"string.trailing", 6};
    constexpr std::string_view uintKey{"uint.trailing", 4};
    constexpr std::string_view intKey{"int.trailing", 3};
    constexpr std::string_view boolKey{"bool.trailing", 4};
    constexpr std::string_view doubleKey{"double.trailing", 6};
    constexpr std::string_view objectKey{"object.trailing", 6};
    constexpr std::string_view arrayKey{"array.trailing", 5};
    constexpr std::string_view stringValue{"value.trailing", 5};

    JsonWriter writer;
    JsonRef root = writer.RootObject();
    root.AddString(stringKey, stringValue);
    root.AddUint(uintKey, 42);
    root.AddInt(intKey, -7);
    root.AddBool(boolKey, true);
    root.AddDouble(doubleKey, 1.25);
    root.AddObject(objectKey).AddBool("nested", true);
    root.AddArray(arrayKey).AppendString(stringValue);
    root.AddString({}, {});

    std::optional<string> text = writer.Write(false);
    ASSERT_TRUE(text.has_value());

    std::optional<JsonDocument> document = JsonDocument::Parse(text.value());
    ASSERT_TRUE(document.has_value());
    JsonValue parsedRoot = document->Root();

    EXPECT_TRUE(parsedRoot.Has(stringKey));
    EXPECT_EQ(parsedRoot[stringKey].AsString(), "value");
    EXPECT_EQ(parsedRoot[uintKey].AsUint(), 42u);
    EXPECT_EQ(parsedRoot[intKey].AsInt(), -7);
    EXPECT_TRUE(parsedRoot[boolKey].AsBool());
    EXPECT_DOUBLE_EQ(parsedRoot[doubleKey].AsDouble(), 1.25);
    EXPECT_TRUE(parsedRoot[objectKey]["nested"].AsBool());
    EXPECT_EQ(parsedRoot[arrayKey].At(0).AsString(), "value");
    EXPECT_EQ(parsedRoot[std::string_view{}].AsString(), "");

    EXPECT_FALSE(parsedRoot.Has("string.trailing"));
    EXPECT_FALSE(parsedRoot.Has("uint.trailing"));
    EXPECT_FALSE(parsedRoot.Has("object.trailing"));
}

}  // namespace
}  // namespace radray
