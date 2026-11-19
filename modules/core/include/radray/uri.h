#pragma once

#include <compare>
#include <filesystem>
#include <functional>
#include <optional>
#include <string_view>

#include <radray/types.h>

namespace radray {

enum class UriKind : uint8_t {
    RelativeOrAbsolute,
    Absolute,
    Relative
};

class Uri {
public:
    static std::optional<Uri> TryCreate(std::string_view text, UriKind kind = UriKind::Absolute);
    static std::optional<Uri> TryCreate(const Uri& base, std::string_view reference);
    static std::optional<Uri> TryCreate(const Uri& base, const Uri& reference);
    static bool TryParse(std::string_view text, UriKind kind, Uri& result);
    static std::optional<Uri> FromFilePath(const std::filesystem::path& path);
    std::optional<std::filesystem::path> ToFilePath() const;

    bool IsAbsoluteUri() const noexcept;
    bool IsFile() const noexcept;
    std::string_view Scheme() const noexcept;
    std::string_view Authority() const noexcept;
    std::string_view UserInfo() const noexcept;
    std::string_view Host() const noexcept;
    std::optional<uint16_t> Port() const noexcept;
    std::string_view AbsolutePath() const noexcept;
    std::string_view Query() const noexcept;
    std::string_view Fragment() const noexcept;
    std::string_view AsString() const noexcept;
    const string& ToString() const noexcept;
    bool operator==(const Uri& other) const noexcept;
    std::strong_ordering operator<=>(const Uri& other) const noexcept;

private:
    struct Range {
        size_t Offset{}, Length{};
    };
    std::string_view View(Range range) const noexcept;
    string _text;
    Range _scheme, _authority, _userInfo, _host, _path, _query, _fragment;
    std::optional<uint16_t> _port;
};

std::string_view format_as(const Uri& value) noexcept;

}  // namespace radray

template <>
struct std::hash<radray::Uri> {
    size_t operator()(const radray::Uri& uri) const noexcept;
};
