#include <radray/gpu_impl/validation.h>

#include <algorithm>
#include <bit>
#include <limits>

#include <radray/logger.h>

namespace radray::gpu {

std::optional<AttachmentSignature> ValidateRenderScope(const RenderPassBeginDescriptor& desc, Device& device) noexcept {
    if ((!desc.Colors.size() && !desc.DepthStencil) || !desc.Width || !desc.Height || !desc.Layers ||
        desc.Colors.size() > device.GetCapabilities().Limits.MaxColorAttachments) return std::nullopt;
    const auto area = desc.RenderArea.value_or(Rect{0, 0, desc.Width, desc.Height});
    if (area.X < 0 || area.Y < 0 || !area.Width || !area.Height || uint64_t(area.X) + area.Width > desc.Width || uint64_t(area.Y) + area.Height > desc.Height) return std::nullopt;
    const auto validLoad = [](LoadAction value) { return value == LoadAction::DontCare || value == LoadAction::Load || value == LoadAction::Clear; };
    const auto validStore = [](StoreAction value) { return value == StoreAction::Store || value == StoreAction::Discard; };
    AttachmentSignature signature;
    uint32_t samples = 0;
    size_t validatedColors = 0;
    const auto validate = [&](TextureView* view, TextureViewUsage usage, bool depth) {
        if (!view || !view->IsValid()) return false;
        const auto v = view->GetDesc();
        if (!v.Target || !v.Target->IsValid() || v.Usage != usage || IsDepthStencilFormat(v.Format) != depth || v.Format == TextureFormat::UNKNOWN) return false;
        const auto t = v.Target->GetDesc();
        if (v.Range.BaseMipLevel >= 32 || v.Range.BaseMipLevel >= t.MipLevels) return false;
        const auto layers = t.Dim == TextureDimension::Dim3D ? std::max(1u, t.DepthOrArraySize >> v.Range.BaseMipLevel) : v.Range.ArrayLayerCount;
        if (v.Range.MipLevelCount != 1 || layers < desc.Layers ||
            v.Range.BaseArrayLayer >= t.DepthOrArraySize || v.Range.ArrayLayerCount > t.DepthOrArraySize - v.Range.BaseArrayLayer ||
            std::max(1u, t.Width >> v.Range.BaseMipLevel) < desc.Width || std::max(1u, t.Height >> v.Range.BaseMipLevel) < desc.Height || !t.SampleCount) return false;
        if ((t.SampleCount & (t.SampleCount - 1)) || t.SampleCount > 64 || (samples && samples != t.SampleCount)) return false;
        samples = t.SampleCount;
        for (size_t index = 0; index < validatedColors; ++index) {
            const auto old = desc.Colors[index].View->GetDesc();
            if (old.Target == v.Target && old.Range.BaseMipLevel == v.Range.BaseMipLevel &&
                old.Range.BaseArrayLayer < v.Range.BaseArrayLayer + v.Range.ArrayLayerCount && v.Range.BaseArrayLayer < old.Range.BaseArrayLayer + old.Range.ArrayLayerCount) return false;
        }
        return true;
    };
    for (const auto& color : desc.Colors) {
        if (!validLoad(color.Load) || !validStore(color.Store)) return std::nullopt;
        if (!validate(color.View, TextureViewUsage::RenderTarget, false)) return std::nullopt;
        signature.Colors.push_back(color.View->GetDesc().Format);
        ++validatedColors;
    }
    if (desc.DepthStencil) {
        const auto& d = *desc.DepthStencil;
        if (!validLoad(d.DepthLoad) || !validStore(d.DepthStore)) return std::nullopt;
        if (!d.View || !IsDepthStencilViewUsage(d.View->GetDesc().Usage) || !validate(d.View, d.View->GetDesc().Usage, true)) return std::nullopt;
        const auto format = d.View->GetDesc().Format;
        const bool stencil = format == TextureFormat::D24_UNORM_S8_UINT || format == TextureFormat::D32_FLOAT_S8_UINT;
        if (stencil && (!validLoad(d.StencilLoad) || !validStore(d.StencilStore))) return std::nullopt;
        if ((IsDepthReadOnly(d.View->GetDesc().Usage) && (d.DepthLoad != LoadAction::Load || d.DepthStore != StoreAction::Store)) ||
            (stencil && IsStencilReadOnly(d.View->GetDesc().Usage) && (d.StencilLoad != LoadAction::Load || d.StencilStore != StoreAction::Store))) return std::nullopt;
        signature.DepthStencil = format;
    }
    vector<TextureView*> attachments;
    for (const auto& color : desc.Colors) attachments.push_back(color.View);
    if (desc.DepthStencil) attachments.push_back(desc.DepthStencil->View);
    const auto validateResolve = [&](TextureView* source, TextureView* target, ResolveMode depthMode, ResolveMode stencilMode) {
        if (!target || !target->IsValid()) return false;
        const auto src = source->GetDesc(), dst = target->GetDesc();
        if (!dst.Target || !dst.Target->IsValid() || dst.Format != src.Format) return false;
        const auto st = src.Target->GetDesc(), dt = dst.Target->GetDesc();
        const bool depth = IsDepthStencilFormat(src.Format);
        if (dst.Usage != (depth ? TextureViewUsage::DepthWrite : TextureViewUsage::RenderTarget) ||
            st.SampleCount <= 1 || dt.SampleCount != 1 || dst.Range.MipLevelCount != 1 || dst.Range.BaseMipLevel >= dt.MipLevels || dst.Range.BaseMipLevel >= 32 ||
            dst.Range.ArrayLayerCount != src.Range.ArrayLayerCount || dst.Range.BaseArrayLayer >= dt.DepthOrArraySize || dst.Range.ArrayLayerCount > dt.DepthOrArraySize - dst.Range.BaseArrayLayer ||
            std::max(1u, st.Width >> src.Range.BaseMipLevel) != std::max(1u, dt.Width >> dst.Range.BaseMipLevel) ||
            std::max(1u, st.Height >> src.Range.BaseMipLevel) != std::max(1u, dt.Height >> dst.Range.BaseMipLevel) ||
            ResolveSubresourceRange(dt, dst.Range).Aspects != GetTextureFormatAspects(dst.Format)) return false;
        const auto support = device.QueryAttachmentResolveSupport(src.Format, st.SampleCount);
        if (!depth) {
            if (!support.ColorAverage) return false;
        } else {
            const auto validMode = [](ResolveMode mode, ResolveModes modes) {
                const auto bits = uint32_t(mode);
                return mode == ResolveMode::None || (!(bits & (bits - 1)) && modes.HasFlag(mode));
            };
            if (!validMode(depthMode, support.DepthModes) || !validMode(stencilMode, support.StencilModes) ||
                (depthMode == ResolveMode::None && stencilMode == ResolveMode::None)) return false;
            if (GetTextureFormatAspects(src.Format).HasFlag(TextureAspect::Stencil) && depthMode != stencilMode) {
                if ((depthMode == ResolveMode::None || stencilMode == ResolveMode::None) ? !support.IndependentResolveNone : !support.IndependentResolve) return false;
            }
        }
        for (const auto* oldView : attachments) {
            const auto old = oldView->GetDesc();
            if (old.Target == dst.Target && old.Range.BaseMipLevel == dst.Range.BaseMipLevel &&
                old.Range.BaseArrayLayer < dst.Range.BaseArrayLayer + dst.Range.ArrayLayerCount && dst.Range.BaseArrayLayer < old.Range.BaseArrayLayer + old.Range.ArrayLayerCount) return false;
        }
        attachments.push_back(target);
        return true;
    };
    for (const auto& color : desc.Colors)
        if (color.ResolveTarget && !validateResolve(color.View, color.ResolveTarget.Get(), ResolveMode::None, ResolveMode::None)) return std::nullopt;
    if (desc.DepthStencil && desc.DepthStencil->Resolve) {
        const auto& resolve = *desc.DepthStencil->Resolve;
        if (!validateResolve(desc.DepthStencil->View, resolve.Target, resolve.DepthMode, resolve.StencilMode)) return std::nullopt;
    }
    signature.Samples = samples;
    return signature;
}

bool ValidateVertexInputState(const VertexInputState& state) noexcept {
    for (size_t index = 0; index < state.Buffers.size(); ++index) {
        const VertexBufferLayout& buffer = state.Buffers[index];
        if (buffer.ArrayStride == 0) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            if (state.Buffers[previous].Binding == buffer.Binding) {
                return false;
            }
        }
    }

