#include <radray/types.h>

#include <iterator>

#include <radray/platform.h>

namespace radray {

void* RadMalloc(size_t align, size_t size) {
    return AlignedAlloc(align, size);
}

void RadFree(void* ptr) noexcept {
    AlignedFree(ptr);
}

RadString VFormat(fmt::string_view fmtStr, fmt::format_args args) noexcept {
    RadFmtMemBuffer buf{};
    fmt::vformat_to(std::back_inserter(buf), fmtStr, args);
    return RadString{buf.data(), buf.size()};
}

}  // namespace radray
