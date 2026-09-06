#include <radray/runtime/render_framework/view_state.h>

#include <algorithm>
#include <radray/logger.h>

namespace radray {

struct ViewStateRegistry::Impl {
    struct Image {
        unique_ptr<render::Texture> Texture;
        vector<PooledTextureView> Views;
        vector<render::TextureStates> States;
        vector<uint8_t> Valid;
        std::optional<RenderExternalTexture> External;
    };
    struct Generation {
        uint64_t Id{0};
        TexturePoolKey Key;
        vector<unique_ptr<Image>> Images;
        uint32_t LastCommitted{0};
        bool HasCommitted{false};
        uint64_t AcquiredFrame{0}, CommittedFrame{0};
        HistoryCommitMode CommitMode{HistoryCommitMode::Independent};
    };
    struct View {
        Eigen::Matrix4f Previous{Eigen::Matrix4f::Identity()}, Pending{Eigen::Matrix4f::Identity()};
        RenderExtent PreviousExtent, PendingExtent;
        render::TextureFormat Format{render::TextureFormat::UNKNOWN};
        uint32_t Samples{0};
        uint64_t LastSeen{0}, LastCommitted{0}, PendingFrame{0};
        bool PreviousValid{false}, Available{false};
        ViewHistoryInvalidationReason Reason{ViewHistoryInvalidationReason::FirstFrame};
        unordered_map<ViewHistoryKey, unique_ptr<Generation>> Histories;
        PrimitiveHistory Primitives;
        uint64_t PrimitivePreparedFrame{0};
    };
    render::Device& Device;
    render::RenderPassRegistry& Registry;
    unordered_map<ViewStateId, View, ViewStateIdHash> Views;
    vector<vector<unique_ptr<Generation>>> RetireBins;
    uint32_t Flight{0};
    uint64_t Serial{0}, NextGeneration{1}, InactiveFrames{120}, TexturesCreated{0}, GenerationsDestroyed{0};
    Impl(render::Device& device, render::RenderPassRegistry& registry, uint32_t flights, uint64_t inactive)
        : Device(device), Registry(registry), RetireBins(flights), InactiveFrames(inactive) { RADRAY_ASSERT(flights > 0); }
    void Destroy(Generation& generation) {
        for (const auto& image : generation.Images) {
            for (const auto& view : image->Views) Registry.RemoveFramebuffersUsing(view.View.get());
            image->Views.clear();
        }
        generation.Images.clear();
        ++GenerationsDestroyed;
    }
    void Invalidate(View& view, ViewHistoryInvalidationReason reason, bool temporalOnly = false) {
        view.PreviousValid = false;
        view.Primitives.Invalidate();
        view.Reason = reason;
        for (auto& [key, generation] : view.Histories) {
            if (!generation || (temporalOnly && generation->CommitMode != HistoryCommitMode::WithView)) continue;
            generation->HasCommitted = false;
            for (auto& image : generation->Images) std::fill(image->Valid.begin(), image->Valid.end(), uint8_t{0});
        }
    }
    bool ValidHistory(const View& view, const Generation& generation, const HistoryWriteToken& token) const {
        if (token.FrameSerial != Serial || !view.Available || view.PendingFrame != Serial ||
            generation.CommitMode != token.CommitMode || generation.Id != token.Generation || generation.AcquiredFrame != Serial ||
            generation.CommittedFrame == Serial || token.Index >= generation.Images.size()) return false;
        const uint32_t expected = generation.HasCommitted ? (generation.LastCommitted + 1) % static_cast<uint32_t>(generation.Images.size()) : 0;
        if (token.Index != expected) return false;
        const auto& current = *generation.Images[token.Index];
        return current.External->Written && std::all_of(current.Valid.begin(), current.Valid.end(), [](uint8_t value) { return value != 0; });
    }
    void AdvanceView(View& view) noexcept {
        view.Previous = view.Pending;
        view.PreviousExtent = view.PendingExtent;
        view.LastCommitted = Serial;
        view.PreviousValid = true;
        view.Reason = ViewHistoryInvalidationReason::None;
    }
};

ViewStateRegistry::ViewStateRegistry(render::Device& device, render::RenderPassRegistry& registry, uint32_t flightCount, uint64_t inactiveFrames)
    : _impl(make_unique<Impl>(device, registry, flightCount, inactiveFrames)) {}
ViewStateRegistry::~ViewStateRegistry() { Clear(); }

void ViewStateRegistry::BeginFlight(uint32_t flight, uint64_t serial) {
    auto& impl = *_impl;
    RADRAY_ASSERT(flight < impl.RetireBins.size() && serial > impl.Serial);
    impl.Flight = flight;
    impl.Serial = serial;
    for (auto& generation : impl.RetireBins[flight]) impl.Destroy(*generation);
    impl.RetireBins[flight].clear();
    std::erase_if(impl.Views, [&](auto& item) {
        auto& view = item.second;
        if (serial - view.LastSeen <= impl.InactiveFrames) return false;
        for (auto& [key, generation] : view.Histories)
            if (generation) impl.RetireBins[flight].push_back(std::move(generation));
        return true;
    });
}

void ViewStateRegistry::Resolve(ResolvedRenderView& view, const ResolvedRenderViewFamily& family) {
    if (!view.StateId.IsValid()) return;
    auto& impl = *_impl;
    auto& record = impl.Views[view.StateId];
    RADRAY_ASSERT(record.PendingFrame != impl.Serial);
    if (view.CameraCut)
        impl.Invalidate(record, ViewHistoryInvalidationReason::CameraCut);
    else if (record.LastSeen != 0 && record.PendingExtent != family.RenderSize)
        impl.Invalidate(record, ViewHistoryInvalidationReason::ExtentChanged);
    else if (record.LastSeen != 0 && record.Format != family.OutputFormat)
        impl.Invalidate(record, ViewHistoryInvalidationReason::FormatChanged);
    else if (record.LastSeen != 0 && record.Samples != family.SampleCount)
        impl.Invalidate(record, ViewHistoryInvalidationReason::SampleCountChanged);
    record.Pending = view.ViewProjection;
    record.PendingExtent = family.RenderSize;
    record.PendingFrame = impl.Serial;
    record.LastSeen = impl.Serial;
    record.Format = family.OutputFormat;
    record.Samples = family.SampleCount;
    record.Available = family.OutputAvailable;
    view.PreviousViewValid = record.PreviousValid;
    view.PreviousViewProjection = record.PreviousValid ? record.Previous : view.ViewProjection;
}

bool ViewStateRegistry::CommitView(ViewStateId id) {
    auto& impl = *_impl;
    const auto found = impl.Views.find(id);
    if (found == impl.Views.end()) return false;
    auto& view = found->second;
    if (!view.Available || view.PendingFrame != impl.Serial || view.LastCommitted == impl.Serial) return false;
    if (view.PrimitivePreparedFrame == impl.Serial) return false;
    for (const auto& [key, generation] : view.Histories)
        if (generation && generation->CommitMode == HistoryCommitMode::WithView && generation->AcquiredFrame == impl.Serial) return false;
    impl.AdvanceView(view);
    return true;
}

bool ViewStateRegistry::CommitViewWithHistory(ViewStateId id, std::span<const HistoryWriteToken> tokens) {
    auto& impl = *_impl;
    const auto found = impl.Views.find(id);
    if (found == impl.Views.end()) return false;
    auto& view = found->second;
    if (!view.Available || view.PendingFrame != impl.Serial || view.LastCommitted == impl.Serial) return false;
    size_t acquired = 0;
    for (const auto& [key, generation] : view.Histories)
        if (generation && generation->CommitMode == HistoryCommitMode::WithView && generation->AcquiredFrame == impl.Serial) ++acquired;
    if (tokens.size() != acquired) return false;
    for (size_t index = 0; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.View != id || token.CommitMode != HistoryCommitMode::WithView) return false;
        for (size_t earlier = 0; earlier < index; ++earlier)
            if (tokens[earlier].Key == token.Key) return false;
        const auto history = view.Histories.find(token.Key);
        if (history == view.Histories.end() || !history->second || !impl.ValidHistory(view, *history->second, token)) return false;
    }
    const bool hasPrimitives = view.PrimitivePreparedFrame == impl.Serial;
    if (hasPrimitives && !view.Primitives.CanCommit(impl.Serial)) return false;
    for (const auto& token : tokens) {
        auto& generation = *view.Histories.find(token.Key)->second;
        generation.LastCommitted = token.Index;
        generation.HasCommitted = true;
        generation.CommittedFrame = impl.Serial;
    }
    if (hasPrimitives) view.Primitives.Commit(impl.Serial);
    impl.AdvanceView(view);
    return true;
}

