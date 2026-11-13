#include <radray/runtime/gpu_resource.h>

#include <radray/logger.h>

namespace radray {

Nullable<IntrusivePtr<GpuBuffer>> GpuBuffer::Create(render::Device& device, const GpuBufferDescriptor& desc) {
    const auto allowedUsage = render::BufferUse::Common | render::BufferUse::CopySource | render::BufferUse::CopyDestination | render::BufferUse::Index |
                              render::BufferUse::Vertex | render::BufferUse::CBuffer | render::BufferUse::Resource |
                              render::BufferUse::UnorderedAccess | render::BufferUse::Indirect;
    const auto maxSize = device.GetCapabilities().Limits.MaxBufferSize;
    if (!desc.Size || (maxSize && desc.Size > maxSize) || (desc.Usage.value() & ~allowedUsage.value())) {
        RADRAY_ERR_LOG("GpuBuffer: invalid size or Device buffer usage");
        return nullptr;
    }
    auto buffer = device.CreateBuffer({.Size = desc.Size, .Memory = render::MemoryType::Device, .Usage = desc.Usage | render::BufferUse::CopyDestination});
    if (!buffer) {
        RADRAY_ERR_LOG("GpuBuffer: cannot create buffer");
        return nullptr;
    }
    return IntrusivePtr<GpuBuffer>{new GpuBuffer{desc, buffer.Release()}};
}

GpuBuffer::GpuBuffer(const GpuBufferDescriptor& desc, unique_ptr<render::Buffer> buffer) noexcept
    : _desc(desc), _buffer(std::move(buffer)) {}

GpuBuffer::~GpuBuffer() noexcept = default;
render::Buffer* GpuBuffer::GetBuffer() const noexcept { return _buffer.get(); }
const GpuBufferDescriptor& GpuBuffer::GetDesc() const noexcept { return _desc; }

render::BufferStates GpuBuffer::GetSubmittedState() const noexcept { return _submittedState; }
void GpuBuffer::CommitSubmittedState(render::BufferStates state) noexcept { _submittedState = state; }
RDGBufferHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuBuffer& buffer) { return graph.ImportPersistentBorrowed(buffer); }
RDGBufferHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuBuffer>& buffer) { return graph.ImportPersistentOwned(buffer); }

Nullable<IntrusivePtr<GpuTexture>> GpuTexture::Create(render::Device& device, const GpuTextureDescriptor& desc) {
    if ((desc.Dim != render::TextureDimension::Dim2D && desc.Dim != render::TextureDimension::Dim2DArray) ||
        render::GetTextureFormatBytesPerPixel(desc.Format) == 0 || render::IsDepthStencilFormat(desc.Format)) {
        RADRAY_ERR_LOG("GpuTexture: requires an uncompressed color 2D/2DArray format");
        return nullptr;
    }
    const render::TextureDescriptor native{.Dim = desc.Dim, .Width = desc.Width, .Height = desc.Height, .DepthOrArraySize = desc.ArrayLayers, .MipLevels = desc.MipLevels, .SampleCount = 1, .Format = desc.Format, .Memory = render::MemoryType::Device, .Usage = render::TextureUse::CopyDestination | render::TextureUse::Resource};
    const auto [valid, diagnostic] = render::ValidateTextureDescriptor(native, device);
    if (!valid) {
        RADRAY_ERR_LOG("GpuTexture: {}", diagnostic);
        return nullptr;
    }
    auto texture = device.CreateTexture(native);
    if (!texture) {
        RADRAY_ERR_LOG("GpuTexture: cannot create texture");
        return nullptr;
    }
    auto view = device.CreateTextureView({.Target = texture.Get(), .Dim = desc.Dim, .Format = desc.Format, .Range = {0, desc.ArrayLayers, 0, desc.MipLevels, render::TextureAspect::Color}, .Usage = render::TextureViewUsage::Resource});
    if (!view) {
        RADRAY_ERR_LOG("GpuTexture: cannot create sampling view");
        return nullptr;
    }
    return IntrusivePtr<GpuTexture>{new GpuTexture{desc, texture.Release(), view.Release()}};
}

GpuTexture::GpuTexture(const GpuTextureDescriptor& desc, unique_ptr<render::Texture> texture, unique_ptr<render::TextureView> view) noexcept
    : _desc(desc), _texture(std::move(texture)), _defaultView(std::move(view)) {}

GpuTexture::~GpuTexture() noexcept = default;
render::Texture* GpuTexture::GetTexture() const noexcept { return _texture.get(); }
render::TextureView* GpuTexture::GetDefaultView() const noexcept { return _defaultView.get(); }
const GpuTextureDescriptor& GpuTexture::GetDesc() const noexcept { return _desc; }

render::TextureState GpuTexture::GetSubmittedState() const noexcept { return _submittedState; }
void GpuTexture::CommitSubmittedState(render::TextureState state) noexcept { _submittedState = state; }
RDGTextureHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuTexture& texture) { return graph.ImportPersistentBorrowed(texture); }
RDGTextureHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuTexture>& texture) { return graph.ImportPersistentOwned(texture); }

}  // namespace radray
