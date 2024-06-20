#include <radray/rhi/common.h>

namespace radray::rhi {
}

auto std::formatter<radray::rhi::ApiType>::format(radray::rhi::ApiType const& val, format_context& ctx) const -> decltype(ctx.out()) {
    auto str = ([&]() {
        switch (val) {
            case radray::rhi::ApiType::D3D12: return "D3D12";
            case radray::rhi::ApiType::Metal: return "Metal";
            case radray::rhi::ApiType::MAX_COUNT: return "UNKNOWN";
        }
    })();
    return std::formatter<const char*>::format(str, ctx);
}
