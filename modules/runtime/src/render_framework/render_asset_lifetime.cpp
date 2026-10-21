#include <radray/runtime/render_framework/render_asset_lifetime.h>

#include <radray/logger.h>
#include <limits>

namespace radray {

RenderAssetLifetime::RenderAssetLifetime(uint32_t flightCount) : _retiredAssets(flightCount) {
    if (flightCount == 0) RADRAY_ABORT("Asset retirement requires at least one flight");
}

RenderAssetLifetime::~RenderAssetLifetime() noexcept = default;

void RenderAssetLifetime::AddUse(const StreamingAssetRefAny& asset) {
    AddUses(asset, 1);
}
void RenderAssetLifetime::AddUses(const StreamingAssetRefAny& asset, size_t count) {
    if (!count) return;
    if (!asset.IsReady()) RADRAY_ABORT("Render binding requires a Ready asset");
    auto [it, inserted] = _uses.try_emplace(asset.GetAssetId());
    auto& use = it->second;
    if (inserted) {
        use.Owner = asset;
    } else if (use.Owner != asset) {
        RADRAY_ABORT("Render asset identity changed");
    }
    if (count > std::numeric_limits<size_t>::max() - use.Count) RADRAY_ABORT("Render asset use count exhausted");
    use.Count += count;
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

vector<RenderAssetLifetime::Retirement>& RenderAssetLifetime::GetRetiredAssets(uint32_t flightIndex) {
    if (flightIndex >= _retiredAssets.size()) RADRAY_ABORT("Invalid asset retirement flight index");
    return _retiredAssets[flightIndex];
}

vector<AssetExitRequest> RenderAssetLifetime::SealRetirements(uint32_t flightIndex, bool requireConfirmation, bool retireAll) {
    auto& retired = GetRetiredAssets(flightIndex);
    if (!retired.empty()) RADRAY_ABORT("Asset retirement flight is still occupied");
    if (retireAll) {
        _retirementCandidates.clear();
        for (auto& [id, use] : _uses) {
            use.Count = 0;
            _retirementCandidates.push_back(id);
        }
    }
    vector<AssetExitRequest> requests;
    for (const auto id : _retirementCandidates) {
        const auto it = _uses.find(id);
        auto& use = it->second;
        if (use.Count == 0) {
            if (_nextToken == UINT64_MAX) RADRAY_ABORT("Asset exit token exhausted");
            const auto token = _nextToken++;
            retired.push_back({std::move(use.Owner), token, !requireConfirmation});
            if (requireConfirmation) requests.push_back({id, token});
            _uses.erase(it);
        } else {
            use.RetirementQueued = false;
        }
    }
    _retirementCandidates.clear();
    return requests;
}

void RenderAssetLifetime::ReleaseFlight(uint32_t flightIndex) {
    auto& retired = GetRetiredAssets(flightIndex);
    for (auto& entry : retired)
        if (!entry.Confirmed) _awaitingConfirmation.push_back(std::move(entry));
    retired.clear();
}

void RenderAssetLifetime::ConfirmExit(uint64_t token) {
    for (auto& flight : _retiredAssets)
        for (auto& entry : flight)
            if (entry.Token == token) { entry.Confirmed = true; return; }
    std::erase_if(_awaitingConfirmation, [&](const auto& entry) { return entry.Token == token; });
}
bool RenderAssetLifetime::HasPendingExits() const noexcept {
    if (!_awaitingConfirmation.empty()) return true;
    for (const auto& flight : _retiredAssets) if (!flight.empty()) return true;
    return false;
}

void RenderAssetLifetime::Clear() noexcept {
    _uses.clear();
    _retirementCandidates.clear();
    for (auto& retired : _retiredAssets) retired.clear();
    _awaitingConfirmation.clear();
}

}  // namespace radray
