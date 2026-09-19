#include <radray/runtime/render_scene/render_asset_lifetime.h>

#include <radray/logger.h>

namespace radray {

RenderAssetLifetime::RenderAssetLifetime(uint32_t flightCount) : _retiredAssets(flightCount) {
    if (flightCount == 0) RADRAY_ABORT("Asset retirement requires at least one flight");
}

RenderAssetLifetime::~RenderAssetLifetime() noexcept = default;

void RenderAssetLifetime::AddUse(const StreamingAssetRefAny& asset) {
    if (!asset.IsReady()) RADRAY_ABORT("Render binding requires a Ready asset");
    auto [it, inserted] = _uses.try_emplace(asset.GetAssetId());
    auto& use = it->second;
    if (inserted) {
        use.Owner = asset;
    } else if (use.Owner != asset) {
        RADRAY_ABORT("Render asset identity changed");
    }
    ++use.Count;
}

void RenderAssetLifetime::RemoveUse(AssetId id) {
    const auto it = _uses.find(id);
    if (it == _uses.end() || it->second.Count == 0) RADRAY_ABORT("Missing render asset use");
    auto& use = it->second;
    if (--use.Count == 0 && !use.RetirementQueued) {
        _retirementCandidates.push_back(id);
        use.RetirementQueued = true;
    }
}

vector<StreamingAssetRefAny>& RenderAssetLifetime::GetRetiredAssets(uint32_t flightIndex) {
    if (flightIndex >= _retiredAssets.size()) RADRAY_ABORT("Invalid asset retirement flight index");
    return _retiredAssets[flightIndex];
}

void RenderAssetLifetime::SealRetirements(uint32_t flightIndex) {
    auto& retired = GetRetiredAssets(flightIndex);
    if (!retired.empty()) RADRAY_ABORT("Asset retirement flight is still occupied");
    for (const auto id : _retirementCandidates) {
        const auto it = _uses.find(id);
        auto& use = it->second;
        if (use.Count == 0) {
            retired.push_back(std::move(use.Owner));
            _uses.erase(it);
        } else {
            use.RetirementQueued = false;
        }
    }
    _retirementCandidates.clear();
}

void RenderAssetLifetime::ReleaseFlight(uint32_t flightIndex) {
    GetRetiredAssets(flightIndex).clear();
}

void RenderAssetLifetime::Clear() noexcept {
    _uses.clear();
    _retirementCandidates.clear();
    for (auto& retired : _retiredAssets) retired.clear();
}

}  // namespace radray
