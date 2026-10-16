#include <radray/runtime/render_framework/renderer_resources.h>

#include <algorithm>
#include <cstring>
#include <numeric>
#include <radray/logger.h>

namespace radray {
RendererResources::RendererResources(render::Device* device, Guid owner) : _device(device), _owner(owner) {}
RendererResources::~RendererResources() noexcept {
    for ([[maybe_unused]] const auto& slot : _slots) RADRAY_ASSERT(!slot.Value || slot.Value->Frames == 0);
    // Views must die before their borrowed imported textures can be acknowledged by the host.
    _slots.clear();
}
RendererResources::Page::Page(unique_ptr<render::Buffer> buffer, byte* mapped, uint64_t alignment, uint64_t capacity)
    : Buffer(std::move(buffer)), Mapped(mapped), Alignment(alignment), Free(size_t(capacity / alignment)) {}
RendererResources::Page::~Page() noexcept { Buffer->Unmap(); }
Nullable<RendererResources::Entry*> RendererResources::Find(ResourceId id) const noexcept {
    if (id.Owner != _owner || id.Index >= _slots.size() || _slots[id.Index].Generation != id.Generation) return nullptr;
    return _slots[id.Index].Value.get();
}
bool RendererResources::IsValid(ResourceId id, bool allowRetiring) const noexcept {
    const auto entry = Find(id);
    return entry && (allowRetiring || !entry->Retiring);
}
ResourceId RendererResources::Insert(unique_ptr<Entry> entry) {
    const auto index = _free.empty() ? uint32_t(_slots.size()) : _free.back();
    if (_free.empty())
        _slots.emplace_back();
    else
        _free.pop_back();
    _slots[index].Value = std::move(entry);
    return {_owner, index, _slots[index].Generation};
}
std::optional<ResourceId> RendererResources::ImportBuffer(render::Buffer* buffer) {
    if (!buffer->IsValid()) return std::nullopt;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        const auto& entry = _slots[i].Value;
        if (entry && !entry->Retiring && entry->Type == Kind::Buffer && entry->Buffer.Get() == buffer)
            return entry->Retiring ? std::nullopt : std::optional{ResourceId{_owner, i, _slots[i].Generation}};
    }
    auto entry = make_unique<Entry>();
    entry->Type = Kind::Buffer;
    entry->Buffer = buffer;
    entry->Range = {0, buffer->GetDesc().Size};
    return Insert(std::move(entry));
}
std::optional<ResourceId> RendererResources::ImportTexture(render::Texture* texture) {
    if (!texture->IsValid()) return std::nullopt;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        const auto& entry = _slots[i].Value;
        if (entry && !entry->Retiring && entry->Type == Kind::Texture && entry->Texture.Get() == texture)
            return entry->Retiring ? std::nullopt : std::optional{ResourceId{_owner, i, _slots[i].Generation}};
    }
    auto entry = make_unique<Entry>();
    entry->Type = Kind::Texture;
    entry->Texture = texture;
    return Insert(std::move(entry));
}
std::optional<ResourceId> RendererResources::CreateBuffer(const render::BufferDescriptor& descriptor) {
    auto buffer = _device->CreateBuffer(descriptor);
    if (!buffer) return std::nullopt;
    auto entry = make_unique<Entry>();
    entry->Type = Kind::OwnedBuffer;
    entry->OwnedBuffer = buffer.Release();
    entry->Buffer = entry->OwnedBuffer.get();
    entry->Range = {0, descriptor.Size};
    return Insert(std::move(entry));
}
Nullable<render::Buffer*> RendererResources::Buffer(ResourceId id) const noexcept {
    const auto entry = Find(id);
    return entry ? entry->Buffer : nullptr;
}
std::optional<ResourceId> RendererResources::CreateView(ResourceId texture, render::TextureViewDescriptor descriptor) {
    const auto parent = Find(texture);
    if (!parent || parent->Retiring || parent->Type != Kind::Texture) return std::nullopt;
    descriptor.Target = parent->Texture.Get();
    auto view = _device->CreateTextureView(descriptor);
    if (!view) return std::nullopt;
    const auto identity = render::GetShaderParameterValueIdentity(view.Get());
    if (!identity) return std::nullopt;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        const auto& entry = _slots[i].Value;
        if (entry && !entry->Retiring && entry->Type == Kind::View && entry->Parent == texture &&
            render::GetShaderParameterValueIdentity(entry->View.get()) == identity)
            return ResourceId{_owner, i, _slots[i].Generation};
    }
    auto entry = make_unique<Entry>();
    entry->Type = Kind::View;
    entry->View = view.Release();
    entry->Parent = texture;
    ++parent->Registrations;
    return Insert(std::move(entry));
}
std::optional<ResourceId> RendererResources::StoreData(std::span<const byte> bytes, uint32_t stride) {
    const auto& limits = _device->GetCapabilities().Limits;
    if (bytes.empty() || (stride && bytes.size() % stride) || (!stride && limits.MaxUniformBufferRange && bytes.size() > limits.MaxUniformBufferRange)) return std::nullopt;
    HashCode hash;
    hash.Add(stride);
    hash.Add(bytes.size());
    hash.Add(HashData(bytes.data(), bytes.size()));
    const auto key = hash.ToHashCode();
    const auto found = _data.find(key);
    if (found != _data.end()) {
        for (const auto id : found->second) {
            const auto entry = Find(id);
            if (entry && !entry->Retiring && entry->Stride == stride && std::ranges::equal(entry->Bytes, bytes)) return id;
        }
    }
    const uint64_t alignment = stride ? std::lcm(std::max(uint64_t{1}, limits.StorageBufferOffsetAlignment), uint64_t(stride)) : std::max(uint64_t{1}, limits.CBufferOffsetAlignment);
    if (bytes.size() > UINT64_MAX - alignment) return std::nullopt;
    const auto units = (bytes.size() + alignment - 1) / alignment;
    size_t pageIndex = 0;
    std::optional<FirstFitAllocator::Allocation> allocation;
    for (; pageIndex < _pages.size(); ++pageIndex) {
        if (_pages[pageIndex] && _pages[pageIndex]->Alignment == alignment) {
            allocation = _pages[pageIndex]->Free.Allocate(size_t(units));
            if (allocation) break;
        }
    }
    if (!allocation) {
        const auto capacity = std::max(uint64_t{65536}, uint64_t(bytes.size()));
        const auto rounded = (capacity + alignment - 1) / alignment * alignment;
        auto buffer = _device->CreateBuffer({rounded, render::MemoryType::Upload,
                                             render::BufferUse::CBuffer | render::BufferUse::Resource | render::BufferUse::MapWrite, render::ResourceHint::PersistentMap});
        if (!buffer) return std::nullopt;
        auto* mapped = static_cast<byte*>(buffer->Map(0, rounded));
        if (!mapped) return std::nullopt;
        pageIndex = 0;
        while (pageIndex < _pages.size() && _pages[pageIndex]) ++pageIndex;
        if (pageIndex == _pages.size()) _pages.emplace_back();
        _pages[pageIndex] = make_unique<Page>(buffer.Release(), mapped, alignment, rounded);
        allocation = _pages[pageIndex]->Free.Allocate(size_t(units));
        if (!allocation) return std::nullopt;
    }
    auto& page = *_pages[pageIndex];
    const auto offset = uint64_t(allocation->Start) * alignment;
    std::memcpy(page.Mapped + offset, bytes.data(), bytes.size());
    page.Buffer->FlushMappedRange({offset, bytes.size()});
    ++page.Allocations;
    auto entry = make_unique<Entry>();
    entry->Type = Kind::Data;
    entry->Buffer = page.Buffer.get();
    entry->Range = {offset, bytes.size()};
    entry->Stride = stride;
    entry->Bytes.assign(bytes.begin(), bytes.end());
    entry->Hash = key;
    entry->PageIndex = pageIndex;
    entry->Allocation = *allocation;
    const auto id = Insert(std::move(entry));
    _data[key].push_back(id);
    return id;
}
std::optional<ResourceId> RendererResources::CreateBufferRange(ResourceId buffer, render::BufferRange range, uint32_t stride) {
    const auto parent = Find(buffer);
    if (!parent || parent->Retiring || (parent->Type != Kind::Buffer && parent->Type != Kind::OwnedBuffer)) return std::nullopt;
    const auto size = parent->Buffer->GetDesc().Size;
    if (range.Offset > size) return std::nullopt;
    if (range.Size == render::BufferRange::All()) range.Size = size - range.Offset;
    if (!range.Size || range.Size > size - range.Offset || (stride && (range.Offset % stride || range.Size % stride))) return std::nullopt;
    if (!range.Offset && range.Size == size && !stride) return buffer;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        const auto& entry = _slots[i].Value;
        if (entry && !entry->Retiring && entry->Type == Kind::BufferRange && entry->Parent == buffer && entry->Range == range && entry->Stride == stride)
            return ResourceId{_owner, i, _slots[i].Generation};
    }
    auto entry = make_unique<Entry>();
    entry->Type = Kind::BufferRange;
    entry->Buffer = parent->Buffer;
    entry->Range = range;
    entry->Stride = stride;
    entry->Parent = buffer;
    ++parent->Registrations;
    return Insert(std::move(entry));
}
std::optional<ResourceId> RendererResources::CreateSampler(const render::SamplerDescriptor& descriptor) {
    auto sampler = _device->CreateSampler(descriptor);
    if (!sampler) return std::nullopt;
    const auto identity = sampler->GetBindingIdentity();
    if (identity.empty()) return std::nullopt;
    for (uint32_t i = 0; i < _slots.size(); ++i) {
        const auto& entry = _slots[i].Value;
        if (entry && !entry->Retiring && entry->Sampler && entry->Sampler->GetBindingIdentity() == identity)
            return ResourceId{_owner, i, _slots[i].Generation};
    }
    auto entry = make_unique<Entry>();
    entry->Type = Kind::Sampler;
    entry->Sampler = sampler.Release();
    return Insert(std::move(entry));
}
bool RendererResources::DependsOn(ResourceId resource, ResourceId ancestor) const noexcept {
    for (auto entry = Find(resource); entry;) {
        if (resource == ancestor) return true;
        if (!entry->Parent) return false;
        resource = *entry->Parent;
        entry = Find(resource);
    }
    return false;
}
void RendererResources::RequestAllExits(BindingRegistry& bindings) {
    for (uint32_t i = 0; i < _slots.size(); ++i)
        if (_slots[i].Value) RequestExit({_owner, i, _slots[i].Generation}, bindings);
}
bool RendererResources::RetainRegistration(ResourceId id) {
    const auto entry = Find(id);
    if (!entry || entry->Retiring || entry->Registrations == UINT32_MAX) return false;
    ++entry->Registrations;
    return true;
}
bool RendererResources::ReleaseRegistration(ResourceId id) {
    const auto entry = Find(id);
    if (!entry || !entry->Registrations) return false;
    --entry->Registrations;
    return true;
}
bool RendererResources::RetainFrame(ResourceId id) {
    const auto entry = Find(id);
    if (!entry || entry->Retiring || entry->Frames == UINT32_MAX) return false;
    ++entry->Frames;
    return true;
}
bool RendererResources::ReleaseFrame(ResourceId id) {
    const auto entry = Find(id);
    if (!entry || !entry->Frames) return false;
    --entry->Frames;
    return true;
}
bool RendererResources::RequestExit(ResourceId id, BindingRegistry& bindings) {
    const auto entry = Find(id);
    if (!entry) return false;
    entry->Retiring = true;
    bindings.RetireResource(id);
    for (uint32_t i = 0; i < _slots.size(); ++i)
        if (_slots[i].Value && _slots[i].Value->Parent == id)
            RequestExit({_owner, i, _slots[i].Generation}, bindings);
    return true;
}
std::optional<render::ShaderParameterValue> RendererResources::Value(ResourceId id) const {
    const auto entry = Find(id);
    if (!entry) return std::nullopt;
    if (entry->View) return entry->View.get();
    if (entry->Sampler) return entry->Sampler.get();
    if (entry->Buffer) return render::ShaderBufferBinding{entry->Buffer.Get(), entry->Range, entry->Stride};
    return std::nullopt;
}
void RendererResources::Destroy(uint32_t index) {
    auto& slot = _slots[index];
    auto& entry = *slot.Value;
    RADRAY_ASSERT(!entry.Frames && !entry.Registrations);
    if (entry.Parent) ReleaseRegistration(*entry.Parent);
    if (entry.Type == Kind::Data) {
        auto& page = *_pages[entry.PageIndex];
        page.Free.Destroy(entry.Allocation);
        --page.Allocations;
        const auto found = _data.find(entry.Hash);
        std::erase(found->second, ResourceId{_owner, index, slot.Generation});
        if (found->second.empty()) _data.erase(found);
    }
    slot.Value.reset();
    if (slot.Generation != UINT32_MAX) {
        ++slot.Generation;
        _free.push_back(index);
    }
}
vector<ResourceId> RendererResources::Collect(BindingRegistry& bindings) {
    vector<ResourceId> complete;
    bool changed;
    do {
        changed = false;
        for (uint32_t i = 0; i < _slots.size(); ++i) {
            const auto& entry = _slots[i].Value;
            if (!entry || entry->Frames || entry->Registrations || (!entry->Retiring && entry->Type != Kind::Data && entry->Type != Kind::Sampler)) continue;
            const ResourceId id{_owner, i, _slots[i].Generation};
            if (bindings.HasResource(id)) continue;
            if (entry->Retiring) complete.push_back(id);
            bindings.ForgetResource(id);
            Destroy(i);
            changed = true;
        }
    } while (changed);
    TrimEmptyPages();
    return complete;
}
void RendererResources::TrimEmptyPages() {
    for (auto& page : _pages)
        if (page && !page->Allocations) page.reset();
    while (!_pages.empty() && !_pages.back()) _pages.pop_back();
}
}  // namespace radray
