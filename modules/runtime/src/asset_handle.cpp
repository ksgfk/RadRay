#include <radray/runtime/asset_handle.h>

#include <radray/hash.h>

namespace radray {

AssetId MakeAssetId(const Uri& uri) noexcept {
    const auto text = uri.AsString();
    return {uri.IsAbsoluteUri() ? HashData64(text.data(), text.size()) : 0};
}

AssetId MakeAssetId(std::string_view virtualPath) {
    if (virtualPath.find("://") != virtualPath.npos) {
        const auto uri = Uri::TryCreate(virtualPath);
        return uri ? MakeAssetId(*uri) : AssetId{};
    }
    return {virtualPath.empty() ? 0 : HashData64(virtualPath.data(), virtualPath.size())};
}

}  // namespace radray