bool ViewStateRegistry::PreparePrimitiveHistory(ResolvedRenderView& view, const RenderSceneSnapshot& snapshot) {
    auto& impl = *_impl;
    const auto found = impl.Views.find(view.StateId);
    if (found == impl.Views.end() || !found->second.Available || found->second.PendingFrame != impl.Serial) return false;
    view.PreviousViewValid = found->second.PreviousValid;
    view.PreviousViewProjection = view.PreviousViewValid ? found->second.Previous : view.ViewProjection;
    found->second.PrimitivePreparedFrame = impl.Serial;
    return found->second.Primitives.Prepare(snapshot, impl.Serial);
}

PrimitiveMotionData ViewStateRegistry::GetPrimitiveMotion(ViewStateId id, const RenderPrimitiveData& primitive) const noexcept {
    const auto found = _impl->Views.find(id);
    return found == _impl->Views.end() ? PrimitiveMotionData{primitive.LocalToWorld, false} : found->second.Primitives.Lookup(primitive);
}
uint64_t ViewStateRegistry::GetCommittedSerial(ViewStateId id) const noexcept {
    const auto found = _impl->Views.find(id);
    return found == _impl->Views.end() ? 0 : found->second.LastCommitted;
}
uint64_t ViewStateRegistry::GetPrimitiveCommittedSerial(ViewStateId id) const noexcept {
    const auto found = _impl->Views.find(id);
    return found == _impl->Views.end() ? 0 : found->second.Primitives.CommittedSerial();
}
void ViewStateRegistry::InvalidateTemporal(ViewStateId id, ViewHistoryInvalidationReason reason) {
    const auto found = _impl->Views.find(id);
    if (found != _impl->Views.end()) _impl->Invalidate(found->second, reason, true);
}

