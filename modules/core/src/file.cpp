#include <radray/file.h>

#include <fstream>

#if defined(RADRAY_PLATFORM_WINDOWS)
#include <radray/platform/win32_headers.h>
#elif defined(RADRAY_PLATFORM_APPLE)
#include <mach-o/dyld.h>
#include <vector>
#elif defined(RADRAY_PLATFORM_LINUX)
#include <unistd.h>
#include <climits>
#endif

namespace radray {

std::optional<string> ReadTextFile(const std::filesystem::path& filepath) noexcept {
    if (!std::filesystem::exists(filepath)) {
        return std::nullopt;
    }
    std::ifstream t{filepath};
    string str(
        std::istreambuf_iterator<char>{t},
        std::istreambuf_iterator<char>{});
    return str;
}

std::filesystem::path GetExecutableDirectory() noexcept {
#if defined(RADRAY_PLATFORM_WINDOWS)
    std::wstring buf;
    buf.resize(MAX_PATH);
    for (;;) {
        DWORD len = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return {};
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);  // 路径被截断,扩容重试
    }
    std::filesystem::path exePath{buf};
    return exePath.parent_path();
#elif defined(RADRAY_PLATFORM_APPLE)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);  // 先取所需长度
    std::vector<char> buf(size + 1, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) {
        return {};
    }
    std::filesystem::path exePath{buf.data()};
    return exePath.parent_path();
#elif defined(RADRAY_PLATFORM_LINUX)
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) {
        return {};
    }
    buf[len] = '\0';
    std::filesystem::path exePath{buf};
    return exePath.parent_path();
#else
    return {};
#endif
}

}  // namespace radray
