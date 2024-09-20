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

std::optional<radray::wstring> ToWideChar(const radray::string& str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    const char* mbstr = str.data();
    std::mbstate_t state{};
    size_t len{0};
    mbsrtowcs_s(&len, nullptr, 0, &mbstr, 0, &state);
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
    size_t len = std::mbsrtowcs(nullptr, &start, 0, &state) + 1;
    radray::vector<wchar_t> wstr(len);
    state = mbstate_t{};
    size_t result = std::mbsrtowcs(&wstr[0], &start, wstr.size(), &state);
    if (result == (size_t)-1) {
        return std::nullopt;
    } else {
        return radray::wstring{wstr.begin(), wstr.end()};
    }
#endif
}

}  // namespace radray
