#include <radray/text_encoding.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#endif

#include <radray/logger.h>

namespace radray {

namespace text_encoding {

#ifdef RADRAY_PLATFORM_WINDOWS
static void LogWinCharCvtErr() {
    DWORD err = ::GetLastError();
    std::string_view strErr = ([err]() {
        switch (err) {
            case ERROR_INSUFFICIENT_BUFFER: return "insufficient buffer";
            case ERROR_INVALID_FLAGS: return "invalid flags";
            case ERROR_INVALID_PARAMETER: return "invalid parameter";
            case ERROR_NO_UNICODE_TRANSLATION: return "no unicode translation";
            default: return "UNKNOWN";
        }
    })();
    RADRAY_ERR_LOG("cannot convert char to wchar: {} (code={})", err, strErr, err);
}
#endif
// TODO: use lib to support utf8 on other platforms
std::optional<wstring> ToWideChar(std::string_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (str.size() >= std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    int test = ::MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (test == 0) {
        LogWinCharCvtErr();
        return std::nullopt;
    }
    wstring to(test, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), to.data(), (int)to.size());
    return to;
#else
    const char* start = str.data();
    std::mbstate_t state{};
    size_t len = std::mbsrtowcs(nullptr, &start, str.size(), &state);
    if (len == static_cast<size_t>(-1)) {
        return std::nullopt;
    }
    wstring wstr(len, L'\0');
    state = mbstate_t{};
    size_t result = std::mbsrtowcs(&wstr[0], &start, str.size(), &state);
    RADRAY_ASSERT(result == wstr.size());
    return result == static_cast<size_t>(-1) ? std::nullopt : std::make_optional(wstr);
#endif
}

std::optional<string> ToMultiByte(std::wstring_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (str.size() >= std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    int test = ::WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0, nullptr, nullptr);
    if (test == 0) {
        LogWinCharCvtErr();
        return std::nullopt;
    }
    string to(test, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), to.data(), (int)to.size(), nullptr, nullptr);
    return to;
#else
    size_t len = std::wcstombs(nullptr, str.data(), str.size());
    if (len == static_cast<size_t>(-1)) {
        return std::nullopt;
    }
    string wstr(len, L'\0');
    size_t result = std::wcstombs(wstr.data(), str.data(), str.size());
    RADRAY_ASSERT(result == str.size());
    return result == static_cast<size_t>(-1) ? std::nullopt : std::make_optional(wstr);
#endif
}

}  // namespace text_encoding

}  // namespace radray
