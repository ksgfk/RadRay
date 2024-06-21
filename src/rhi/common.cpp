#include <radray/rhi/common.h>

namespace radray::rhi {

const char* to_string(ApiType val) noexcept {
    switch (val) {
        case radray::rhi::ApiType::D3D12: return "D3D12";
        case radray::rhi::ApiType::Metal: return "Metal";
        case radray::rhi::ApiType::MAX_COUNT: return "UNKNOWN";
    }
}

}  // namespace radray::rhi
