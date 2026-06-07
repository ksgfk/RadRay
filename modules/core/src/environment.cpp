#include <radray/environment.h>

namespace radray {

}  // namespace radray

#ifdef RADRAY_PLATFORM_WINDOWS

#include <radray/platform/win32_headers.h>
#include <cstdlib>

namespace radray {

string GetEnv(std::string_view name) {
    string envName{name};
    char* value = nullptr;
    size_t valueLen = 0;
    auto err = _dupenv_s(&value, &valueLen, envName.c_str());
    if (err != 0 || value == nullptr) {
        return {};
    }
    string result{value};
    std::free(value);
    return result;
}

}  // namespace radray

#else
namespace radray {

string GetEnv(std::string_view name) {
    string envName{name};
    auto* value = std::getenv(envName.c_str());
    if (value == nullptr) {
        return {};
    }
    return string{value};
}

}  // namespace radray
#endif
