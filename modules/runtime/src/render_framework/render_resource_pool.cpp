#include <radray/runtime/render_framework/render_resource_pool.h>

#include <algorithm>
#include <tuple>
#include <radray/logger.h>

namespace radray {

bool operator==(const TexturePoolKey& lhs, const TexturePoolKey& rhs) noexcept {
    const auto& a = lhs.Desc;
    const auto& b = rhs.Desc;
    return std::tie(a.Dim, a.Width, a.Height, a.DepthOrArraySize, a.MipLevels, a.SampleCount, a.Format, a.Memory, a.Usage, a.Hints) ==
           std::tie(b.Dim, b.Width, b.Height, b.DepthOrArraySize, b.MipLevels, b.SampleCount, b.Format, b.Memory, b.Usage, b.Hints);
}
bool operator==(const BufferPoolKey& lhs, const BufferPoolKey& rhs) noexcept {
    const auto& a = lhs.Desc;
    const auto& b = rhs.Desc;
    return std::tie(a.Size, a.Memory, a.Usage, a.Hints) == std::tie(b.Size, b.Memory, b.Usage, b.Hints);
}

render::BufferStates InitialBufferState(const render::BufferDescriptor& desc) noexcept {
    if (desc.Memory == render::MemoryType::Upload) return render::BufferState::HostWrite;
    if (desc.Memory == render::MemoryType::ReadBack) return render::BufferState::CopyDestination;
    return render::BufferState::Undefined;
}

uint64_t EstimateTextureBytes(const render::TextureDescriptor& desc) noexcept {
    uint64_t result = 0;
    for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
        const uint64_t depth = desc.Dim == render::TextureDimension::Dim3D ? std::max(1u, desc.DepthOrArraySize >> mip) : desc.DepthOrArraySize;
        result += uint64_t{std::max(1u, desc.Width >> mip)} * std::max(1u, desc.Height >> mip) * depth * desc.SampleCount * render::GetTextureFormatBytesPerPixel(desc.Format);
    }
    return result;
}

RenderResourcePool::RenderResourcePool(render::Device& device, render::RenderPassRegistry& registry, uint32_t unusedFlightCycles)
    : _device(device), _registry(registry), _unusedFlightCycles(unusedFlightCycles) {}

RenderResourcePool::~RenderResourcePool() { Clear(); }

void RenderResourcePool::BeginFlight(uint64_t frameSerial) {
#if defined(RADRAY_IS_DEBUG)
    for (const auto& texture : _textures) RADRAY_ASSERT(!texture->InUse);
    for (const auto& buffer : _buffers) RADRAY_ASSERT(!buffer->InUse);
#endif
    for (const auto& view : _externalViews) _registry.RemoveFramebuffersUsing(view.get());
    _externalViews.clear();
    ++_cycle;
    _frameSerial = frameSerial;
    std::erase_if(_textures, [&](const auto& texture) {
        if (_cycle - texture->LastUsedCycle <= _unusedFlightCycles) return false;
        RemoveViews(*texture);
        ++_stats.Trimmed;
        return true;
    });
    std::erase_if(_buffers, [&](const auto& buffer) {
        if (_cycle - buffer->LastUsedCycle <= _unusedFlightCycles) return false;
        ++_stats.Trimmed;
        return true;
    });
    RefreshStats();
}

Nullable<PooledTexture*> RenderResourcePool::AcquireTexture(const render::TextureDescriptor& desc, std::string_view name) {
    RADRAY_ASSERT(_cycle != 0);
    const TexturePoolKey key{desc};
    for (auto& texture : _textures) {
        if (!texture->InUse && texture->LastUsedCycle != _cycle && texture->Key == key) {
            texture->InUse = true;
            texture->LastUsedCycle = _cycle;
            ++_stats.Hits;
            return texture.get();
        }
    }
    if (!render::ValidateTextureDescriptor(desc, _device).Supported) return nullptr;
    ++_stats.Misses;
    auto native = _device.CreateTexture(desc);
    if (!native) return nullptr;
    auto texture = make_unique<PooledTexture>();
    texture->Id = _nextId++;
    texture->Key = key;
    texture->Texture = native.Release();
    texture->Texture->SetDebugName(name);
    const uint32_t layers = desc.Dim == render::TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize;
    texture->States.assign(size_t{layers} * desc.MipLevels, render::TextureState::Undefined);
    texture->InUse = true;
    texture->LastUsedCycle = _cycle;
    auto* result = texture.get();
    _textures.push_back(std::move(texture));
    ++_stats.Created;
    RefreshStats();
    return result;
}

