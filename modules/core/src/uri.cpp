#include <radray/uri.h>

#include <algorithm>
#include <charconv>
#include <radray/text_encoding.h>
#include <radray/logger.h>

namespace radray {
namespace {
bool Alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool Digit(char c) { return c >= '0' && c <= '9'; }
char Lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }
bool Unreserved(char c) {
    return Alpha(c) || Digit(c) || std::string_view{"-._~"}.find(c) != std::string_view::npos;
}
int Hex(char c) {
    if (Digit(c)) return c - '0';
    c = Lower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
void Escape(string& out, unsigned char c) {
    constexpr std::string_view digits{"0123456789ABCDEF"};
    out += '%';
    out += digits[c >> 4];
    out += digits[c & 15];
}
bool Normalize(string& out, std::string_view part, std::string_view allowed, bool lower = false) {
    for (size_t i = 0; i < part.size(); ++i) {
        char c = part[i];
        if (c == '%') {
            if (part.size() - i < 3 || Hex(part[i + 1]) < 0 || Hex(part[i + 2]) < 0) return false;
            c = static_cast<char>(16 * Hex(part[i + 1]) + Hex(part[i + 2]));
            i += 2;
            if (!Unreserved(c)) {
                Escape(out, static_cast<unsigned char>(c));
                continue;
            }
        } else if (!Unreserved(c) && allowed.find(c) == std::string_view::npos) {
            return false;
        }
        out += lower ? Lower(c) : c;
    }
    return true;
}

string RemoveDotSegments(std::string_view path) {
    const bool rooted = path.starts_with('/');
    if (rooted) path.remove_prefix(1);
    vector<std::string_view> parts;
    for (;;) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (part != ".") {
            parts.push_back(part);
        }
        if (slash == path.npos) {
            if (part == "." || part == "..") parts.emplace_back();
            break;
        }
        path.remove_prefix(slash + 1);
    }
    string result = rooted ? "/" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) result += '/';
        result += parts[i];
    }
    return result;
}
}  // namespace

std::optional<Uri> Uri::TryCreate(std::string_view text, UriKind kind) {
    Uri uri;
    const auto take = [&text](std::string_view delimiters) {
        const auto end = text.find_first_of(delimiters);
        const auto prefix = text.substr(0, end);
        text.remove_prefix(prefix.size());
        return prefix;
    };
    const auto delimiter = text.find_first_of(":/\\?#");
    if (delimiter != text.npos && text[delimiter] == ':') {
        auto scheme = take(":");
        if (scheme.empty() || !Alpha(scheme.front()) || kind == UriKind::Relative) return std::nullopt;
        for (char c : scheme) {
            if (!Alpha(c) && !Digit(c) && c != '+' && c != '-' && c != '.') return std::nullopt;
            uri._text += Lower(c);
        }
        uri._scheme = {0, uri._text.size()};
        uri._text += ':';
        text.remove_prefix(1);
    } else if (kind == UriKind::Absolute)
        return std::nullopt;

    constexpr std::string_view subDelims = "!$&'()*+,;=";
    if (text.starts_with("//")) {
        text.remove_prefix(2);
        uri._text += "//";
        auto authority = take("/\\?#");
        const auto start = uri._text.size();
        const auto at = authority.find('@');
        if (at != authority.npos) {
            if (!Normalize(uri._text, authority.substr(0, at), "!$&'()*+,;=:")) return std::nullopt;
            uri._userInfo = {start, uri._text.size() - start};
            uri._text += '@';
            authority.remove_prefix(at + 1);
        }
        size_t hostEnd = authority.find(':');
        bool bracketed = authority.starts_with('[');
        if (bracketed) {
            hostEnd = authority.find(']');
            if (hostEnd == authority.npos || hostEnd < 2) return std::nullopt;
            ++hostEnd;
        }
        const auto host = authority.substr(0, hostEnd);
        if (host.empty() && (!uri.IsFile() || at != authority.npos || hostEnd != authority.npos)) return std::nullopt;
        const auto hostStart = uri._text.size();
        if (!Normalize(uri._text, host, bracketed ? std::string_view{"[]:"} : subDelims, true)) return std::nullopt;
        uri._host = {hostStart, uri._text.size() - hostStart};
        authority.remove_prefix(host.size());
        if (!authority.empty()) {
            if (authority.front() != ':') return std::nullopt;
            authority.remove_prefix(1);
            uint16_t port{};
            const auto [end, ec] = std::from_chars(authority.data(), authority.data() + authority.size(), port);
            if (ec != std::errc{} || end != authority.data() + authority.size()) return std::nullopt;
            uri._port = port;
            uri._text += ':';
            uri._text += radray::format("{}", port);
        }
        uri._authority = {start, uri._text.size() - start};
    }
    string rawPath{take("?#")};
    std::replace(rawPath.begin(), rawPath.end(), '\\', '/');
    string path;
    if (!Normalize(path, rawPath, "/:@!$&'()*+,;=")) return std::nullopt;
    if (uri.IsAbsoluteUri()) path = RemoveDotSegments(path);
    uri._path = {uri._text.size(), path.size()};
    uri._text += path;
    const auto appendSuffix = [&](Range& range, std::string_view part) {
        const auto start = uri._text.size();
        uri._text += part.front();
        if (!Normalize(uri._text, part.substr(1), "?/:@!$&'()*+,;=")) return false;
        range = {start, uri._text.size() - start};
        return true;
    };
    if (text.starts_with('?') && !appendSuffix(uri._query, take("#"))) return std::nullopt;
    if (!text.empty() && !appendSuffix(uri._fragment, text)) return std::nullopt;
    return uri;
}

