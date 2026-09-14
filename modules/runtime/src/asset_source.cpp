#include <radray/runtime/asset_source.h>

#include <utility>

namespace radray {

AssetLoadResult AssetLoadResult::Success(unique_ptr<Asset> object) noexcept {
    AssetLoadResult result;
    result.Object = std::move(object);
    result.Succeeded = true;
    return result;
}

AssetLoadResult AssetLoadResult::Failure(string error) noexcept {
    AssetLoadResult result;
    result.Error = std::move(error);
    return result;
}

bool AssetLoadResult::IsSuccess() const noexcept {
    return Succeeded && Object != nullptr;
}

IAssetSource::~IAssetSource() noexcept = default;

}  // namespace radray