Nullable<PooledBuffer*> RenderResourcePool::AcquireBuffer(const render::BufferDescriptor& desc, std::string_view name) {
    RADRAY_ASSERT(_cycle != 0);
    const BufferPoolKey key{desc};
    for (auto& buffer : _buffers) {
        if (!buffer->InUse && buffer->LastUsedCycle != _cycle && buffer->Key == key) {
            buffer->InUse = true;
            buffer->LastUsedCycle = _cycle;
            ++_stats.Hits;
            return buffer.get();
        }
    }
    if (desc.Size == 0 || desc.Size > _device.GetCapabilities().Limits.MaxBufferSize || !desc.Usage || !EnumContains(desc.Memory)) return nullptr;
    ++_stats.Misses;
    auto native = _device.CreateBuffer(desc);
    if (!native) return nullptr;
    auto buffer = make_unique<PooledBuffer>();
    buffer->Id = _nextId++;
    buffer->Key = key;
    buffer->Buffer = native.Release();
    buffer->Buffer->SetDebugName(name);
    buffer->State = InitialBufferState(desc);
    buffer->InUse = true;
    buffer->LastUsedCycle = _cycle;
    auto* result = buffer.get();
    _buffers.push_back(std::move(buffer));
    ++_stats.Created;
    RefreshStats();
    return result;
}

Nullable<render::TextureView*> RenderResourcePool::GetTextureView(PooledTexture& texture, TextureViewKey key) {
    RADRAY_ASSERT(texture.InUse);
    if (key.Format != texture.Key.Desc.Format) return nullptr;
    const auto range = render::NormalizeSubresourceRange(texture.Key.Desc, key.Range);
    if (!range) return nullptr;
    key.Range = *range;
    for (const auto& view : texture.Views)
        if (view.Key == key) return view.View.get();
    auto view = _device.CreateTextureView({texture.Texture.get(), key.Dimension, key.Format, key.Range, key.Usage});
    if (!view) return nullptr;
    auto* result = view.Get();
    texture.Views.push_back({key, view.Release()});
    ++_stats.ViewsCreated;
    ++_stats.ViewCount;
    return result;
}

void RenderResourcePool::EndGraph() noexcept {
    for (auto& texture : _textures) texture->InUse = false;
    for (auto& buffer : _buffers) buffer->InUse = false;
}

Nullable<render::TextureView*> RenderResourcePool::CreateExternalTextureView(const render::TextureViewDescriptor& desc) {
    auto view = _device.CreateTextureView(desc);
    if (!view) return nullptr;
    auto* result = view.Get();
    _externalViews.push_back(view.Release());
    return result;
}

void RenderResourcePool::RemoveViews(PooledTexture& texture) {
    for (const auto& view : texture.Views) _registry.RemoveFramebuffersUsing(view.View.get());
    texture.Views.clear();
}

void RenderResourcePool::Clear() {
    for (const auto& view : _externalViews) _registry.RemoveFramebuffersUsing(view.get());
    _externalViews.clear();
    for (auto& texture : _textures) RemoveViews(*texture);
    _textures.clear();
    _buffers.clear();
    RefreshStats();
}

void RenderResourcePool::RefreshStats() {
    _stats.TextureCount = static_cast<uint32_t>(_textures.size());
    _stats.BufferCount = static_cast<uint32_t>(_buffers.size());
    _stats.ViewCount = 0;
    _stats.EstimatedBytes = 0;
    for (const auto& texture : _textures) {
        _stats.ViewCount += static_cast<uint32_t>(texture->Views.size());
        _stats.EstimatedBytes += EstimateTextureBytes(texture->Key.Desc);
    }
    for (const auto& buffer : _buffers) _stats.EstimatedBytes += buffer->Key.Desc.Size;
}

}  // namespace radray