bool Uri::TryParse(std::string_view text, UriKind kind, Uri& result) {
    auto uri = TryCreate(text, kind);
    if (!uri) return false;
    result = std::move(*uri);
    return true;
}

std::optional<Uri> Uri::TryCreate(const Uri& base, std::string_view reference) {
    const auto parsed = TryCreate(reference, UriKind::RelativeOrAbsolute);
    return parsed ? TryCreate(base, *parsed) : std::nullopt;
}

std::optional<Uri> Uri::TryCreate(const Uri& base, const Uri& reference) {
    if (!base.IsAbsoluteUri()) return std::nullopt;
    if (reference.IsAbsoluteUri()) return reference;
    if (reference.AsString().starts_with("//")) {
        string result{base.Scheme()};
        result += ':';
        result += reference.AsString();
        return TryCreate(result);
    }
    string result{base.AsString().substr(0, base._path.Offset)};
    const auto path = reference.AbsolutePath();
    if (path.empty()) {
        result += base.AbsolutePath();
        if (reference.Query().empty()) result += base.Query();
    } else {
        if (!path.starts_with('/')) {
            const auto slash = base.AbsolutePath().rfind('/');
            if (slash != path.npos)
                result += base.AbsolutePath().substr(0, slash + 1);
            else if (base._authority.Offset)
                result += '/';
        }
        result += path;
    }
    result += reference.Query();
    result += reference.Fragment();
    return TryCreate(result);
}

std::optional<Uri> Uri::FromFilePath(const std::filesystem::path& path) {
    if (!path.is_absolute()) return std::nullopt;
#if defined(_WIN32)
    const auto wide = path.generic_wstring();
    const auto bytes = ToMultiByte(wide);
    if (!bytes || ToWideChar(*bytes) != std::wstring_view{wide}) return std::nullopt;
    std::string_view value{*bytes};
#else
    const auto bytes = path.generic_string();
    std::string_view value{bytes};
#endif
    string result{"file://"};
#if defined(_WIN32)
    if (value.starts_with("//"))
        value.remove_prefix(2);
    else
        result += '/';
#endif
    for (char c : value) {
        if (Unreserved(c) || c == '/' || c == ':')
            result += c;
        else
            Escape(result, static_cast<unsigned char>(c));
    }
    return TryCreate(result);
}

std::optional<std::filesystem::path> Uri::ToFilePath() const {
    if (!IsFile() || _userInfo.Offset || Port() || !Query().empty() || !Fragment().empty() ||
        !AbsolutePath().starts_with('/')) return std::nullopt;
    string path;
    auto encoded = AbsolutePath();
#if defined(_WIN32)
    if (!Host().empty()) {
        path = "//";
        encoded = AsString().substr(_host.Offset);
    } else {
        if (encoded.size() < 4 || !Alpha(encoded[1]) || !encoded.substr(2).starts_with(":/")) return std::nullopt;
        encoded.remove_prefix(1);
    }
#else
    if (!Host().empty()) return std::nullopt;
#endif
    for (size_t i = 0; i < encoded.size(); ++i) {
        char c = encoded[i];
        if (c == '%') {
            c = static_cast<char>(16 * Hex(encoded[i + 1]) + Hex(encoded[i + 2]));
            if (c == '/' || c == '\0') return std::nullopt;
#if defined(_WIN32)
            if (c == '\\') return std::nullopt;
#endif
            i += 2;
        }
        path += c;
    }
#if defined(_WIN32)
    const auto wide = ToWideChar(path);
    if (!wide || ToMultiByte(*wide) != path || path.starts_with("//./") || path.starts_with("//?/")) return std::nullopt;
    std::filesystem::path native{*wide};
#else
    std::filesystem::path native{path};
#endif
    native.make_preferred();
    return native;
}

bool Uri::IsAbsoluteUri() const noexcept {
    return !Scheme().empty();
}

bool Uri::IsFile() const noexcept {
    return Scheme() == "file";
}

std::string_view Uri::Scheme() const noexcept {
    return View(_scheme);
}

std::string_view Uri::Authority() const noexcept {
    return View(_authority);
}

std::string_view Uri::UserInfo() const noexcept {
    return View(_userInfo);
}

std::string_view Uri::Host() const noexcept {
    return View(_host);
}

std::optional<uint16_t> Uri::Port() const noexcept {
    return _text.empty() ? std::nullopt : _port;
}

std::string_view Uri::AbsolutePath() const noexcept {
    return View(_path);
}

std::string_view Uri::Query() const noexcept {
    return View(_query);
}

std::string_view Uri::Fragment() const noexcept {
    return View(_fragment);
}

std::string_view Uri::AsString() const noexcept {
    return _text;
}

const string& Uri::ToString() const noexcept {
    return _text;
}

bool Uri::operator==(const Uri& other) const noexcept {
    return _text == other._text;
}

std::strong_ordering Uri::operator<=>(const Uri& other) const noexcept {
    return _text <=> other._text;
}

std::string_view Uri::View(Range range) const noexcept {
    return _text.empty() ? std::string_view{} : AsString().substr(range.Offset, range.Length);
}

std::string_view format_as(const Uri& value) noexcept {
    return value.AsString();
}

}  // namespace radray

size_t std::hash<radray::Uri>::operator()(const radray::Uri& uri) const noexcept {
    return std::hash<std::string_view>{}(uri.AsString());
}
