#include <radray/utility.h>

#include <fstream>
#include <stdexcept>
#include <cwchar>

#include <radray/logger.h>

namespace radray {

radray::string ReadText(const std::filesystem::path& filepath) {
    if (!std::filesystem::exists(filepath)) {
        RADRAY_THROW(std::runtime_error, "cannot read text {}", filepath.generic_string());
    }
    std::ifstream t{filepath};
    radray::string str(
        std::istreambuf_iterator<char>{t},
        std::istreambuf_iterator<char>{});
    return str;
}

radray::wstring ToWideChar(const radray::string& str) {
#ifdef RADRAY_PLATFORM_WINDOWS
    const char* mbstr = str.data();
    std::mbstate_t state{};
    size_t len{0};
    mbsrtowcs_s(&len, nullptr, 0, &mbstr, 0, &state);
    radray::vector<wchar_t> wstr(len);
    state = mbstate_t{};
    mbsrtowcs_s(nullptr, wstr.data(), wstr.size(), &mbstr, len, &state);
    return radray::wstring{wstr.begin(), wstr.end()};
#else
    const char* start = str.data();
    std::mbstate_t state{};
    size_t len = std::mbsrtowcs(nullptr, &start, 0, &state) + 1;
    radray::vector<wchar_t> wstr(len);
    state = mbstate_t{};
    std::mbsrtowcs(&wstr[0], &start, wstr.size(), &state);
    return radray::wstring{wstr.begin(), wstr.end()};
#endif
}

}  // namespace radray
