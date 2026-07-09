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

std::optional<vector<byte>> ReadBinaryFile(const std::filesystem::path& filepath) noexcept {
    std::error_code ec{};
    if (!std::filesystem::exists(filepath, ec) || ec) {
        return std::nullopt;
    }
    std::ifstream file{filepath, std::ios::binary | std::ios::ate};
    if (!file) {
        return std::nullopt;
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return std::nullopt;
    }
    file.seekg(0, std::ios::beg);
    vector<byte> data(static_cast<size_t>(size));
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        return std::nullopt;
    }
    return data;
}

bool WriteTextFile(const std::filesystem::path& filepath, std::string_view content) noexcept {
    std::error_code ec{};
    if (filepath.has_parent_path()) {
        std::filesystem::create_directories(filepath.parent_path(), ec);
    }
    std::ofstream file{filepath, std::ios::binary | std::ios::trunc};
    if (!file) {
        return false;
    }
    if (!content.empty()) {
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    return static_cast<bool>(file);
}

bool WriteBinaryFile(const std::filesystem::path& filepath, std::span<const byte> data) noexcept {
    std::error_code ec{};
    if (filepath.has_parent_path()) {
        std::filesystem::create_directories(filepath.parent_path(), ec);
    }
    std::ofstream file{filepath, std::ios::binary | std::ios::trunc};
    if (!file) {
        return false;
    }
    if (!data.empty()) {
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(file);
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
