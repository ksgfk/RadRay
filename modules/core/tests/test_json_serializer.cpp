#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

#include <radray/json.h>

namespace radray {
namespace json_serializer_test {

enum class Mode : int16_t {
    Disabled = -1,
    Enabled = 2,
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

}  // namespace json_serializer_test

template <>
struct JsonSerializer<json_serializer_test::Child> {
    static bool Write(
        JsonWriteContext& context,
        const json_serializer_test::Child& value) noexcept {
        using value_type = json_serializer_test::Child;
        return SerializeJsonObject(
            context,
            value,
            JsonMember{"Name", &value_type::Name},
            JsonMember{"Value", &value_type::Value});
    }
};

template <>
struct JsonSerializer<json_serializer_test::Parent> {
    static bool Write(
        JsonWriteContext& context,
        const json_serializer_test::Parent& value) noexcept {
        constexpr std::string_view dynamicKey{"Dynamic.trailing", 7};

        JsonObjectWriter object = context.BeginObject();
        return object.IsValid() &&
               object.Member("Name", value.Name) &&
               object.Member("Children", value.Children) &&
               object.OptionalMember("Limit", value.Limit) &&
               object.Member("ExplicitNull", value.ExplicitNull) &&
               object.DynamicMember(dynamicKey, value.Dynamic);
    }
};

static_assert(json_serializable<json_serializer_test::Parent>);
static_assert(json_serializable<vector<json_serializer_test::Parent>>);
static_assert(json_serializable<std::array<uint32_t, 2>>);
static_assert(json_serializable<std::span<const uint32_t>>);
static_assert(is_json_serializable_v<std::optional<uint32_t>>);
static_assert(!json_serializable<json_serializer_test::Unsupported>);

namespace {

TEST(JsonSerializerTest, RecursivelySerializesMembersAndContainers) {
    json_serializer_test::Parent value{
        .Name = "root",
        .Children = {
            {.Name = "first", .Value = json_serializer_test::Mode::Enabled},
            {.Name = "second", .Value = json_serializer_test::Mode::Disabled},
        },
        .Limit = std::nullopt,
        .ExplicitNull = std::nullopt,
        .Dynamic = 9,
    };

    std::optional<string> text = SerializeJson(value, false);
    ASSERT_TRUE(text.has_value());

    std::optional<JsonDocument> document = JsonDocument::Parse(text.value());
    ASSERT_TRUE(document.has_value());
    JsonValue root = document->Root();

    EXPECT_EQ(root["Name"].AsString(), "root");
    ASSERT_EQ(root["Children"].Size(), 2u);
    EXPECT_EQ(root["Children"].At(0)["Name"].AsString(), "first");
    EXPECT_EQ(root["Children"].At(0)["Value"].AsInt(), 2);
    EXPECT_EQ(root["Children"].At(1)["Value"].AsInt(), -1);
    EXPECT_FALSE(root.Has("Limit"));
    EXPECT_TRUE(root["ExplicitNull"].IsNull());
    EXPECT_EQ(root["Dynamic"].AsUint(), 9u);
    EXPECT_FALSE(root.Has("Dynamic.trailing"));
}

TEST(JsonSerializerTest, SupportsAnyJsonRootValue) {
    std::optional<string> integer = SerializeJson(uint32_t{42}, false);
    ASSERT_TRUE(integer.has_value());
    EXPECT_EQ(integer.value(), "42");

    std::optional<string> null = SerializeJson(std::optional<uint32_t>{}, false);
    ASSERT_TRUE(null.has_value());
    EXPECT_EQ(null.value(), "null");

    constexpr std::string_view sliced{"value.trailing", 5};
    std::optional<string> stringValue = SerializeJson(sliced, false);
    ASSERT_TRUE(stringValue.has_value());
    EXPECT_EQ(stringValue.value(), "\"value\"");

    const std::array values{uint32_t{1}, uint32_t{2}, uint32_t{3}};
    std::optional<string> array = SerializeJson(values, false);
    ASSERT_TRUE(array.has_value());
    EXPECT_EQ(array.value(), "[1,2,3]");

    std::optional<string> span = SerializeJson(std::span{values}, false);
    ASSERT_TRUE(span.has_value());
    EXPECT_EQ(span.value(), "[1,2,3]");

    const vector<bool> bits{true, false, true};
    std::optional<string> boolVector = SerializeJson(bits, false);
    ASSERT_TRUE(boolVector.has_value());
    EXPECT_EQ(boolVector.value(), "[true,false,true]");
}

TEST(JsonSerializerTest, RejectsNonFiniteNumbers) {
    EXPECT_FALSE(SerializeJson(std::numeric_limits<double>::infinity()).has_value());
    EXPECT_FALSE(SerializeJson(std::numeric_limits<double>::quiet_NaN()).has_value());
}

}  // namespace
}  // namespace radray
