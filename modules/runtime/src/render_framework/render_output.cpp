#include <radray/runtime/render_framework/render_output.h>

#include <algorithm>
#include <radray/logger.h>

namespace radray {
namespace {
std::atomic<uint64_t> NextOutputId{1};

RenderOutputId AllocateOutputId() noexcept {
    const uint64_t value = NextOutputId.fetch_add(1, std::memory_order_relaxed);
    if (value == 0 || value == UINT64_MAX) RADRAY_ABORT("RenderOutputId exhausted");
    return {value};
}
bool ValidExternalState(const render::TextureDescriptor& desc, render::TextureStates state, bool allowUndefined) {
    using enum render::TextureState;
    if (state == Undefined) return allowUndefined;
    constexpr uint32_t allowed = uint32_t(Common) | uint32_t(CopySource) | uint32_t(CopyDestination) | uint32_t(ShaderRead) | uint32_t(RenderTarget) | uint32_t(UnorderedAccess);
    if (!state || (state.value() & ~allowed) != 0) return false;
    const uint32_t exclusive = uint32_t(Common) | uint32_t(CopyDestination) | uint32_t(RenderTarget) | uint32_t(UnorderedAccess);
    if ((state.value() & exclusive) != 0 && (state.value() & (state.value() - 1)) != 0) return false;
    for (const auto [bit, usage] : {std::pair{CopySource, render::TextureUse::CopySource}, {CopyDestination, render::TextureUse::CopyDestination}, {ShaderRead, render::TextureUse::Resource}, {RenderTarget, render::TextureUse::RenderTarget}, {UnorderedAccess, render::TextureUse::UnorderedAccess}}) {
        if (state.HasFlag(bit) && !desc.Usage.HasFlag(usage)) return false;
    }
    return true;
}
}  // namespace

RenderOutputRegistry::RenderOutputRegistry() : _gameThread(std::this_thread::get_id()) {}

void RenderOutputRegistry::AssertMutable() const noexcept {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    RADRAY_ASSERT(_renderIdle);
}

void RenderOutputRegistry::SetRenderIdle(bool idle) noexcept {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    _renderIdle = idle;
}

RenderOutputId RenderOutputRegistry::RegisterPresentation(string name, const render::TextureDescriptor& desc, RenderOutputUsage usage) {
    AssertMutable();
    const auto id = AllocateOutputId();
    _records.push_back({{id, RenderOutputKind::Presentation, std::move(name), desc.Width, desc.Height, desc.Format, desc.SampleCount, true, usage}, {}});
    return id;
}

RenderOutputId RenderOutputRegistry::RegisterExternal(const ExternalRenderOutputDesc& desc) {
    AssertMutable();
    if (!desc.Texture || !desc.ColorAttachmentView) return {};
    const auto texture = desc.Texture->GetDesc();
    const auto view = desc.ColorAttachmentView->GetDesc();
    for (const auto& record : _records)
        if (record.External && record.External->Texture.Get() == desc.Texture.Get()) return {};
    if (texture.Dim != render::TextureDimension::Dim2D || texture.MipLevels != 1 || texture.DepthOrArraySize != 1 ||
        render::IsDepthStencilFormat(texture.Format) || !texture.Usage.HasFlag(render::TextureUse::RenderTarget) ||
        view.Target != desc.Texture.Get() || view.Format != texture.Format || view.Usage != render::TextureViewUsage::RenderTarget ||
        render::NormalizeSubresourceRange(texture, view.Range) != render::SubresourceRange{0, 1, 0, 1} ||
        view.Dim != render::TextureDimension::Dim2D ||
        !ValidExternalState(texture, desc.RequiredFinalState, false) || !ValidExternalState(texture, desc.CurrentState, !desc.PreserveContents)) {
        RADRAY_ERR_LOG("Invalid external output '{}'", desc.Name);
        return {};
    }
    const auto id = AllocateOutputId();
    _records.push_back({{id, RenderOutputKind::ExternalColorTexture, desc.Name, texture.Width, texture.Height, texture.Format, texture.SampleCount, true}, desc});
    return id;
}

bool RenderOutputRegistry::Unregister(RenderOutputId id) {
    AssertMutable();
    return std::erase_if(_records, [id](const Record& value) { return value.Info.Id == id; }) != 0;
}

bool RenderOutputRegistry::UpdatePresentation(RenderOutputId id, uint32_t width, uint32_t height, bool active) {
    AssertMutable();
    for (auto& record : _records) {
        if (record.Info.Id == id && record.Info.Kind == RenderOutputKind::Presentation) {
            record.Info.Width = width;
            record.Info.Height = height;
            record.Info.Active = active;
            return true;
        }
    }
    return false;
}

bool RenderOutputRegistry::UpdateExternalOutputState(RenderOutputId id, render::TextureStates state, bool preserveContents) {
    AssertMutable();
    if (state == render::TextureState::UNKNOWN || (preserveContents && state == render::TextureState::Undefined)) return false;
    for (auto& record : _records) {
        if (record.Info.Id == id && record.External) {
            if (!ValidExternalState(record.External->Texture->GetDesc(), state, !preserveContents)) return false;
            record.External->CurrentState = state;
            record.External->PreserveContents = preserveContents;
            return true;
        }
    }
    return false;
}

vector<RenderOutputInfo> RenderOutputRegistry::GetGameThreadInfos() const {
    RADRAY_ASSERT(_gameThread == std::this_thread::get_id());
    vector<RenderOutputInfo> infos;
    infos.reserve(_records.size());
    for (const auto& record : _records) infos.push_back(record.Info);
    return infos;
}

Nullable<const RenderOutputInfo*> RenderOutputRegistry::Find(RenderOutputId id) const noexcept {
    for (const auto& record : _records)
        if (record.Info.Id == id) return &record.Info;
    return nullptr;
}

std::optional<RenderSurfaceFrame> RenderOutputRegistry::ResolveExternal(RenderOutputId id) const {
    for (const auto& record : _records) {
        if (record.Info.Id == id && record.External) {
            const auto& external = *record.External;
            return RenderSurfaceFrame{id, external.Texture.Get(), external.ColorAttachmentView.Get(), external.Texture->GetDesc(),
                                      external.CurrentState, external.RequiredFinalState, external.PreserveContents, false};
        }
    }
    return std::nullopt;
}

void RenderOutputRegistry::CommitExternalState(const RenderSurfaceFrame& surface) {
    for (auto& record : _records) {
        if (record.Info.Id == surface.Id && record.External) {
            record.External->CurrentState = surface.CurrentState;
            return;
        }
    }
}

void RenderOutputRegistry::Clear() {
    AssertMutable();
    _records.clear();
}

}  // namespace radray
