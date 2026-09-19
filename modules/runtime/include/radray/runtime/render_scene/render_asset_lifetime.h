#pragma once

#include <radray/runtime/asset_manager.h>

namespace radray {

/// GT only. Owns assets used by one Scene; must outlive its readers and die before AssetManager.
/// Retirement requires ordered Scene delivery and a single ordered GPU queue covering all asset uses.
class RenderAssetLifetime {
public:
    explicit RenderAssetLifetime(uint32_t flightCount);
    ~RenderAssetLifetime() noexcept;
    RenderAssetLifetime(const RenderAssetLifetime&) = delete;
    RenderAssetLifetime& operator=(const RenderAssetLifetime&) = delete;
    RenderAssetLifetime(RenderAssetLifetime&&) = delete;
    RenderAssetLifetime& operator=(RenderAssetLifetime&&) = delete;

    void AddUse(const StreamingAssetRefAny& asset);
    void RemoveUse(AssetId id);
    /// After all binding changes for this flight; only visits candidates whose use count reached zero.
    void SealRetirements(uint32_t flightIndex);
    /// After this flight's real GPU completion, or an unpublished flight after RT stops and GPU is idle.
    void ReleaseFlight(uint32_t flightIndex);
    /// Shutdown only, after RT stops and GPU is idle. Outstanding bindings may remain.
    void Clear() noexcept;

private:
    struct AssetUse {
        StreamingAssetRefAny Owner;
        size_t Count{0};
        bool RetirementQueued{false};
    };

    vector<StreamingAssetRefAny>& GetRetiredAssets(uint32_t flightIndex);

    unordered_map<AssetId, AssetUse> _uses;
    vector<AssetId> _retirementCandidates;
    vector<vector<StreamingAssetRefAny>> _retiredAssets;
};

}  // namespace radray