    for (size_t index = 0; index < state.Attributes.size(); ++index) {
        const VertexAttribute& attribute = state.Attributes[index];
        if (attribute.Semantic.empty()) {
            return false;
        }
        const uint32_t formatSize = GetVertexFormatSizeInBytes(attribute.Format);
        if (formatSize == 0) {
            return false;
        }
        const auto buffer = std::find_if(
            state.Buffers.begin(),
            state.Buffers.end(),
            [&](const VertexBufferLayout& value) noexcept {
                return value.Binding == attribute.BufferBinding;
            });
        if (buffer == state.Buffers.end() ||
            attribute.Offset > buffer->ArrayStride ||
            formatSize > buffer->ArrayStride - attribute.Offset) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const VertexAttribute& old = state.Attributes[previous];
            if (old.Location == attribute.Location ||
                (old.Semantic == attribute.Semantic && old.SemanticIndex == attribute.SemanticIndex)) {
                return false;
            }
        }
    }
    return true;
}

namespace {
std::pair<bool, std::string_view> ValidateTextureRegion(const TextureDescriptor& texture, const TextureRegion2D& target, TextureUse usage) {
    const auto block = GetTextureFormatBlockInfo(texture.Format);
    if (texture.Dim == TextureDimension::UNKNOWN || texture.SampleCount != 1 || block.BytesPerBlock == 0 ||
        IsDepthStencilFormat(texture.Format) ||
        !texture.Usage.HasFlag(usage))
        return {false, "Texture transfer requires a color texture, single sample and copy usages"};
    if (target.MipLevel >= texture.MipLevels || target.MipLevel >= 32 || !target.Width || !target.Height || !target.Depth)
        return {false, "Texture transfer subresource or extent is invalid"};
    const auto width = std::max(1u, texture.Width >> target.MipLevel);
    const auto height = std::max(1u, texture.Height >> target.MipLevel);
    const bool volume = texture.Dim == TextureDimension::Dim3D;
    const auto depth = volume ? std::max(1u, texture.DepthOrArraySize >> target.MipLevel) : 1u;
    if (target.ArrayLayer >= (volume ? 1u : texture.DepthOrArraySize) || target.Z > depth || target.Depth > depth - target.Z)
        return {false, "Texture transfer exceeds the texture layers or slices"};
    if (target.X > width || target.Width > width - target.X || target.Y > height || target.Height > height - target.Y)
        return {false, "Texture transfer exceeds the texture mip"};
    if (target.X % block.BlockWidth || target.Y % block.BlockHeight ||
        (target.Width % block.BlockWidth && target.Width != width - target.X) ||
        (target.Height % block.BlockHeight && target.Height != height - target.Y))
        return {false, "Partial format blocks are only valid at the mip edge"};
    return {true, {}};
}

std::pair<bool, std::string_view> ValidateTransferLayout(
    const TextureDescriptor& texture, const TextureRegion2D& target, uint64_t capacity,
    uint64_t offset, uint32_t rowPitch, uint32_t rowsPerImage, const DeviceDetail& detail) {
    const auto block = GetTextureFormatBlockInfo(texture.Format);
    const uint64_t rowBytes = uint64_t{block.Columns(target.Width)} * block.BytesPerBlock;
    if (offset % std::max(uint64_t{1}, detail.TextureDataPlacementAlignment) != 0 ||
        offset % block.BytesPerBlock != 0 || rowPitch < rowBytes || rowPitch % block.BytesPerBlock != 0 ||
        rowPitch % std::max(1u, detail.TextureDataPitchAlignment) != 0)
        return {false, "Texture transfer buffer offset or row pitch is not aligned"};
    const uint32_t rows = block.Rows(target.Height);
    const uint32_t imageRows = rowsPerImage ? rowsPerImage : rows;
    if (imageRows < rows || imageRows > std::numeric_limits<uint32_t>::max() / block.BlockHeight ||
        rowPitch / block.BytesPerBlock > std::numeric_limits<uint32_t>::max() / block.BlockWidth)
        return {false, "Texture transfer strides cannot represent this copy extent"};
    const uint64_t lastRow = uint64_t{target.Depth - 1} * imageRows + rows - 1;
    if (offset > capacity || rowBytes > capacity - offset ||
        lastRow > (capacity - offset - rowBytes) / rowPitch)
        return {false, "Texture transfer exceeds the buffer"};
    return {true, {}};
}
}  // namespace

