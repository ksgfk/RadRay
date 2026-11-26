#include <radray/runtime/gpu_resource.h>

#include <algorithm>
#include <limits>

#include <radray/runtime/rdg.h>
#include <radray/gpu_impl/validation.h>

#include <radray/logger.h>

namespace radray {

bool CanAllocateDeviceUpload(const gpu::Device& device, uint64_t size) noexcept {
    if (!device.GetCapabilities().Memory.HostVisibleDeviceLocalBytes) return false;
    const auto budget = device.QueryMemoryBudget().DeviceUpload;
    if (!budget.BudgetBytes) return true;
    const uint64_t limit = budget.BudgetBytes / 100 * 85 + budget.BudgetBytes % 100 * 85 / 100;
    return budget.UsageBytes <= limit && size <= limit - budget.UsageBytes;
}

Nullable<IntrusivePtr<GpuBuffer>> GpuBuffer::Create(gpu::Device& device, const GpuBufferDescriptor& desc) {
    const auto& limits = device.GetCapabilities().Limits;
    if (!desc.Size || (limits.MaxBufferSize && desc.Size > limits.MaxBufferSize) ||
        (desc.Usage & (gpu::BufferUse::MapRead | gpu::BufferUse::MapWrite))) return nullptr;
    const bool local = desc.Memory == GpuBufferMemory::DeviceLocal;
    gpu::BufferDescriptor native{.Size = desc.Size,
                                 .Memory = local ? gpu::MemoryType::DeviceLocal : gpu::MemoryType::HostUpload,
                                 .Usage = desc.Usage | (local ? gpu::BufferUse::CopyDestination : gpu::BufferUse::MapWrite)};
    if (!gpu::IsBufferMemoryUsageValid(native) || (!local && !desc.Usage)) return nullptr;
    switch (desc.Memory) {
        case GpuBufferMemory::DeviceLocal:
        case GpuBufferMemory::HostUpload: break;
        case GpuBufferMemory::DeviceUploadWithFallback:
        case GpuBufferMemory::DeviceUploadStrict:
            if (CanAllocateDeviceUpload(device, native.Size)) native.Memory = gpu::MemoryType::DeviceUpload;
            else if (desc.Memory == GpuBufferMemory::DeviceUploadStrict) return nullptr;
            break;
        default: return nullptr;
    }
    auto buffer = device.CreateBuffer(native);
    if (!buffer && native.Memory == gpu::MemoryType::DeviceUpload && desc.Memory == GpuBufferMemory::DeviceUploadWithFallback) {
        native.Memory = gpu::MemoryType::HostUpload;
        buffer = device.CreateBuffer(native);
    }
    if (!buffer) return nullptr;
    if (local) return IntrusivePtr<GpuBuffer>{new GpuBuffer{desc, buffer.Release(), nullptr, 1}};
    auto* mapped = static_cast<byte*>(buffer->GetMappedData().Get());
    if (!mapped) return nullptr;
    auto alignment = std::max<uint64_t>(1, limits.HostCoherentAtomSize);
    if (desc.Usage.HasFlag(gpu::BufferUse::CBuffer)) alignment = std::max(alignment, limits.CBufferOffsetAlignment);
    if (desc.Usage.HasFlag(gpu::BufferUse::Resource)) alignment = std::max({alignment, limits.StorageBufferOffsetAlignment, limits.TexelBufferOffsetAlignment});
    if (desc.Usage.HasFlag(gpu::BufferUse::CopySource)) alignment = std::max(alignment, limits.BufferCopyOffsetAlignment);
    if (desc.Usage.HasFlag(gpu::BufferUse::Indirect) || desc.Usage.HasFlag(gpu::BufferUse::Index)) alignment = std::max<uint64_t>(alignment, 4);
    return IntrusivePtr<GpuBuffer>{new GpuBuffer{desc, buffer.Release(), mapped, alignment}};
}

GpuBuffer::GpuBuffer(const GpuBufferDescriptor& desc, unique_ptr<gpu::Buffer> buffer, Nullable<byte*> mapped, uint64_t alignment) noexcept
    : _desc(desc), _buffer(std::move(buffer)), _mapped(mapped), _alignment(alignment),
      _submittedState(mapped ? gpu::BufferState::HostWrite : gpu::BufferState::Common) {}

GpuBuffer::~GpuBuffer() noexcept = default;
gpu::Buffer* GpuBuffer::GetBuffer() const noexcept { return _buffer.get(); }
const GpuBufferDescriptor& GpuBuffer::GetDesc() const noexcept { return _desc; }

gpu::BufferStates GpuBuffer::GetSubmittedState() const noexcept { return _submittedState; }
void GpuBuffer::CommitSubmittedState(gpu::BufferStates state) noexcept { _submittedState = state; }
RDGBufferHandle ImportPersistentBorrowed(RDGBuilder& graph, GpuBuffer& buffer) { return graph.ImportPersistentBorrowed(buffer); }
RDGBufferHandle ImportPersistentOwned(RDGBuilder& graph, const IntrusivePtr<GpuBuffer>& buffer) { return graph.ImportPersistentOwned(buffer); }

bool GpuBuffer::IsHostVisible() const noexcept { return bool(_mapped); }
gpu::BufferMemoryProperties GpuBuffer::GetMemoryProperties() const noexcept { return _buffer->GetMemoryProperties(); }
Nullable<byte*> GpuBuffer::GetMappedData() const noexcept { return _mapped; }
uint64_t GpuBuffer::GetSliceAlignment() const noexcept { return _alignment; }
uint64_t GpuBuffer::AlignSliceSize(uint64_t size) const noexcept {
    const auto remainder = size % _alignment;
    if (!remainder) return size;
    const auto padding = _alignment - remainder;
    return padding > std::numeric_limits<uint64_t>::max() - size ? 0 : size + padding;
}

bool GpuBuffer::FlushMappedRange(gpu::BufferRange range) noexcept {
    if (!_mapped || range.Offset > _desc.Size) return false;
    if (range.Size == gpu::BufferRange::All()) range.Size = _desc.Size - range.Offset;
    if (range.Size > _desc.Size - range.Offset) return false;
    if (!range.Size) return true;
    return _buffer->FlushMappedRange(range);
}

Nullable<IntrusivePtr<GpuTexture>> GpuTexture::Create(gpu::Device& device, const GpuTextureDescriptor& desc) {
    auto resolved = desc;
    if (!resolved.Usage) {
        resolved.Usage = gpu::IsDepthStencilFormat(desc.Format) ? gpu::TextureUse::DepthStencilRead | gpu::TextureUse::DepthStencilWrite
                                                               : gpu::TextureUse::CopyDestination | gpu::TextureUse::Resource;
    }
    const gpu::TextureDescriptor native{.Dim = desc.Dim, .Width = desc.Width, .Height = desc.Height, .DepthOrArraySize = desc.ArrayLayers, .MipLevels = desc.MipLevels, .SampleCount = 1, .Format = desc.Format, .Memory = gpu::MemoryType::DeviceLocal, .Usage = resolved.Usage};
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
    std::lock_guard lock{_viewMutex};
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
