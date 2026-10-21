#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

#include <radray/json.h>

namespace radray {
namespace json_deserializer_test {

enum class Mode : int16_t {
    Disabled = -1,
    Enabled = 2,
};

enum class NamedFlag : uint8_t {
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
    uint32_t Dynamic{0};
};

struct Unsupported {};

}  // namespace json_deserializer_test

template <>
struct is_flags<json_deserializer_test::NamedFlag> : std::true_type {};

template <>
struct JsonSerializer<json_deserializer_test::Child> {
    static bool Write(
        JsonWriteContext& context,
        const json_deserializer_test::Child& value) noexcept {
        using value_type = json_deserializer_test::Child;
        return SerializeJsonObject(
            context,
            value,
            JsonMember{"Name", &value_type::Name},
            JsonMember{"Value", &value_type::Value});
    }
};

template <>
struct JsonDeserializer<json_deserializer_test::Child> {
    static bool Read(
        const JsonValue& json,
        json_deserializer_test::Child& value) noexcept {
        using value_type = json_deserializer_test::Child;
        return DeserializeJsonObject(
            json,
            value,
            JsonMember{"Name", &value_type::Name},
            JsonMember{"Value", &value_type::Value});
    }
};

template <>
struct JsonSerializer<json_deserializer_test::Parent> {
    static bool Write(
        JsonWriteContext& context,
        const json_deserializer_test::Parent& value) noexcept {
        JsonObjectWriter object = context.BeginObject();
        return object.IsValid() &&
               object.Member("Name", value.Name) &&
               object.Member("Children", value.Children) &&
               object.OptionalMember("Limit", value.Limit) &&
               object.Member("ExplicitNull", value.ExplicitNull) &&
               object.Member("Dynamic", value.Dynamic);
    }
};

template <>
struct JsonDeserializer<json_deserializer_test::Parent> {
    static bool Read(
        const JsonValue& json,
        json_deserializer_test::Parent& value) noexcept {
        JsonObjectReader object{json};
        json_deserializer_test::Parent decoded{};
        if (!object.IsValid() ||
            !object.Member("Name", decoded.Name) ||
            !object.Member("Children", decoded.Children) ||
            !object.OptionalMember("Limit", decoded.Limit) ||
            !object.Member("ExplicitNull", decoded.ExplicitNull) ||
            !object.Member("Dynamic", decoded.Dynamic)) {
            return false;
        }
        value = std::move(decoded);
        return true;
    }
};

static_assert(json_deserializable<json_deserializer_test::Parent>);
static_assert(json_deserializable<vector<json_deserializer_test::Parent>>);
static_assert(json_deserializable<std::array<uint32_t, 2>>);
static_assert(is_json_deserializable_v<std::optional<uint32_t>>);
static_assert(json_deserializable<EnumFlags<json_deserializer_test::NamedFlag>>);
static_assert(!json_deserializable<std::string_view>);
static_assert(!json_deserializable<json_deserializer_test::Unsupported>);

namespace {

TEST(JsonDeserializerTest, RecursivelyRoundTripsMembersAndContainers) {
    json_deserializer_test::Parent source{
        .Name = "root",
        .Children = {
            {.Name = "first", .Value = json_deserializer_test::Mode::Enabled},
            {.Name = "second", .Value = json_deserializer_test::Mode::Disabled},
        },
        .Limit = std::nullopt,
        .ExplicitNull = std::nullopt,
        .Dynamic = 9,
    };

    std::optional<string> json = SerializeJson(source, false);
    ASSERT_TRUE(json.has_value());

    std::optional<json_deserializer_test::Parent> decoded =
        DeserializeJson<json_deserializer_test::Parent>(json.value());
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->Name, source.Name);
    ASSERT_EQ(decoded->Children.size(), 2u);
    EXPECT_EQ(decoded->Children[0].Name, "first");
    EXPECT_EQ(decoded->Children[0].Value, json_deserializer_test::Mode::Enabled);
    EXPECT_EQ(decoded->Children[1].Value, json_deserializer_test::Mode::Disabled);
    EXPECT_FALSE(decoded->Limit.has_value());
    EXPECT_FALSE(decoded->ExplicitNull.has_value());
    EXPECT_EQ(decoded->Dynamic, 9u);
}

TEST(JsonDeserializerTest, SupportsPrimitiveAndContainerRootValues) {
    std::optional<int16_t> integer = DeserializeJson<int16_t>("-42");
    ASSERT_TRUE(integer.has_value());
    EXPECT_EQ(integer.value(), -42);

    std::optional<double> real = DeserializeJson<double>("1.25");
    ASSERT_TRUE(real.has_value());
    EXPECT_DOUBLE_EQ(real.value(), 1.25);

    std::optional<string> text = DeserializeJson<string>(R"("value")");
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(text.value(), "value");

    std::optional<vector<bool>> bits = DeserializeJson<vector<bool>>("[true,false,true]");
    ASSERT_TRUE(bits.has_value());
    ASSERT_EQ(bits->size(), 3u);
    EXPECT_TRUE((*bits)[0]);
    EXPECT_FALSE((*bits)[1]);
    EXPECT_TRUE((*bits)[2]);

    std::optional<std::array<uint32_t, 3>> array =
        DeserializeJson<std::array<uint32_t, 3>>("[1,2,3]");
    ASSERT_TRUE(array.has_value());
    EXPECT_EQ(array.value(), (std::array<uint32_t, 3>{1, 2, 3}));

    std::optional<std::optional<uint32_t>> null =
        DeserializeJson<std::optional<uint32_t>>("null");
    ASSERT_TRUE(null.has_value());
    EXPECT_FALSE(null->has_value());
}

TEST(JsonDeserializerTest, EnumsAndFlagsUseMemberNames) {
    using flag_type = json_deserializer_test::NamedFlag;

    const std::optional<flag_type> flag = DeserializeJson<flag_type>(R"("Write")");
    ASSERT_TRUE(flag.has_value());
    EXPECT_EQ(flag.value(), flag_type::Write);
    EXPECT_FALSE(DeserializeJson<flag_type>("2").has_value());
    EXPECT_FALSE(DeserializeJson<flag_type>(R"("unknown")").has_value());

    const std::optional<EnumFlags<flag_type>> flags =
        DeserializeJson<EnumFlags<flag_type>>(R"(["Read","Write"])");
    ASSERT_TRUE(flags.has_value());
    EXPECT_TRUE(flags->HasFlag(flag_type::Read));
    EXPECT_TRUE(flags->HasFlag(flag_type::Write));

    const std::optional<string> json = SerializeJson(flags.value(), false);
    ASSERT_TRUE(json.has_value());
    EXPECT_EQ(json.value(), R"(["Read","Write"])");
    EXPECT_FALSE(
        SerializeJson(EnumFlags<flag_type>{static_cast<flag_type>(4)}, false).has_value());
    EXPECT_FALSE(DeserializeJson<EnumFlags<flag_type>>(R"(["execute"])").has_value());
    EXPECT_FALSE(DeserializeJson<EnumFlags<flag_type>>(R"(["Read|Write"])").has_value());
    EXPECT_FALSE(DeserializeJson<EnumFlags<flag_type>>(R"(["Read|Read"])").has_value());
}

TEST(JsonDeserializerTest, RejectsTypeShapeAndNumericRangeErrors) {
    EXPECT_FALSE(DeserializeJson<uint8_t>("256").has_value());
    EXPECT_FALSE(DeserializeJson<uint32_t>("-1").has_value());
    EXPECT_FALSE(DeserializeJson<int32_t>("1.0").has_value());
    EXPECT_FALSE(DeserializeJson<float>("1e100").has_value());
    EXPECT_FALSE(DeserializeJson<bool>("1").has_value());
    EXPECT_FALSE(DeserializeJson<string>("true").has_value());
    EXPECT_FALSE(DeserializeJson<json_deserializer_test::Mode>("32768").has_value());
    EXPECT_FALSE(DeserializeJson<vector<uint32_t>>(R"([1,"bad",3])").has_value());
    EXPECT_FALSE((DeserializeJson<std::array<uint32_t, 2>>("[1]").has_value()));
    EXPECT_FALSE(DeserializeJson<json_deserializer_test::Child>(R"({"Name":"missing"})").has_value());
    EXPECT_FALSE(DeserializeJson<uint32_t>("{").has_value());
}

TEST(JsonDeserializerTest, LeavesOutputUnchangedOnFailure) {
    constexpr std::string_view invalid = R"({
        "Name": "changed",
        "Children": [{"Name": "bad", "Value": 32768}],
        "ExplicitNull": null,
        "Dynamic": 9
    })";
    std::optional<JsonDocument> document = JsonDocument::Parse(invalid);
    ASSERT_TRUE(document.has_value());

    json_deserializer_test::Parent output{
        .Name = "original",
        .Children = {{.Name = "kept", .Value = json_deserializer_test::Mode::Enabled}},
        .Limit = 7,
        .ExplicitNull = 8,
        .Dynamic = 10,
    };
    EXPECT_FALSE(DeserializeJsonValue(document->Root(), output));
    EXPECT_EQ(output.Name, "original");
    ASSERT_EQ(output.Children.size(), 1u);
    EXPECT_EQ(output.Children[0].Name, "kept");
    EXPECT_EQ(output.Limit, 7u);
    EXPECT_EQ(output.ExplicitNull, 8u);
    EXPECT_EQ(output.Dynamic, 10u);
}

}  // namespace
}  // namespace radray
