#include <radray/utility.h>

#include <fstream>
#include <cwchar>

#include <radray/logger.h>

namespace radray {

std::optional<radray::string> ReadText(const std::filesystem::path& filepath) noexcept {
    if (!std::filesystem::exists(filepath)) {
        return std::nullopt;
    }
    std::ifstream t{filepath};
    radray::string str(
        std::istreambuf_iterator<char>{t},
        std::istreambuf_iterator<char>{});
    return str;
}

std::optional<radray::wstring> ToWideChar(std::string_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    const char* mbstr = str.data();
    std::mbstate_t state{};
    size_t len{0};
    mbsrtowcs_s(&len, nullptr, 0, &mbstr, str.size(), &state);
    if (len != str.size() + 1) {  // because string_view dose not contains eof '\0'
        return std::nullopt;
    }
    radray::vector<wchar_t> wstr(len);
    state = mbstate_t{};
    auto err = mbsrtowcs_s(nullptr, wstr.data(), wstr.size(), &mbstr, len, &state);
    if (err == 0) {
        return radray::wstring{wstr.begin(), wstr.end()};
    } else {
        return std::nullopt;
    }
#else
    const char* start = str.data();
    std::mbstate_t state{};
    size_t len = std::mbsrtowcs(nullptr, &start, str.size(), &state);
    if (len != str.size()) {
        return std::nullopt;
    }
    radray::vector<wchar_t> wstr(len + 1);
    state = mbstate_t{};
    size_t result = std::mbsrtowcs(&wstr[0], &start, wstr.size(), &state);
    if (result == (size_t)-1) {
        return std::nullopt;
    } else {
        return radray::wstring{wstr.begin(), wstr.end()};
    }
#endif
}

std::optional<radray::string> ToMultiByte(std::wstring_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    std::mbstate_t state{};
    size_t len{0};
    wcstombs_s(&len, nullptr, 0, str.data(), str.size());
    if (len != str.size() + 1) {  // because string_view dose not contains eof '\0'
        return std::nullopt;
    }
    radray::vector<char> wstr(len);
    state = mbstate_t{};
    auto err = wcstombs_s(nullptr, wstr.data(), wstr.size(), str.data(), len);
    if (err == 0) {
        return radray::string{wstr.begin(), wstr.end()};
    } else {
        return std::nullopt;
    }
#else
#error "todo"
#endif
}

}  // namespace radray