std::pair<bool, std::string_view> ValidateTextureUploadRegion(const TextureDescriptor& destination, const TextureRegion2D& target) {
    return ValidateTextureRegion(destination, target, TextureUse::CopyDestination);
}

std::pair<bool, std::string_view> ValidateBufferTextureCopyRegion(
    const BufferDescriptor& source, const TextureDescriptor& destination,
    const BufferTextureCopyRegion& r, const DeviceDetail& detail) {
    if (!source.Usage.HasFlag(BufferUse::CopySource)) return {false, "Region upload requires a copy source buffer"};
    auto status = ValidateTextureUploadRegion(destination, r.Destination);
    if (!status.first) return status;
    return ValidateTransferLayout(destination, r.Destination, source.Size, r.SourceOffset, r.RowPitch, r.RowsPerImage, detail);
}

std::pair<bool, std::string_view> ValidateTextureReadbackRegion(const TextureDescriptor& source, const TextureRegion2D& region) {
    return ValidateTextureRegion(source, region, TextureUse::CopySource);
}

std::pair<bool, std::string_view> ValidateTextureReadbackRegion(
    const TextureDescriptor& source, const BufferDescriptor& destination,
    const TextureBufferCopyRegion& r, const DeviceDetail& detail) {
    if (!destination.Usage.HasFlag(BufferUse::CopyDestination)) return {false, "Region readback requires a copy destination buffer"};
    auto status = ValidateTextureReadbackRegion(source, r.Source);
    if (!status.first) return status;
    return ValidateTransferLayout(source, r.Source, destination.Size, r.DestinationOffset, r.RowPitch, r.RowsPerImage, detail);
}

