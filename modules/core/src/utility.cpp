#include <radray/utility.h>

#include <fstream>
#include <limits>
#include <bit>

#include <xxhash.h>

#include <radray/platform.h>
#include <radray/logger.h>

#ifdef RADRAY_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WINDOWS
#define _WINDOWS
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#endif

namespace radray {

std::optional<string> ReadText(const std::filesystem::path& filepath) noexcept {
    if (!std::filesystem::exists(filepath)) {
        return std::nullopt;
    }
    std::ifstream t{filepath};
    string str(
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
            default: return "UNKNOWN";
        }
    })();
    RADRAY_ERR_LOG("cannot convert char to wchar, reason={} (code={})", strErr, err);
}
#endif
// TODO: use lib to support utf8 on other platforms
std::optional<wstring> ToWideChar(std::string_view str) noexcept {
#ifdef RADRAY_PLATFORM_WINDOWS
    if (str.size() >= std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    int test = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0);
    if (test == 0) {
        LogWinCharCvtErr();
        return std::nullopt;
    }
    wstring to(test, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), to.data(), (int)to.size());
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
    int test = WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), nullptr, 0, nullptr, nullptr);
    if (test == 0) {
        LogWinCharCvtErr();
        return std::nullopt;
    }
    string to(test, '\0');
    WideCharToMultiByte(CP_UTF8, 0, str.data(), (int)str.size(), to.data(), (int)to.size(), nullptr, nullptr);
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

vector<uint32_t> ByteToDWORD(std::span<uint8_t> bytes) noexcept {
    size_t quo = bytes.size() / 4;
    size_t remain = bytes.size() % 4;
    vector<uint32_t> result;
    result.resize(quo + (remain == 0 ? 0 : 1));
    for (size_t i = 0; i < quo; i++) {
        size_t p = i * 4;
        uint32_t a = (bytes[p]);
        uint32_t b = (bytes[p + 1]);
        uint32_t c = (bytes[p + 2]);
        uint32_t d = (bytes[p + 3]);
        uint32_t dword;
        if constexpr (std::endian::native == std::endian::little) {
            dword = a | (b << 8) | (c << 16) | (d << 24);
        } else if constexpr (std::endian::native == std::endian::big) {
            dword = (a << 24) | (b << 16) | (c << 8) | d;
        } else {
            static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big, "unknown endian platform");
        }
        result[i] = dword;
    }
    if (remain != 0) {
        uint32_t last = 0;
        for (size_t i = 0; i < remain; i++) {
            uint32_t a = (bytes[quo * 4 + i]);
            if constexpr (std::endian::native == std::endian::little) {
                last |= a << (i * 8);
            } else if constexpr (std::endian::native == std::endian::big) {
                last |= a << (24 - i * 8);
            } else {
                static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big, "unknown endian platform");
            }
        }
        result[quo] = last;
    }
    return result;
}

size_t HashData(const void* data, size_t size) noexcept {
    if constexpr (sizeof(size_t) == sizeof(uint32_t)) {
        return XXH32(data, size, 0);
    } else if constexpr (sizeof(size_t) == sizeof(uint64_t)) {
        return XXH64(data, size, 0);
    } else {
        static_assert(sizeof(size_t) == sizeof(uint32_t) || sizeof(size_t) == sizeof(uint64_t), "unknown size_t size");
    }
}

}  // namespace radray
