#include <radray/logger.h>
#include <radray/types.h>

void AAA(int a) {
    RADRAY_LOG_DEBUG("AAA");
}

template <typename T>
void p(T&& t) {
    RADRAY_LOG_DEBUG("{}", std::is_function_v<decltype(t)>);
    RADRAY_LOG_DEBUG("{}", std::is_pointer_v<decltype(t)>);
    RADRAY_LOG_DEBUG("{}", std::is_reference_v<decltype(t)>);
    RADRAY_LOG_DEBUG("");
    RADRAY_LOG_DEBUG("{}", std::is_function_v<std::remove_reference_t<decltype(t)>>);
    RADRAY_LOG_DEBUG("{}", std::is_pointer_v<std::remove_reference_t<decltype(t)>>);
    RADRAY_LOG_DEBUG("{}", std::is_reference_v<std::remove_reference_t<decltype(t)>>);
    RADRAY_LOG_DEBUG("");
    RADRAY_LOG_DEBUG("{}", std::is_function_v<std::remove_pointer_t<std::remove_reference_t<decltype(t)>>>);
    RADRAY_LOG_DEBUG("{}", std::is_pointer_v<std::remove_pointer_t<std::remove_reference_t<decltype(t)>>>);
    RADRAY_LOG_DEBUG("{}", std::is_reference_v<std::remove_pointer_t<std::remove_reference_t<decltype(t)>>>);
    RADRAY_LOG_DEBUG("");
    RADRAY_LOG_DEBUG("{}", std::is_function_v<radray::remove_all_cvref_t<decltype(t)>>);
    RADRAY_LOG_DEBUG("{}", std::is_pointer_v<radray::remove_all_cvref_t<decltype(t)>>);
    RADRAY_LOG_DEBUG("{}", std::is_reference_v<radray::remove_all_cvref_t<decltype(t)>>);
}

int main() {
    // RADRAY_LOG_DEBUG("233 {}", 666);
    // RADRAY_LOG_DEBUG_AT_SRC("{}", "holy");
    // RADRAY_LOG_ERROR("ERR");
    // RADRAY_ABORT("core dump");

    p(AAA);
    RADRAY_LOG_DEBUG("---------------------");
    auto e = AAA;
    auto v = e;
    p(v);

    return 0;
}
