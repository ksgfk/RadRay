#include <radray/logger.h>

int main() {
    RADRAY_LOG_DEBUG("233 {}", 666);
    RADRAY_LOG_DEBUG_AT_SRC("{}", "holy");
    RADRAY_LOG_ERROR("ERR");
    // RADRAY_ABORT("core dump");
    return 0;
}