void ViewStateRegistry::Invalidate(ViewStateId id, ViewHistoryInvalidationReason reason) {
    auto& impl = *_impl;
    const auto found = impl.Views.find(id);
    if (found != impl.Views.end()) impl.Invalidate(found->second, reason);
}

HistoryTexturePair ViewStateRegistry::AcquireHistoryTexture(const ResolvedRenderView& view, const ResolvedRenderViewFamily& family,
                                                            const HistoryTextureRequest& request, string& reason) {
    auto& impl = *_impl;
    const auto found = impl.Views.find(view.StateId);
    if (!view.StateId.IsValid() || found == impl.Views.end() || found->second.PendingFrame != impl.Serial || !family.OutputAvailable ||
        request.Key.empty() || request.BufferCount < 2 || request.BufferCount > 4 || !EnumContains(request.CommitMode)) {
        reason = "History requires a resolved available view, nonempty key and 2..4 buffers";
        return {};
    }
    const auto desc = ResolveRuntimeTextureDesc(request.Desc, family, impl.Device, reason);
    if (!desc) return {};
    auto& record = found->second;
    auto& generation = record.Histories[request.Key];
    if (generation && generation->AcquiredFrame == impl.Serial) {
        reason = "History key already acquired for this view and frame";
        return {};
    }
    if (!generation || !(generation->Key == TexturePoolKey{*desc}) || generation->Images.size() != request.BufferCount || generation->CommitMode != request.CommitMode) {
        auto next = make_unique<Impl::Generation>();
        next->Id = impl.NextGeneration++;
        next->Key = {*desc};
        next->CommitMode = request.CommitMode;
        for (uint32_t i = 0; i < request.BufferCount; ++i) {
            auto texture = impl.Device.CreateTexture(*desc);
            if (!texture) {
                reason = "History texture allocation failed";
                impl.Destroy(*next);
                return {};
            }
            auto image = make_unique<Impl::Image>();
            image->Texture = texture.Release();
            image->Texture->SetDebugName(fmt::format("{}.{}.{}", request.DebugName, next->Id, i));
            const uint32_t cells = desc->MipLevels * (desc->Dim == render::TextureDimension::Dim3D ? 1 : desc->DepthOrArraySize);
            image->States.assign(cells, render::TextureState::Undefined);
            image->Valid.assign(cells, 0);
            image->External.emplace(RenderExternalTexture{image->Texture.get(), *desc, image->States, image->Valid, nullptr, false, &image->Views});
            next->Images.push_back(std::move(image));
            ++impl.TexturesCreated;
        }
        if (generation) {
            if (request.CommitMode == HistoryCommitMode::WithView || generation->CommitMode == HistoryCommitMode::WithView)
                impl.Invalidate(record, ViewHistoryInvalidationReason::FormatChanged, true);
            impl.RetireBins[impl.Flight].push_back(std::move(generation));
        }
        generation = std::move(next);
    }
    generation->AcquiredFrame = impl.Serial;
    const uint32_t current = generation->HasCommitted ? (generation->LastCommitted + 1) % request.BufferCount : 0;
    auto& currentImage = *generation->Images[current];
    std::fill(currentImage.Valid.begin(), currentImage.Valid.end(), uint8_t{0});
    currentImage.External->Written = false;
    auto* previous = generation->HasCommitted ? &*generation->Images[generation->LastCommitted]->External : nullptr;
    const bool valid = previous && std::all_of(previous->ContentValid.begin(), previous->ContentValid.end(), [](uint8_t value) { return value != 0; });
    return {valid ? previous : nullptr, &*currentImage.External, valid, {view.StateId, request.Key, generation->Id, impl.Serial, current, request.CommitMode}};
}

