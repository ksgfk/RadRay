#include <radray/utility.h>

#include <fstream>
#include <limits>

#include <radray/platform.h>
#include <radray/logger.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#include <windows.h>
#endif

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

#ifdef RADRAY_PLATFORM_WINDOWS
static void LogWinCharCvtErr() {
    DWORD err = GetLastError();
    std::string_view strErr = ([err]() {
        switch (err) {
            case ERROR_INSUFFICIENT_BUFFER: return "insufficient buffer";
            case ERROR_INVALID_FLAGS: return "invalid flags";
            case ERROR_INVALID_PARAMETER: return "invalid parameter";
            case ERROR_NO_UNICODE_TRANSLATION: return "no unicode translation";
            default: return "unknown";
        }
    })();
    RADRAY_ERR_LOG("cannot convert char to wchar, reason={} (code={})", strErr, err);
}
#endif

std::optional<radray::wstring> ToWideChar(std::string_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (str.size() >= std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    int test = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (test == 0) {
        LogWinCharCvtErr();
        return std::nullopt;
    }
    radray::wstring to(test, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), str.size(), to.data(), (int)to.size());
    return to;
#else
    // can process utf8 on other platform? need test
    const char* start = str.data();
    std::mbstate_t state{};
    size_t len = std::mbsrtowcs(nullptr, &start, str.size(), &state);
    if (len == static_cast<size_t>(-1)) {
        return std::nullopt;
    }
    radray::wstring wstr(len, L'\0');
    state = mbstate_t{};
    size_t result = std::mbsrtowcs(&wstr[0], &start, str.size(), &state);
    RADRAY_ASSERT(result == wstr.size());
    return result == static_cast<size_t>(-1) ? std::nullopt : std::make_optional(wstr);
#endif
}

std::optional<radray::string> ToMultiByte(std::wstring_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (str.size() >= std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    int test = WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0, nullptr, nullptr);
    if (test == 0) {
        LogWinCharCvtErr();
        return std::nullopt;
    }
    radray::string to(test, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.data(), str.size(), to.data(), (int)to.size(), nullptr, nullptr);
    return to;
#else
    // can process utf8 on other platform? need test
    size_t len = std::wcstombs(nullptr, str.data(), str.size());
    if (len == static_cast<size_t>(-1)) {
        return std::nullopt;
    }
    radray::string wstr(len, L'\0');
    size_t result = std::wcstombs(wstr.data(), str.data(), str.size());
    RADRAY_ASSERT(result == str.size());
    return result == static_cast<size_t>(-1) ? std::nullopt : std::make_optional(wstr);
#endif
}

}  // namespace radray
