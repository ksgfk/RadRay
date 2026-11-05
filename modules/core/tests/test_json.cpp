#include <radray/json.h>

#include <filesystem>
#include <limits>

#include <gtest/gtest.h>

#ifdef RADRAY_ENABLE_MIMALLOC
#include <mimalloc.h>
#endif

namespace radray {
namespace json_test {

enum class Mode : int16_t {
    Disabled = -1,
    Enabled = 2,
};

enum class Access : uint8_t {
    Read = 1,
    Write = 2,
};

struct Child {
    string Name;
    Mode Value{Mode::Disabled};
};

struct Parent {
    string Name;
    vector<Child> Children;
    std::optional<uint32_t> Limit;
    std::optional<uint32_t> ExplicitNull;
    uint32_t Count{0};
};

struct Unsupported {};

}  // namespace json_test

template <>
struct is_flags<json_test::Access> : std::true_type {};

template <>
struct JsonSerializer<json_test::Child> {
    static bool Write(JsonWriteContext& context, const json_test::Child& value) noexcept {
        return SerializeJsonObject(context, value, JsonMember{"Name", &json_test::Child::Name}, JsonMember{"Value", &json_test::Child::Value});
    }
};

template <>
struct JsonDeserializer<json_test::Child> {
    static bool Read(const JsonValue& json, json_test::Child& value) noexcept {
        return DeserializeJsonObject(json, value, JsonMember{"Name", &json_test::Child::Name}, JsonMember{"Value", &json_test::Child::Value});
    }
};

template <>
struct JsonSerializer<json_test::Parent> {
    static bool Write(JsonWriteContext& context, const json_test::Parent& value) noexcept {
        JsonObjectWriter object = context.BeginObject();
        return object.IsValid() &&
               object.Member("Name", value.Name) &&
               object.Member("Children", value.Children) &&
               object.OptionalMember("Limit", value.Limit) &&
               object.Member("ExplicitNull", value.ExplicitNull) &&
               object.Member("Count", value.Count);
    }
};

template <>
struct JsonDeserializer<json_test::Parent> {
    static bool Read(const JsonValue& json, json_test::Parent& value) noexcept {
        JsonObjectReader object{json};
        json_test::Parent decoded{};
        if (!object.IsValid() ||
            !object.Member("Name", decoded.Name) ||
            !object.Member("Children", decoded.Children) ||
            !object.OptionalMember("Limit", decoded.Limit) ||
            !object.Member("ExplicitNull", decoded.ExplicitNull) ||
            !object.Member("Count", decoded.Count)) {
            return false;
        }
        value = std::move(decoded);
        return true;
    }
};

static_assert(!json_serializable<json_test::Unsupported>);
static_assert(!json_deserializable<std::string_view>);

namespace {

using json_test::Access;
using json_test::Child;
using json_test::Mode;
using json_test::Parent;

TEST(JsonTest, ParseRejectsMalformedText) {
    EXPECT_FALSE(JsonDocument::Parse("{").has_value());
    EXPECT_FALSE(JsonDocument::Parse("").has_value());
}

TEST(JsonTest, AccessorsReturnDefaultsForMissingOrMismatchedNodes) {
    const auto document = JsonDocument::Parse(R"({"text":"x","list":[1]})");
    ASSERT_TRUE(document.has_value());
    const JsonValue root = document->Root();

    EXPECT_FALSE(root["missing"].IsValid());
    EXPECT_FALSE(root["list"].At(1).IsValid());
    EXPECT_FALSE(root["text"]["nested"].IsValid());
    EXPECT_EQ(root["text"].AsUint(7), 7u);
    EXPECT_EQ(root["missing"].AsString("fallback"), "fallback");
    EXPECT_EQ(root["text"].Size(), 0u);
    EXPECT_EQ(root["list"].Size(), 1u);
}

TEST(JsonTest, WriterOutputParsesBackWithNonNullTerminatedViews) {
    constexpr std::string_view key{"key.trailing", 3};
    constexpr std::string_view value{"value.trailing", 5};
    JsonWriter writer;
    JsonRef root = writer.RootObject();
    root.AddString(key, value);
    root.AddInt("int", -7);
    root.AddDouble("double", 1.25);
    root.AddObject("object").AddBool("nested", true);
    root.AddArray("array").AppendUint(42);

    const auto text = writer.Write(false);
    ASSERT_TRUE(text.has_value());
    const auto document = JsonDocument::Parse(*text);
    ASSERT_TRUE(document.has_value());
    const JsonValue parsed = document->Root();

    EXPECT_EQ(parsed["key"].AsString(), "value");
    EXPECT_FALSE(parsed.Has("key.trailing"));
    EXPECT_EQ(parsed["int"].AsInt(), -7);
    EXPECT_DOUBLE_EQ(parsed["double"].AsDouble(), 1.25);
    EXPECT_TRUE(parsed["object"]["nested"].AsBool());
    EXPECT_EQ(parsed["array"].At(0).AsUint(), 42u);
}

TEST(JsonTest, MovedWriterKeepsLargeEscapedPayload) {
    const string payload(32768, '\n');
    JsonWriter original;
    original.RootObject().AddString("payload", payload);

    const JsonWriter writer = std::move(original);

    for (bool pretty : {false, true}) {
        const auto text = writer.Write(pretty);
        ASSERT_TRUE(text.has_value());
        const auto document = JsonDocument::Parse(*text);
        ASSERT_TRUE(document.has_value());
        EXPECT_EQ(document->Root()["payload"].AsString(), payload);
    }
}

TEST(JsonTest, MovedDocumentKeepsValuesReadable) {
    auto document = JsonDocument::Parse(R"({"payload":"kept"})");
    ASSERT_TRUE(document.has_value());

    const JsonDocument moved = std::move(*document);

    EXPECT_EQ(moved.Root()["payload"].AsString(), "kept");
}

#ifdef RADRAY_ENABLE_MIMALLOC
TEST(JsonTest, ParsedDocumentIsAllocatedByMimalloc) {
    const auto document = JsonDocument::Parse(R"({"payload":"allocator-owned string"})");
    ASSERT_TRUE(document.has_value());

    EXPECT_TRUE(mi_any_heap_contains(document->Root()["payload"].AsString().data()));
}
#endif

TEST(JsonTest, WriteFileCreatesParentDirectoriesAndParseFileReadsIt) {
    const auto directory = std::filesystem::temp_directory_path() / "radray_core_json_test";
    const auto path = directory / "nested" / "value.json";
    std::filesystem::remove_all(directory);
    JsonWriter writer;
    writer.RootArray().AppendString("saved");

    ASSERT_TRUE(writer.WriteFile(path));
    const auto document = JsonDocument::ParseFile(path);

    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(document->Root().At(0).AsString(), "saved");
    std::filesystem::remove_all(directory);
}

TEST(JsonTest, ParseFileFailsForMissingFile) {
    EXPECT_FALSE(JsonDocument::ParseFile(std::filesystem::temp_directory_path() / "radray_core_json_missing.json").has_value());
}

TEST(JsonSerializerTest, SerializesScalarAndContainerRoots) {
    const array<uint32_t, 3> values{1, 2, 3};

    EXPECT_EQ(SerializeJson(uint32_t{42}, false), "42");
    EXPECT_EQ(SerializeJson(int8_t{-3}, false), "-3");
    EXPECT_EQ(SerializeJson(true, false), "true");
    EXPECT_EQ(SerializeJson(std::optional<uint32_t>{}, false), "null");
    EXPECT_EQ(SerializeJson(std::string_view{"value.trailing", 5}, false), R"("value")");
    EXPECT_EQ(SerializeJson(values, false), "[1,2,3]");
    EXPECT_EQ(SerializeJson(std::span{values}, false), "[1,2,3]");
    EXPECT_EQ(SerializeJson(vector<bool>{true, false}, false), "[true,false]");
}

TEST(JsonSerializerTest, SerializesNestedObjectsAndOmitsEmptyOptionalMembers) {
    const Parent value{
        .Name = "root",
        .Children = {{.Name = "first", .Value = Mode::Enabled}},
        .Limit = std::nullopt,
        .ExplicitNull = std::nullopt,
        .Count = 9,
    };

    EXPECT_EQ(SerializeJson(value, false), R"({"Name":"root","Children":[{"Name":"first","Value":"Enabled"}],"ExplicitNull":null,"Count":9})");
}

TEST(JsonSerializerTest, EnumsUseMemberNamesAndUnnamedValuesFail) {
    EXPECT_EQ(SerializeJson(Mode::Disabled, false), R"("Disabled")");
    EXPECT_FALSE(SerializeJson(static_cast<Mode>(7), false).has_value());
}

TEST(JsonSerializerTest, FlagsSerializeAsArrayOfBitNames) {
    EXPECT_EQ(SerializeJson(EnumFlags<Access>{Access::Read | Access::Write}, false), R"(["Read","Write"])");
    EXPECT_EQ(SerializeJson(EnumFlags<Access>{}, false), "[]");
    EXPECT_FALSE(SerializeJson(EnumFlags<Access>{static_cast<Access>(4)}, false).has_value());
}

TEST(JsonSerializerTest, RejectsNonFiniteNumbers) {
    EXPECT_FALSE(SerializeJson(std::numeric_limits<double>::infinity()).has_value());
    EXPECT_FALSE(SerializeJson(std::numeric_limits<float>::quiet_NaN()).has_value());
}

TEST(JsonSerializerTest, ObjectWriterCopiesMemberNames) {
    JsonWriter writer;
    JsonWriteContext context{writer};
    JsonObjectWriter object = context.BeginObject();
    ASSERT_TRUE(object.IsValid());
    string name{"Dynamic"};

    ASSERT_TRUE(object.Member(name, uint32_t{42}));
    name.assign("xxxxxxx");

    EXPECT_EQ(writer.Write(false), R"({"Dynamic":42})");
}

TEST(JsonDeserializerTest, RoundTripsNestedObject) {
    const Parent source{
        .Name = "root",
        .Children = {{.Name = "first", .Value = Mode::Enabled}, {.Name = "second", .Value = Mode::Disabled}},
        .Limit = 5,
        .ExplicitNull = std::nullopt,
        .Count = 9,
    };
    const auto json = SerializeJson(source, false);
    ASSERT_TRUE(json.has_value());

    const auto decoded = DeserializeJson<Parent>(*json);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->Name, "root");
    ASSERT_EQ(decoded->Children.size(), 2u);
    EXPECT_EQ(decoded->Children[1].Name, "second");
    EXPECT_EQ(decoded->Children[0].Value, Mode::Enabled);
    EXPECT_EQ(decoded->Limit, 5u);
    EXPECT_FALSE(decoded->ExplicitNull.has_value());
    EXPECT_EQ(decoded->Count, 9u);
}

TEST(JsonDeserializerTest, ReadsScalarAndContainerRoots) {
    EXPECT_EQ(DeserializeJson<int16_t>("-42"), int16_t{-42});
    EXPECT_EQ(DeserializeJson<double>("1.25"), 1.25);
    EXPECT_EQ(DeserializeJson<string>(R"("value")"), "value");
    EXPECT_EQ(DeserializeJson<vector<bool>>("[true,false]"), (vector<bool>{true, false}));
    EXPECT_EQ((DeserializeJson<array<uint32_t, 3>>("[1,2,3]")), (array<uint32_t, 3>{1, 2, 3}));
    const auto null = DeserializeJson<std::optional<uint32_t>>("null");
    ASSERT_TRUE(null.has_value());
    EXPECT_FALSE(null->has_value());
}

TEST(JsonDeserializerTest, RejectsNumbersOutsideTargetRange) {
    EXPECT_FALSE(DeserializeJson<uint8_t>("256").has_value());
    EXPECT_FALSE(DeserializeJson<int8_t>("-129").has_value());
    EXPECT_FALSE(DeserializeJson<uint32_t>("-1").has_value());
    EXPECT_FALSE(DeserializeJson<float>("1e100").has_value());
    EXPECT_EQ(DeserializeJson<uint8_t>("255"), uint8_t{255});
}

TEST(JsonDeserializerTest, RejectsMismatchedTypesAndShapes) {
    EXPECT_FALSE(DeserializeJson<int32_t>("1.0").has_value());
    EXPECT_FALSE(DeserializeJson<bool>("1").has_value());
    EXPECT_FALSE(DeserializeJson<string>("true").has_value());
    EXPECT_FALSE(DeserializeJson<Mode>("2").has_value());
    EXPECT_FALSE(DeserializeJson<Mode>(R"("enabled")").has_value());
    EXPECT_FALSE(DeserializeJson<vector<uint32_t>>(R"([1,"bad",3])").has_value());
    EXPECT_FALSE((DeserializeJson<array<uint32_t, 2>>("[1]").has_value()));
    EXPECT_FALSE((DeserializeJson<array<uint32_t, 2>>("[1,2,3]").has_value()));
    EXPECT_FALSE(DeserializeJson<Child>(R"({"Name":"missing value"})").has_value());
}

TEST(JsonDeserializerTest, FlagsAcceptOnlySingleBitNames) {
    const auto flags = DeserializeJson<EnumFlags<Access>>(R"(["Read","Write"])");
    ASSERT_TRUE(flags.has_value());
    EXPECT_EQ(*flags, Access::Read | Access::Write);

    EXPECT_FALSE(DeserializeJson<EnumFlags<Access>>(R"(["Execute"])").has_value());
    EXPECT_FALSE(DeserializeJson<EnumFlags<Access>>(R"(["Read|Write"])").has_value());
    EXPECT_FALSE(DeserializeJson<EnumFlags<Access>>(R"("Read")").has_value());
}

TEST(JsonDeserializerTest, FailureLeavesOutputUnchanged) {
    const auto document = JsonDocument::Parse(R"({"Name":"changed","Children":[{"Name":"bad","Value":"Unknown"}],"ExplicitNull":null,"Count":1})");
    ASSERT_TRUE(document.has_value());
    Parent output{.Name = "original", .Children = {{.Name = "kept", .Value = Mode::Enabled}}, .Limit = 7, .ExplicitNull = 8, .Count = 10};

    EXPECT_FALSE(DeserializeJsonValue(document->Root(), output));

    EXPECT_EQ(output.Name, "original");
    ASSERT_EQ(output.Children.size(), 1u);
    EXPECT_EQ(output.Children[0].Name, "kept");
    EXPECT_EQ(output.Limit, 7u);
    EXPECT_EQ(output.Count, 10u);
}

TEST(JsonDeserializerTest, ObjectReaderMemberPresenceRules) {
    const auto document = JsonDocument::Parse(R"({"present":3,"null":null})");
    ASSERT_TRUE(document.has_value());
    const JsonObjectReader object{document->Root()};
    uint32_t required = 0;
    std::optional<uint32_t> optionalMissing{1};
    std::optional<uint32_t> optionalNull{1};
    uint32_t defaulted = 5;

    EXPECT_FALSE(object.Member("missing", required));
    EXPECT_TRUE(object.OptionalMember("missing", optionalMissing));
    EXPECT_TRUE(object.OptionalMember("null", optionalNull));
    EXPECT_TRUE(object.MemberIfPresent("missing", defaulted));

    EXPECT_FALSE(optionalMissing.has_value());
    EXPECT_FALSE(optionalNull.has_value());
    EXPECT_EQ(defaulted, 5u);
    EXPECT_TRUE(object.MemberIfPresent("present", defaulted));
    EXPECT_EQ(defaulted, 3u);
}

}  // namespace
}  // namespace radray
