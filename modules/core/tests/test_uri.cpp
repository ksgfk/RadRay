#include <gtest/gtest.h>
#include <radray/uri.h>
#include <radray/json.h>
#include <fmt/format.h>

namespace radray {
TEST(Uri, CanonicalComponentsAndRelativeResolution) {
    const auto base = Uri::TryCreate("VIRTUAL://User@SHADERS:80/x/%2e%2e/material/%2f?q#p");
    ASSERT_TRUE(base);
    EXPECT_EQ(base->AsString(), "virtual://User@shaders:80/material/%2F?q#p");
    EXPECT_EQ(base->Authority(), "User@shaders:80");
    EXPECT_EQ(base->Host(), "shaders");
    EXPECT_EQ(base->Port(), 80);
    const auto next = Uri::TryCreate(*base, "../other.hlsl");
    ASSERT_TRUE(next);
    EXPECT_EQ(next->AsString(), "virtual://User@shaders:80/other.hlsl");
    EXPECT_FALSE(Uri::TryCreate("virtual://shaders/a%2"));
    EXPECT_FALSE(Uri::TryCreate("virtual://shaders:65536/x"));
}

TEST(Uri, SerializationHashAndFormatUseCanonicalText) {
    const auto uri = Uri::TryCreate("VIRTUAL://Host/%7e");
    ASSERT_TRUE(uri);
    EXPECT_EQ(fmt::format("{}", *uri), "virtual://host/~");
    EXPECT_EQ(std::hash<Uri>{}(*uri), std::hash<std::string_view>{}(uri->AsString()));
    const auto json = SerializeJson(*uri, false);
    ASSERT_TRUE(json);
    EXPECT_EQ(DeserializeJson<Uri>(*json), uri);
    EXPECT_FALSE(DeserializeJson<Uri>(R"("virtual://host/%xx")"));
}

TEST(Uri, FilePathRoundTrip) {
    auto path = std::filesystem::temp_directory_path() / "a b#%.hlsl";
    const auto uri = Uri::FromFilePath(path);
    ASSERT_TRUE(uri);
    EXPECT_TRUE(uri->AsString().ends_with("/a%20b%23%25.hlsl"));
    EXPECT_EQ(uri->ToFilePath(), path);
    EXPECT_FALSE(Uri::FromFilePath("relative.hlsl"));
#if defined(_WIN32)
    const std::filesystem::path unc{LR"(\\server\share\a b)"};
    const auto network = Uri::FromFilePath(unc);
    ASSERT_TRUE(network);
    EXPECT_EQ(network->AsString(), "file://server/share/a%20b");
    EXPECT_EQ(network->ToFilePath(), unc);
    const auto invalid = Uri::TryCreate("file:///C:/bad%FF");
    ASSERT_TRUE(invalid);
    EXPECT_FALSE(invalid->ToFilePath());
#endif
}
}  // namespace radray
