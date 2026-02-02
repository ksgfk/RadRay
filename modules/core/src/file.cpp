#include <radray/file.h>

#include <fstream>

namespace radray {

namespace file {

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

}  // namespace file

}  // namespace radray
