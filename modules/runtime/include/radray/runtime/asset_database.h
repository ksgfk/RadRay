#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include <radray/lmdb.h>
#include <radray/types.h>

#include <radray/runtime/asset.h>

namespace radray {

class AssetDatabase {
public:
    AssetDatabase(const std::filesystem::path& assetRoot, const std::filesystem::path& storePath);
    ~AssetDatabase() = default;
    AssetDatabase(const AssetDatabase&) = delete;
    AssetDatabase& operator=(const AssetDatabase&) = delete;
    AssetDatabase(AssetDatabase&&) = delete;
    AssetDatabase& operator=(AssetDatabase&&) = delete;

    struct ResolvedAsset {
        std::filesystem::path AbsolutePath;
        string Type;
        vector<byte> Data;
    };

    std::optional<ResolvedAsset> Resolve(const AssetId& id);

    std::optional<AssetId> AddEntry(std::string_view relPath, std::string_view type);

private:
    std::filesystem::path _assetRoot;
    LmdbEnvironment _env;
    LmdbDatabase _assetsDbi{0};
};

}  // namespace radray
