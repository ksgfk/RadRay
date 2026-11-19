#include <radray/runtime/gpu_resource.h>

#include <radray/runtime/rdg.h>
#include <radray/gpu_impl/validation.h>

#include <radray/logger.h>

namespace radray {

Nullable<IntrusivePtr<GpuBuffer>> GpuBuffer::Create(gpu::Device& device, const GpuBufferDescriptor& desc) {
    const auto allowedUsage = gpu::BufferUse::Common | gpu::BufferUse::CopySource | gpu::BufferUse::CopyDestination | gpu::BufferUse::Index |
                              gpu::BufferUse::Vertex | gpu::BufferUse::CBuffer | gpu::BufferUse::Resource |
                              gpu::BufferUse::UnorderedAccess | gpu::BufferUse::Indirect;
    const auto maxSize = device.GetCapabilities().Limits.MaxBufferSize;
    if (!desc.Size || (maxSize && desc.Size > maxSize) || (desc.Usage.value() & ~allowedUsage.value())) {
        RADRAY_ERR_LOG("GpuBuffer: invalid size or Device buffer usage");
        return nullptr;
    }
    auto buffer = device.CreateBuffer({.Size = desc.Size, .Memory = gpu::MemoryType::Device, .Usage = desc.Usage | gpu::BufferUse::CopyDestination});
    if (!buffer) {
        RADRAY_ERR_LOG("GpuBuffer: cannot create buffer");
        return nullptr;
    }
    return IntrusivePtr<GpuBuffer>{new GpuBuffer{desc, buffer.Release()}};
}

GpuBuffer::GpuBuffer(const GpuBufferDescriptor& desc, unique_ptr<gpu::Buffer> buffer) noexcept
    : _desc(desc), _buffer(std::move(buffer)) {}

GpuBuffer::~GpuBuffer() noexcept = default;
gpu::Buffer* GpuBuffer::GetBuffer() const noexcept { return _buffer.get(); }
const GpuBufferDescriptor& GpuBuffer::GetDesc() const noexcept { return _desc; }

gpu::BufferStates GpuBuffer::GetSubmittedState() const noexcept { return _submittedState; }
void GpuBuffer::CommitSubmittedState(gpu::BufferStates state) noexcept { _submittedState = state; }
RDGBufferHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuBuffer& buffer) { return graph.ImportPersistentBorrowed(buffer); }
RDGBufferHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuBuffer>& buffer) { return graph.ImportPersistentOwned(buffer); }

Nullable<IntrusivePtr<GpuTexture>> GpuTexture::Create(gpu::Device& device, const GpuTextureDescriptor& desc) {
    auto resolved = desc;
    if (!resolved.Usage) {
        resolved.Usage = gpu::IsDepthStencilFormat(desc.Format) ? gpu::TextureUse::DepthStencilRead | gpu::TextureUse::DepthStencilWrite
                                                               : gpu::TextureUse::CopyDestination | gpu::TextureUse::Resource;
    }
    const gpu::TextureDescriptor native{.Dim = desc.Dim, .Width = desc.Width, .Height = desc.Height, .DepthOrArraySize = desc.ArrayLayers, .MipLevels = desc.MipLevels, .SampleCount = 1, .Format = desc.Format, .Memory = gpu::MemoryType::Device, .Usage = resolved.Usage};
    const auto [valid, diagnostic] = gpu::ValidateTextureDescriptor(native, device);
    if (!valid) {
        RADRAY_ERR_LOG("GpuTexture: {}", diagnostic);
        return nullptr;
    }
    auto texture = device.CreateTexture(native);
    if (!texture) {
        RADRAY_ERR_LOG("GpuTexture: cannot create texture");
        return nullptr;
    }
    unique_ptr<gpu::TextureView> view;
    if (resolved.Usage.HasFlag(gpu::TextureUse::Resource)) {
        auto range = gpu::ResolveSubresourceRange(native, gpu::SubresourceRange::AllSub());
        if (gpu::IsDepthStencilFormat(desc.Format)) range.Aspects = gpu::TextureAspect::Depth;
        view = device.CreateTextureView({texture.Get(), desc.Dim, desc.Format, range, gpu::TextureViewUsage::Resource}).Release();
        if (!view) {
            RADRAY_ERR_LOG("GpuTexture: cannot create sampling view");
            return nullptr;
        }
    }
    return IntrusivePtr<GpuTexture>{new GpuTexture{resolved, texture.Release(), std::move(view)}};
}

GpuTexture::GpuTexture(const GpuTextureDescriptor& desc, unique_ptr<gpu::Texture> texture, unique_ptr<gpu::TextureView> view) noexcept
    : _desc(desc), _texture(std::move(texture)), _defaultView(std::move(view)) {}

GpuTexture::~GpuTexture() noexcept = default;
gpu::Texture* GpuTexture::GetTexture() const noexcept { return _texture.get(); }
Nullable<gpu::TextureView*> GpuTexture::GetDefaultView() const noexcept { return _defaultView.get(); }
Nullable<gpu::TextureView*> GpuTexture::GetOrCreateView(gpu::Device& device, const GpuTextureViewKey& key) {
    const auto desc = _texture->GetDesc();
    auto canonical = key;
    if (canonical.Dim == gpu::TextureDimension::UNKNOWN) canonical.Dim = desc.Dim;
    if (canonical.Format == gpu::TextureFormat::UNKNOWN) canonical.Format = desc.Format;
    if (canonical.Usage == gpu::TextureViewUsage::Resource && !canonical.Range.Aspects && gpu::IsDepthStencilFormat(desc.Format))
        canonical.Range.Aspects = gpu::TextureAspect::Depth;
    const auto range = gpu::NormalizeSubresourceRange(desc, canonical.Range);
    if (!range) return nullptr;
    canonical.Range = *range;
    if (_defaultView && canonical.Usage == gpu::TextureViewUsage::Resource && canonical.Dim == desc.Dim && canonical.Format == desc.Format &&
        _defaultView->GetDesc().Range == canonical.Range) return _defaultView.get();
    for (const auto& [existing, view] : _viewCache) {
        if (existing == canonical) return view.get();
    }
    auto view = device.CreateTextureView({_texture.get(), canonical.Dim, canonical.Format, canonical.Range, canonical.Usage});
    if (!view) return nullptr;
    return _viewCache.emplace_back(canonical, view.Release()).second.get();
}
const GpuTextureDescriptor& GpuTexture::GetDesc() const noexcept { return _desc; }

gpu::TextureState GpuTexture::GetSubmittedState() const noexcept { return _submittedState; }
void GpuTexture::CommitSubmittedState(gpu::TextureState state) noexcept { _submittedState = state; }
RDGTextureHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuTexture& texture) { return graph.ImportPersistentBorrowed(texture); }
RDGTextureHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuTexture>& texture) { return graph.ImportPersistentOwned(texture); }

}  // namespace radray
