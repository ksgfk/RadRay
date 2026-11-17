#include <radray/runtime/gpu_resource.h>
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
    if ((desc.Dim != gpu::TextureDimension::Dim2D && desc.Dim != gpu::TextureDimension::Dim2DArray) ||
        gpu::GetTextureFormatBytesPerPixel(desc.Format) == 0 || gpu::IsDepthStencilFormat(desc.Format)) {
        RADRAY_ERR_LOG("GpuTexture: requires an uncompressed color 2D/2DArray format");
        return nullptr;
    }
    const gpu::TextureDescriptor native{.Dim = desc.Dim, .Width = desc.Width, .Height = desc.Height, .DepthOrArraySize = desc.ArrayLayers, .MipLevels = desc.MipLevels, .SampleCount = 1, .Format = desc.Format, .Memory = gpu::MemoryType::Device, .Usage = gpu::TextureUse::CopyDestination | gpu::TextureUse::Resource};
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
    auto view = device.CreateTextureView({.Target = texture.Get(), .Dim = desc.Dim, .Format = desc.Format, .Range = {0, desc.ArrayLayers, 0, desc.MipLevels, gpu::TextureAspect::Color}, .Usage = gpu::TextureViewUsage::Resource});
    if (!view) {
        RADRAY_ERR_LOG("GpuTexture: cannot create sampling view");
        return nullptr;
    }
    return IntrusivePtr<GpuTexture>{new GpuTexture{desc, texture.Release(), view.Release()}};
}

GpuTexture::GpuTexture(const GpuTextureDescriptor& desc, unique_ptr<gpu::Texture> texture, unique_ptr<gpu::TextureView> view) noexcept
    : _desc(desc), _texture(std::move(texture)), _defaultView(std::move(view)) {}

GpuTexture::~GpuTexture() noexcept = default;
gpu::Texture* GpuTexture::GetTexture() const noexcept { return _texture.get(); }
gpu::TextureView* GpuTexture::GetDefaultView() const noexcept { return _defaultView.get(); }
const GpuTextureDescriptor& GpuTexture::GetDesc() const noexcept { return _desc; }

gpu::TextureState GpuTexture::GetSubmittedState() const noexcept { return _submittedState; }
void GpuTexture::CommitSubmittedState(gpu::TextureState state) noexcept { _submittedState = state; }
RDGTextureHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuTexture& texture) { return graph.ImportPersistentBorrowed(texture); }
RDGTextureHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuTexture>& texture) { return graph.ImportPersistentOwned(texture); }

}  // namespace radray