bool IsValidTextureSupportQuery(const TextureSupportQuery& query) noexcept {
    if (!EnumContains(query.Dimension) || query.Dimension == TextureDimension::UNKNOWN ||
        !EnumContains(query.Format) || query.Format == TextureFormat::UNKNOWN ||
        !query.Usage || (query.Usage.value() & ~uint32_t{127}) != 0) {
        return false;
    }
    const bool depthUsage = bool(query.Usage & (TextureUse::DepthStencilRead | TextureUse::DepthStencilWrite));
    const bool depthFormat = IsDepthStencilFormat(query.Format);
    if (depthUsage && !depthFormat) return false;
    if (IsBlockCompressedFormat(query.Format) && (depthUsage || (query.Usage & (TextureUse::RenderTarget | TextureUse::UnorderedAccess)))) return false;
    if (depthFormat && (query.Usage & (TextureUse::RenderTarget | TextureUse::UnorderedAccess))) return false;
    if (depthUsage && query.Dimension == TextureDimension::Dim3D) return false;
    return true;
}

bool BufferRangeFits(uint64_t bufferSize, BufferRange range) noexcept {
    return range.Offset < bufferSize && range.Size != 0 &&
           (range.Size == BufferRange::All() || range.Size <= bufferSize - range.Offset);
}

bool IsBufferMemoryUsageValid(const BufferDescriptor& desc) noexcept {
    const auto mapped = desc.Usage & (BufferUse::MapRead | BufferUse::MapWrite);
    if (desc.Hints & ~ResourceHints{ResourceHint::Dedicated}) return false;
    switch (desc.Memory) {
        case MemoryType::Device: return !mapped;
        case MemoryType::Upload: return mapped == BufferUse::MapWrite;
        case MemoryType::ReadBack: return mapped == BufferUse::MapRead;
    }
    return false;
}

