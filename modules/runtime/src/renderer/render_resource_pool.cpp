#include <radray/runtime/renderer/render_resource_pool.h>

#include <span>

#include <radray/logger.h>

namespace radray {

namespace {

// 两个 TextureDescriptor 是否等价(决定能否复用而非重建)。
bool SameTextureDesc(const render::TextureDescriptor& a, const render::TextureDescriptor& b) noexcept {
    return a.Dim == b.Dim &&
           a.Width == b.Width &&
           a.Height == b.Height &&
           a.DepthOrArraySize == b.DepthOrArraySize &&
           a.MipLevels == b.MipLevels &&
           a.SampleCount == b.SampleCount &&
           a.Format == b.Format &&
           a.Memory == b.Memory &&
           a.Usage == b.Usage &&
           a.Hints == b.Hints;
}

// 两个 TextureViewDescriptor 是否等价(决定能否命中已缓存 view)。
bool SameViewDesc(const render::TextureViewDescriptor& a, const render::TextureViewDescriptor& b) noexcept {
    return a.Target == b.Target &&
           a.Dim == b.Dim &&
           a.Format == b.Format &&
           a.Range.BaseArrayLayer == b.Range.BaseArrayLayer &&
           a.Range.ArrayLayerCount == b.Range.ArrayLayerCount &&
           a.Range.BaseMipLevel == b.Range.BaseMipLevel &&
           a.Range.MipLevelCount == b.Range.MipLevelCount &&
           a.Usage == b.Usage;
}

}  // namespace

RenderResourcePool::Entry* RenderResourcePool::FindEntry(std::string_view name, uint32_t flight) noexcept {
    for (Entry& e : _entries) {
        if (e.Flight == flight && std::string_view{e.Name} == name) {
            return &e;
        }
    }
    return nullptr;
}

const RenderResourcePool::Entry* RenderResourcePool::FindEntry(std::string_view name, uint32_t flight) const noexcept {
    for (const Entry& e : _entries) {
        if (e.Flight == flight && std::string_view{e.Name} == name) {
            return &e;
        }
    }
    return nullptr;
}

render::Texture* RenderResourcePool::Acquire(
    std::string_view name,
    uint32_t flight,
    const render::TextureDescriptor& desc,
    render::Device& device) {
    Entry* entry = FindEntry(name, flight);
    if (entry != nullptr && entry->Tex != nullptr && SameTextureDesc(entry->Desc, desc)) {
        return entry->Tex.get();  // desc 匹配:原样复用,不动跟踪态。
    }

    if (entry == nullptr) {
        Entry fresh{};
        fresh.Name = string{name};
        fresh.Flight = flight;
        _entries.push_back(std::move(fresh));
        entry = &_entries.back();
    }

    // 重建:旧 view 的 Target 即将失效,先丢弃;再释放旧纹理并重置跟踪态。
    entry->Views.clear();
    entry->Tex.reset();
    entry->State = render::TextureState::Undefined;

    auto texOpt = device.CreateTexture(desc);
    if (!texOpt.HasValue()) {
        RADRAY_ERR_LOG("RenderResourcePool: failed to create texture '{}'", name);
        return nullptr;
    }
    entry->Tex = texOpt.Release();
    entry->Tex->SetDebugName(name);
    entry->Desc = desc;
    return entry->Tex.get();
}

render::Texture* RenderResourcePool::Find(std::string_view name, uint32_t flight) const noexcept {
    const Entry* entry = FindEntry(name, flight);
    return entry != nullptr ? entry->Tex.get() : nullptr;
}

render::TextureView* RenderResourcePool::GetView(
    std::string_view name,
    uint32_t flight,
    const render::TextureViewDescriptor& viewDesc,
    render::Device& device) {
    Entry* entry = FindEntry(name, flight);
    if (entry == nullptr || entry->Tex == nullptr) {
        return nullptr;
    }

    // Target 由池托管:始终指向当前纹理,调用方填的 Target 被忽略。
    render::TextureViewDescriptor desc = viewDesc;
    desc.Target = entry->Tex.get();

    for (ViewEntry& v : entry->Views) {
        if (v.View != nullptr && SameViewDesc(v.Desc, desc)) {
            return v.View.get();
        }
    }

    auto viewOpt = device.CreateTextureView(desc);
    if (!viewOpt.HasValue()) {
        RADRAY_ERR_LOG("RenderResourcePool: failed to create view for '{}'", name);
        return nullptr;
    }
    ViewEntry ve{};
    ve.Desc = desc;
    ve.View = viewOpt.Release();
    render::TextureView* result = ve.View.get();
    entry->Views.push_back(std::move(ve));
    return result;
}

void RenderResourcePool::Transition(
    std::string_view name,
    uint32_t flight,
    render::TextureStates state,
    render::CommandBuffer& cmd) {
    Entry* entry = FindEntry(name, flight);
    if (entry == nullptr || entry->Tex == nullptr) {
        return;
    }
    if (entry->State == state) {
        return;  // 已是目标态:无需 barrier。
    }
    render::ResourceBarrierDescriptor barrier = render::BarrierTextureDescriptor{
        .Target = entry->Tex.get(),
        .Before = entry->State,
        .After = state};
    cmd.ResourceBarrier(std::span{&barrier, 1});
    entry->State = state;
}

void RenderResourcePool::Clear() noexcept {
    _entries.clear();
}

}  // namespace radray
