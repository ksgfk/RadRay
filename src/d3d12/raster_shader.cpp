#include <radray/d3d12/raster_shader.h>

#include <cstdlib>

#include <xxhash.h>

namespace radray::d3d12 {

size_t RasterPipelineStateInfoHash::operator()(const RasterPipelineStateInfo& v) const noexcept {
    static_assert(sizeof(size_t) == sizeof(XXH64_hash_t) || sizeof(size_t) == sizeof(XXH32_hash_t), "no way. 16bit or 128bit+ platform?");
    const RasterPipelineStateInfo* ptr = &v;
    if constexpr (sizeof(size_t) == sizeof(XXH64_hash_t)) {
        XXH64_hash_t hash = XXH64(ptr, sizeof(std::remove_pointer_t<decltype(ptr)>), 0);
        return hash;
    } else if constexpr (sizeof(size_t) == sizeof(XXH32_hash_t)) {
        XXH32_hash_t hash = XXH32(ptr, sizeof(std::remove_pointer_t<decltype(ptr)>), 0);
        return hash;
    } else {
        return 0;
    }
}

bool RasterPipelineStateInfoEqual::operator()(const RasterPipelineStateInfo& l, const RasterPipelineStateInfo& r) const noexcept {
    const RasterPipelineStateInfo* pl = &l;
    const RasterPipelineStateInfo* pr = &l;
    int i = std::memcmp(pl, pr, sizeof(RasterPipelineStateInfo));
    return i == 0;
}

}  // namespace radray::d3d12