bool ViewStateRegistry::CommitHistory(const HistoryWriteToken& token) {
    auto& impl = *_impl;
    const auto view = impl.Views.find(token.View);
    if (view == impl.Views.end() || token.FrameSerial != impl.Serial || !view->second.Available) return false;
    const auto history = view->second.Histories.find(token.Key);
    if (history == view->second.Histories.end() || !history->second) return false;
    auto& generation = *history->second;
    if (generation.CommitMode != HistoryCommitMode::Independent || !impl.ValidHistory(view->second, generation, token)) return false;
    generation.LastCommitted = token.Index;
    generation.HasCommitted = true;
    generation.CommittedFrame = impl.Serial;
    return true;
}

ViewHistoryInvalidationReason ViewStateRegistry::GetInvalidationReason(ViewStateId id) const {
    const auto found = _impl->Views.find(id);
    return found == _impl->Views.end() ? ViewHistoryInvalidationReason::FirstFrame : found->second.Reason;
}
ViewStateStats ViewStateRegistry::GetStats() const {
    ViewStateStats result{};
    result.ActiveViews = static_cast<uint32_t>(_impl->Views.size());
    for (const auto& [key, view] : _impl->Views)
        for (const auto& [history, generation] : view.Histories)
            if (generation) ++result.HistoryGenerations;
    for (const auto& bin : _impl->RetireBins) result.RetiredGenerations += static_cast<uint32_t>(bin.size());
    result.TexturesCreated = _impl->TexturesCreated;
    result.GenerationsDestroyed = _impl->GenerationsDestroyed;
    return result;
}
void ViewStateRegistry::Clear() {
    auto& impl = *_impl;
    for (auto& [id, view] : impl.Views)
        for (auto& [key, generation] : view.Histories)
            if (generation) impl.Destroy(*generation);
    impl.Views.clear();
    for (auto& bin : impl.RetireBins) {
        for (auto& generation : bin) impl.Destroy(*generation);
        bin.clear();
    }
}

}  // namespace radray