std::pair<bool, string> ValidateTextureDescriptor(
    const TextureDescriptor& desc, const GpuDeviceCapabilities& capabilities, const TextureSupport& support) {
    if (!IsValidTextureSupportQuery({desc.Dim, desc.Format, desc.Usage})) {
        return {false, "Dimension, Format or Usage is invalid or incompatible"};
    }
    if (desc.Memory != MemoryType::Device || (desc.Hints & ~ResourceHints{ResourceHint::Dedicated})) {
        return {false, "Texture Memory must be Device; only the Dedicated allocation hint is supported"};
    }
    if (desc.Width == 0 || desc.Height == 0 || desc.DepthOrArraySize == 0 || desc.MipLevels == 0) {
        return {false, "Width, Height, DepthOrArraySize and MipLevels must be nonzero"};
    }
    if (!EnumContains(static_cast<SampleCount>(desc.SampleCount))) {
        return {false, "SampleCount must be 1, 2, 4, 8 or 16"};
    }
    const bool is1D = desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim1DArray;
    const bool is3D = desc.Dim == TextureDimension::Dim3D;
    const bool isCube = desc.Dim == TextureDimension::Cube || desc.Dim == TextureDimension::CubeArray;
    if ((is1D && desc.Height != 1) ||
        ((desc.Dim == TextureDimension::Dim1D || desc.Dim == TextureDimension::Dim2D) && desc.DepthOrArraySize != 1) ||
        (isCube && (desc.Width != desc.Height || desc.DepthOrArraySize % 6 != 0)) ||
        (desc.Dim == TextureDimension::Cube && desc.DepthOrArraySize != 6)) {
        return {false, "Extent or array layer count is incompatible with Dimension"};
    }
    if (desc.SampleCount > 1 &&
        (desc.MipLevels != 1 || (desc.Dim != TextureDimension::Dim2D && desc.Dim != TextureDimension::Dim2DArray) ||
         desc.Usage.HasFlag(TextureUse::UnorderedAccess))) {
        return {false, "MSAA requires 2D/2DArray, MipLevels=1 and no UnorderedAccess usage"};
    }
    const auto& limits = capabilities.Limits;
    const uint32_t dimensionLimit = is1D ? limits.MaxTexture1DDimension : is3D ? limits.MaxTexture3DDimension
                                                                               : limits.MaxTexture2DDimension;
    if (desc.Width > dimensionLimit || desc.Height > dimensionLimit ||
        desc.DepthOrArraySize > (is3D ? limits.MaxTexture3DDimension : limits.MaxTextureArrayLayers)) {
        return {false, "Extent or array layer count exceeds DeviceLimits"};
    }
    const uint32_t maxDimension = std::max({desc.Width, desc.Height, is3D ? desc.DepthOrArraySize : 1u});
    if (desc.MipLevels > static_cast<uint32_t>(std::bit_width(maxDimension))) {
        return {false, "MipLevels exceeds the complete mip chain for this extent"};
    }
    if (!support.Supported || !support.SampleCounts.HasFlag(static_cast<SampleCount>(desc.SampleCount))) {
        return {false, "Dimension/Format/Usage/SampleCount is not supported by the device"};
    }
    if (desc.Width > support.MaxWidth || desc.Height > support.MaxHeight ||
        (is3D ? desc.DepthOrArraySize > support.MaxDepth : desc.DepthOrArraySize > support.MaxArrayLayers) ||
        desc.MipLevels > support.MaxMipLevels) {
        return {false, "Extent, layers or mip count exceeds the format/usage support limits"};
    }
    uint64_t bytes = 0;
    const auto block = GetTextureFormatBlockInfo(desc.Format);
    for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
        uint64_t mipBytes = block.BytesPerBlock;
        const uint32_t factors[]{block.Columns(std::max(1u, desc.Width >> mip)), block.Rows(std::max(1u, desc.Height >> mip)),
                                 is3D ? std::max(1u, desc.DepthOrArraySize >> mip) : desc.DepthOrArraySize, desc.SampleCount};
        for (const auto factor : factors) {
            if (mipBytes > std::numeric_limits<uint64_t>::max() / factor) return {false, "Texture size overflows uint64_t"};
            mipBytes *= factor;
        }
        if (mipBytes > support.MaxResourceSize || bytes > support.MaxResourceSize - mipBytes) {
            return {false, "Texture size exceeds format/usage MaxResourceSize"};
        }
        bytes += mipBytes;
    }
    return {true, {}};
}

std::pair<bool, string> ValidateTextureDescriptor(const TextureDescriptor& desc, const Device& device) {
    const TextureSupportQuery query{desc.Dim, desc.Format, desc.Usage};
    return ValidateTextureDescriptor(desc, device.GetCapabilities(), device.QueryTextureSupport(query));
}

}  // namespace radray::gpu
