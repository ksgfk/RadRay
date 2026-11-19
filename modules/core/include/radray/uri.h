#pragma once

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

    bool IsAbsoluteUri() const noexcept { return !Scheme().empty(); }
    bool IsFile() const noexcept { return Scheme() == "file"; }
    std::string_view Scheme() const noexcept { return View(_scheme); }
    std::string_view Authority() const noexcept { return View(_authority); }
    std::string_view UserInfo() const noexcept { return View(_userInfo); }
    std::string_view Host() const noexcept { return View(_host); }
    std::optional<uint16_t> Port() const noexcept { return _text.empty() ? std::nullopt : _port; }
    std::string_view AbsolutePath() const noexcept { return View(_path); }
    std::string_view Query() const noexcept { return View(_query); }
    std::string_view Fragment() const noexcept { return View(_fragment); }
    std::string_view AsString() const noexcept { return _text; }
    const string& ToString() const noexcept { return _text; }
    bool operator==(const Uri& other) const noexcept { return _text == other._text; }
    auto operator<=>(const Uri& other) const noexcept { return _text <=> other._text; }

private:
    struct Range {
        size_t Offset{}, Length{};
    };
    std::string_view View(Range range) const noexcept {
        return _text.empty() ? std::string_view{} : AsString().substr(range.Offset, range.Length);
    }
    string _text;
    Range _scheme, _authority, _userInfo, _host, _path, _query, _fragment;
    std::optional<uint16_t> _port;
};

inline std::string_view format_as(const Uri& value) noexcept { return value.AsString(); }

}  // namespace radray

template <>
struct std::hash<radray::Uri> {
    size_t operator()(const radray::Uri& uri) const noexcept { return std::hash<std::string_view>{}(uri.AsString()); }
};
