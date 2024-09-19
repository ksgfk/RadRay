#include <radray/utility.h>

#include <fstream>
#include <stdexcept>

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

}  // namespace radray
