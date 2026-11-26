#include <radray/gpu/types.h>

#include <cmath>
#include <limits>
#include <utility>

#include <magic_enum/magic_enum.hpp>

#include <radray/enum_flags.h>
#include <radray/logger.h>
#include <radray/utility.h>

namespace radray::gpu {

SwapChainFrame::SwapChainFrame(SwapChainFrame&& other) noexcept {
    swap(*this, other);
}

SwapChainFrame& SwapChainFrame::operator=(SwapChainFrame&& other) noexcept {
    SwapChainFrame tmp{std::move(other)};
    swap(*this, tmp);
    return *this;
}

Texture* SwapChainFrame::GetBackBuffer() const noexcept { return _backBuffer; }

uint32_t SwapChainFrame::GetBackBufferIndex() const noexcept { return _backBufferIndex; }

SwapChainSyncObject* SwapChainFrame::GetWaitToDraw() const noexcept { return _waitToDraw; }

SwapChainSyncObject* SwapChainFrame::GetReadyToPresent() const noexcept { return _readyToPresent; }

bool SwapChainFrame::IsValid() const noexcept { return _owner != nullptr; }

bool SwapChainFrame::IsFrom(const SwapChain* owner) const noexcept { return IsValid() && _owner == owner; }

GpuObjectTags PipelineCache::GetTag() const noexcept { return GpuObjectTag::PipelineCache; }

GpuObjectTags Device::GetTag() const noexcept { return GpuObjectTag::Device; }

Nullable<unique_ptr<PipelineCache>> Device::CreatePipelineCache(std::span<const byte>) noexcept { return nullptr; }

GpuObjectTags CommandQueue::GetTag() const noexcept { return GpuObjectTag::CmdQueue; }

uint32_t CommandQueue::GetTimestampValidBits() const noexcept { return 0; }

GpuObjectTags CommandAllocator::GetTag() const noexcept { return GpuObjectTag::CmdAllocator; }

GpuObjectTags CommandBuffer::GetTag() const noexcept { return GpuObjectTag::CmdBuffer; }

GpuObjectTags QueryPool::GetTag() const noexcept { return GpuObjectTag::QueryPool; }

GpuObjectTags CommandEncoder::GetTag() const noexcept { return GpuObjectTag::CmdEncoder; }

GpuObjectTags GraphicsCommandEncoder::GetTag() const noexcept { return GpuObjectTag::GraphicsCmdEncoder; }

GpuObjectTags ComputeCommandEncoder::GetTag() const noexcept { return GpuObjectTag::ComputeCmdEncoder; }

GpuObjectTags Fence::GetTag() const noexcept { return GpuObjectTag::Fence; }

GpuObjectTags SwapChainSyncObject::GetTag() const noexcept { return GpuObjectTag::UNKNOWN; }

GpuObjectTags SwapChain::GetTag() const noexcept { return GpuObjectTag::SwapChain; }

SwapChainFrame SwapChain::MakeFrame(
    SwapChain* owner,
    uint64_t token,
    Texture* backBuffer,
    uint32_t backBufferIndex,
    SwapChainSyncObject* waitToDraw,
    SwapChainSyncObject* readyToPresent) noexcept {
    SwapChainFrame f{};
    f._owner = owner;
    f._token = token;
    f._backBuffer = backBuffer;
    f._backBufferIndex = backBufferIndex;
    f._waitToDraw = waitToDraw;
    f._readyToPresent = readyToPresent;
    return f;
}

bool SwapChain::ValidateFrame(const SwapChainFrame& frame, const SwapChain* expectedOwner, uint64_t expectedToken) noexcept {
    return frame.IsValid() && frame._owner == expectedOwner && frame._token == expectedToken;
}

void SwapChain::InvalidateFrame(SwapChainFrame& frame) noexcept {
    frame = SwapChainFrame{};
}

GpuObjectTags Buffer::GetTag() const noexcept { return GpuObjectTag::Buffer; }

GpuObjectTags Texture::GetTag() const noexcept { return GpuObjectTag::Texture; }

GpuObjectTags TextureView::GetTag() const noexcept { return GpuObjectTag::TextureView; }

GpuObjectTags Shader::GetTag() const noexcept { return GpuObjectTag::Shader; }

GpuObjectTags BindingGroupLayout::GetTag() const noexcept { return GpuObjectTag::BindingGroupLayout; }

GpuObjectTags PipelineLayout::GetTag() const noexcept { return GpuObjectTag::PipelineLayout; }

GpuObjectTags ShaderParameterSet::GetTag() const noexcept { return GpuObjectTag::ShaderParameterSet; }

GpuObjectTags PipelineState::GetTag() const noexcept { return GpuObjectTag::PipelineState; }

GpuObjectTags GraphicsPipelineState::GetTag() const noexcept { return GpuObjectTag::GraphicsPipelineState; }

GpuObjectTags ComputePipelineState::GetTag() const noexcept { return GpuObjectTag::ComputePipelineState; }

GpuObjectTags Sampler::GetTag() const noexcept { return GpuObjectTag::Sampler; }

GpuObjectTags InstanceVulkan::GetTag() const noexcept { return GpuObjectTag::VkInstance; }

GpuObjectTags DXGIFactory::GetTag() const noexcept { return GpuObjectTag::DXGIFactory; }

AttachmentSignature GetAttachmentSignature(const GraphicsPipelineStateDescriptor& desc) {
    AttachmentSignature result{{}, {}, desc.MultiSample.Count};
    for (const auto& color : desc.ColorTargets) result.Colors.push_back(color.Format);
    if (desc.DepthStencil) result.DepthStencil = desc.DepthStencil->Format;
    return result;
}

SubresourceRange ResolveSubresourceRange(const TextureDescriptor& desc, SubresourceRange range) noexcept {
    if (!range.Aspects) range.Aspects = GetTextureFormatAspects(desc.Format);
    if (range.ArrayLayerCount == SubresourceRange::All) range.ArrayLayerCount = (desc.Dim == TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize) - range.BaseArrayLayer;
    if (range.MipLevelCount == SubresourceRange::All) range.MipLevelCount = desc.MipLevels - range.BaseMipLevel;
    return range;
}

std::optional<SubresourceRange> NormalizeSubresourceRange(const TextureDescriptor& desc, SubresourceRange range) noexcept {
    const auto aspects = GetTextureFormatAspects(desc.Format);
    if (!range.Aspects) range.Aspects = aspects;
    if ((range.Aspects.value() & ~aspects.value()) != 0) return std::nullopt;
    const uint32_t layers = desc.Dim == TextureDimension::Dim3D ? 1 : desc.DepthOrArraySize;
    if (range.BaseArrayLayer >= layers || range.BaseMipLevel >= desc.MipLevels) return std::nullopt;
    if (range.ArrayLayerCount == SubresourceRange::All) range.ArrayLayerCount = layers - range.BaseArrayLayer;
    if (range.MipLevelCount == SubresourceRange::All) range.MipLevelCount = desc.MipLevels - range.BaseMipLevel;
    if (range.ArrayLayerCount == 0 || range.ArrayLayerCount > layers - range.BaseArrayLayer ||
        range.MipLevelCount == 0 || range.MipLevelCount > desc.MipLevels - range.BaseMipLevel) return std::nullopt;
    return range;
}

bool AreWaitStagesSupported(QueueType queue, PipelineStages stages) noexcept {
    using enum PipelineStage;
    constexpr auto copy = TopOfPipe | Copy | BottomOfPipe | AllCommands;
    constexpr auto compute = copy | DrawIndirect | ComputeShader;
    constexpr auto direct = compute | VertexInput | VertexShader | PixelShader | DepthStencil | ColorAttachment | AllGraphics;
    switch (queue) {
        case QueueType::Direct: return stages && !(stages & ~direct);
        case QueueType::Compute: return stages && !(stages & ~compute);
        case QueueType::Copy: return stages && !(stages & ~copy);
        default: return false;
    }
}

bool IsValidShaderStages(ShaderStages stages) noexcept {
    return stages && !(stages & ~ShaderStages{ShaderStage::Graphics | ShaderStage::Compute});
}

bool IsValidSamplerDescriptor(const SamplerDescriptor& s) noexcept {
    return EnumContains(s.AddressS) && EnumContains(s.AddressT) && EnumContains(s.AddressR) &&
           EnumContains(s.MinFilter) && EnumContains(s.MagFilter) && EnumContains(s.MipmapFilter) &&
           EnumContains(s.BorderColor) && EnumContains(s.Reduction) && (!s.Compare || EnumContains(*s.Compare)) &&
           std::isfinite(s.LodMin) && std::isfinite(s.LodMax) && std::isfinite(s.MipLodBias) &&
           s.LodMin <= s.LodMax && s.AnisotropyClamp <= 16 &&
           (!s.Compare || s.Reduction == SamplerReductionMode::WeightedAverage) &&
           (!s.UnnormalizedCoordinates ||
            (s.MinFilter == s.MagFilter && s.MipmapFilter == FilterMode::Nearest && s.LodMin == 0 && s.LodMax == 0 &&
             s.AnisotropyClamp <= 1 && !s.Compare &&
             (s.AddressS == AddressMode::ClampToEdge || s.AddressS == AddressMode::ClampToBorder) &&
             (s.AddressT == AddressMode::ClampToEdge || s.AddressT == AddressMode::ClampToBorder)));
}

bool IsValidBindingMapping(const BindingDescriptor& b, GpuBackend backend) noexcept {
    if (!EnumContains(b.Kind) || !b.Count || !IsValidShaderStages(b.Stages) ||
        (b.ImmutableSampler && (b.Kind != BindingKind::Sampler || b.Count != 1))) return false;
    const bool singleRootBuffer = b.Count == 1 && CanBeRootDescriptor(b.Kind);
    if (backend == GpuBackend::D3D12 && b.D3D12) {
        const auto& d = *b.D3D12;
        return d.RegisterType == GetBindingRegisterClass(b.Kind) && EnumContains(d.Placement) &&
               b.Count - 1 <= std::numeric_limits<uint32_t>::max() - d.Register &&
               (d.Placement != D3D12BufferPlacement::RootDescriptor || singleRootBuffer);
    }
    if (backend == GpuBackend::Vulkan && b.Vulkan) {
        const auto& v = *b.Vulkan;
        return EnumContains(v.Placement) && v.Set != std::numeric_limits<uint32_t>::max() &&
               (v.Placement != VulkanBufferDescriptorPlacement::Dynamic || singleRootBuffer);
    }
    return false;
}

bool BindingMappingsOverlap(const BindingDescriptor& a, const BindingDescriptor& b, GpuBackend backend) noexcept {
    if (backend == GpuBackend::D3D12 && a.D3D12 && b.D3D12)
        return a.D3D12->Space == b.D3D12->Space && a.D3D12->RegisterType == b.D3D12->RegisterType &&
               uint64_t{a.D3D12->Register} < uint64_t{b.D3D12->Register} + b.Count &&
               uint64_t{b.D3D12->Register} < uint64_t{a.D3D12->Register} + a.Count;
    return backend == GpuBackend::Vulkan && a.Vulkan && b.Vulkan &&
           a.Vulkan->Set == b.Vulkan->Set && a.Vulkan->Binding == b.Vulkan->Binding;
}

bool IsValidPushConstantRange(const PushConstantDescriptor& p, GpuBackend backend) noexcept {
    if (!p.Size || p.Size % 4 || !IsValidShaderStages(p.Stages)) return false;
    if (backend == GpuBackend::D3D12) return p.D3D12.has_value();
    return backend == GpuBackend::Vulkan && p.Vulkan && p.Vulkan->Offset % 4 == 0 &&
           p.Size <= std::numeric_limits<uint32_t>::max() - p.Vulkan->Offset;
}

BindingRegisterClass GetBindingRegisterClass(BindingKind kind) noexcept {
    if (kind == BindingKind::CBuffer) return BindingRegisterClass::CBV;
    if (kind == BindingKind::Sampler) return BindingRegisterClass::Sampler;
    return IsWritableBinding(kind) ? BindingRegisterClass::UAV : BindingRegisterClass::SRV;
}

bool IsWritableBinding(BindingKind kind) noexcept {
    return kind == BindingKind::RWStructuredBuffer || kind == BindingKind::RWRawBuffer ||
           kind == BindingKind::RWTypedBuffer || kind == BindingKind::RWTexture;
}

bool CanBeRootDescriptor(BindingKind kind) noexcept {
    return kind == BindingKind::CBuffer || kind == BindingKind::StructuredBuffer ||
           kind == BindingKind::RWStructuredBuffer || kind == BindingKind::RawBuffer || kind == BindingKind::RWRawBuffer;
}

TextureStates TextureStateForAspect(TextureStates states, TextureAspect aspect) noexcept {
    const bool depthRead = states.HasFlag(TextureState::DepthReadStencilWrite);
    const bool stencilRead = states.HasFlag(TextureState::DepthWriteStencilRead);
    if (!depthRead && !stencilRead) return states;
    const bool read = aspect == TextureAspect::Depth ? depthRead : stencilRead;
    auto result = TextureStates{states.value() & ~(uint32_t(TextureState::DepthReadStencilWrite) | uint32_t(TextureState::DepthWriteStencilRead))};
    if (!read) result = TextureStates{result.value() & ~uint32_t(TextureState::ShaderRead)};
    result |= read ? TextureState::DepthRead : TextureState::DepthWrite;
    return result;
}

bool IsDepthStencilViewUsage(TextureViewUsage usage) noexcept {
    return usage == TextureViewUsage::DepthRead || usage == TextureViewUsage::DepthWrite ||
           usage == TextureViewUsage::DepthReadStencilWrite || usage == TextureViewUsage::DepthWriteStencilRead;
}

bool IsDepthReadOnly(TextureViewUsage usage) noexcept {
    return usage == TextureViewUsage::DepthRead || usage == TextureViewUsage::DepthReadStencilWrite;
}

bool IsStencilReadOnly(TextureViewUsage usage) noexcept {
    return usage == TextureViewUsage::DepthRead || usage == TextureViewUsage::DepthWriteStencilRead;
}

uint32_t GetVertexFormatSizeInBytes(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::SINT8X2:
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2: return 2;
        case VertexFormat::UINT8X4:
        case VertexFormat::SINT8X4:
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UINT16X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::UINT32:
        case VertexFormat::SINT32:
        case VertexFormat::FLOAT32: return 4;
        case VertexFormat::UINT16X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::UINT32X2:
        case VertexFormat::SINT32X2:
        case VertexFormat::FLOAT32X2: return 8;
        case VertexFormat::UINT32X3:
        case VertexFormat::SINT32X3:
        case VertexFormat::FLOAT32X3: return 12;
        case VertexFormat::UINT32X4:
        case VertexFormat::SINT32X4:
        case VertexFormat::FLOAT32X4: return 16;
        case VertexFormat::UNKNOWN: return 0;
    }
    Unreachable();
}

bool IsDepthStencilFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::D16_UNORM:
        case TextureFormat::D32_FLOAT:
        case TextureFormat::D24_UNORM_S8_UINT:
        case TextureFormat::D32_FLOAT_S8_UINT: return true;
        default: return false;
    }
}

TextureAspects GetTextureFormatAspects(TextureFormat format) noexcept {
    if (format == TextureFormat::D24_UNORM_S8_UINT || format == TextureFormat::D32_FLOAT_S8_UINT) return TextureAspect::Depth | TextureAspect::Stencil;
    return IsDepthStencilFormat(format) ? TextureAspects{TextureAspect::Depth} : TextureAspects{TextureAspect::Color};
}

bool IsUintFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::R8_UINT:
        case TextureFormat::R16_UINT:
        case TextureFormat::RG8_UINT:
        case TextureFormat::R32_UINT:
        case TextureFormat::RG16_UINT:
        case TextureFormat::RGBA8_UINT:
        case TextureFormat::RGB10A2_UINT:
        case TextureFormat::RG32_UINT:
        case TextureFormat::RGBA16_UINT:
        case TextureFormat::RGBA32_UINT: return true;
        default: return false;
    }
}

bool IsSintFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::R8_SINT:
        case TextureFormat::R16_SINT:
        case TextureFormat::RG8_SINT:
        case TextureFormat::R32_SINT:
        case TextureFormat::RG16_SINT:
        case TextureFormat::RGBA8_SINT:
        case TextureFormat::RG32_SINT:
        case TextureFormat::RGBA16_SINT:
        case TextureFormat::RGBA32_SINT: return true;
        default: return false;
    }
}

uint32_t GetIndexFormatSizeInBytes(IndexFormat format) noexcept {
    switch (format) {
        case IndexFormat::UINT16: return 2;
        case IndexFormat::UINT32: return 4;
    }
    Unreachable();
}

IndexFormat SizeInBytesToIndexFormat(uint32_t size) noexcept {
    switch (size) {
        case 2: return IndexFormat::UINT16;
        case 4: return IndexFormat::UINT32;
        default: return IndexFormat::UINT32;
    }
}

TextureFormatBlockInfo GetTextureFormatBlockInfo(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::BC1_UNORM:
        case TextureFormat::BC1_UNORM_SRGB:
        case TextureFormat::BC4_UNORM:
        case TextureFormat::BC4_SNORM: return {4, 4, 8};
        case TextureFormat::BC2_UNORM:
        case TextureFormat::BC2_UNORM_SRGB:
        case TextureFormat::BC3_UNORM:
        case TextureFormat::BC3_UNORM_SRGB:
        case TextureFormat::BC5_UNORM:
        case TextureFormat::BC5_SNORM:
        case TextureFormat::BC6H_UFLOAT:
        case TextureFormat::BC6H_SFLOAT:
        case TextureFormat::BC7_UNORM:
        case TextureFormat::BC7_UNORM_SRGB: return {4, 4, 16};
        case TextureFormat::R8_SINT:
        case TextureFormat::R8_UINT:
        case TextureFormat::R8_SNORM:
        case TextureFormat::R8_UNORM: return {1, 1, 1};
        case TextureFormat::R16_SINT:
        case TextureFormat::R16_UINT:
        case TextureFormat::R16_SNORM:
        case TextureFormat::R16_UNORM:
        case TextureFormat::R16_FLOAT:
        case TextureFormat::RG8_SINT:
        case TextureFormat::RG8_UINT:
        case TextureFormat::RG8_SNORM:
        case TextureFormat::RG8_UNORM:
        case TextureFormat::D16_UNORM: return {1, 1, 2};
        case TextureFormat::R32_SINT:
        case TextureFormat::R32_UINT:
        case TextureFormat::R32_FLOAT:
        case TextureFormat::RG16_SINT:
        case TextureFormat::RG16_UINT:
        case TextureFormat::RG16_SNORM:
        case TextureFormat::RG16_UNORM:
        case TextureFormat::RG16_FLOAT:
        case TextureFormat::RGBA8_SINT:
        case TextureFormat::RGBA8_UINT:
        case TextureFormat::RGBA8_SNORM:
        case TextureFormat::RGBA8_UNORM:
        case TextureFormat::RGBA8_UNORM_SRGB:
        case TextureFormat::BGRA8_UNORM:
        case TextureFormat::BGRA8_UNORM_SRGB:
        case TextureFormat::RGB10A2_UINT:
        case TextureFormat::RGB10A2_UNORM:
        case TextureFormat::RG11B10_FLOAT:
        case TextureFormat::D32_FLOAT:
        case TextureFormat::D24_UNORM_S8_UINT: return {1, 1, 4};
        case TextureFormat::RG32_SINT:
        case TextureFormat::RG32_UINT:
        case TextureFormat::RG32_FLOAT:
        case TextureFormat::RGBA16_SINT:
        case TextureFormat::RGBA16_UINT:
        case TextureFormat::RGBA16_SNORM:
        case TextureFormat::RGBA16_UNORM:
        case TextureFormat::RGBA16_FLOAT:
        case TextureFormat::D32_FLOAT_S8_UINT: return {1, 1, 8};
        case TextureFormat::RGBA32_SINT:
        case TextureFormat::RGBA32_UINT:
        case TextureFormat::RGBA32_FLOAT: return {1, 1, 16};
        case TextureFormat::UNKNOWN: return {1, 1, 0};
    }
    Unreachable();
}

bool IsBlockCompressedFormat(TextureFormat format) noexcept {
    return GetTextureFormatBlockInfo(format).BlockWidth > 1;
}

uint32_t GetTextureFormatBytesPerPixel(TextureFormat format) noexcept {
    const auto block = GetTextureFormatBlockInfo(format);
    return block.BlockWidth == 1 ? block.BytesPerBlock : 0;
}

std::string_view format_as(ShaderStage v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(ShaderBlobCategory v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(VertexFormat v) noexcept {
    switch (v) {
        case VertexFormat::UNKNOWN: return "UNKNOWN";
        case VertexFormat::UINT8X2: return "byte2";
        case VertexFormat::UINT8X4: return "byte4";
        case VertexFormat::SINT8X2: return "char2";
        case VertexFormat::SINT8X4: return "char4";
        case VertexFormat::UNORM8X2: return "unorm8x2";
        case VertexFormat::UNORM8X4: return "unorm8x4";
        case VertexFormat::SNORM8X2: return "snorm8x2";
        case VertexFormat::SNORM8X4: return "snorm8x4";
        case VertexFormat::UINT16X2: return "ushort2";
        case VertexFormat::UINT16X4: return "ushort4";
        case VertexFormat::SINT16X2: return "short2";
        case VertexFormat::SINT16X4: return "short4";
        case VertexFormat::UNORM16X2: return "unorm16x2";
        case VertexFormat::UNORM16X4: return "unorm16x4";
        case VertexFormat::SNORM16X2: return "snorm16x2";
        case VertexFormat::SNORM16X4: return "snorm16x4";
        case VertexFormat::FLOAT16X2: return "half2";
        case VertexFormat::FLOAT16X4: return "half4";
        case VertexFormat::UINT32: return "uint";
        case VertexFormat::UINT32X2: return "uint2";
        case VertexFormat::UINT32X3: return "uint3";
        case VertexFormat::UINT32X4: return "uint4";
        case VertexFormat::SINT32: return "int";
        case VertexFormat::SINT32X2: return "int2";
        case VertexFormat::SINT32X3: return "int3";
        case VertexFormat::SINT32X4: return "int4";
        case VertexFormat::FLOAT32: return "float";
        case VertexFormat::FLOAT32X2: return "float2";
        case VertexFormat::FLOAT32X3: return "float3";
        case VertexFormat::FLOAT32X4: return "float4";
    }
    Unreachable();
}

std::string_view format_as(GpuBackend v) noexcept {
    // MAX_COUNT 是哨兵而非真实后端, 不暴露其成员名。
    if (v == GpuBackend::MAX_COUNT) {
        return "UNKNOWN";
    }
    return EnumNameOr(v);
}

std::string_view format_as(TextureFormat v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(TextureAspect value) noexcept {
    return EnumNameOr(value);
}

std::string_view format_as(SampleCount value) noexcept {
    return EnumNameOr(value);
}

std::string_view format_as(QueueType v) noexcept {
    // MAX_COUNT 是哨兵而非真实队列类型, 不暴露其成员名。
    if (v == QueueType::MAX_COUNT) {
        return "UNKNOWN";
    }
    return EnumNameOr(v);
}

std::string_view format_as(PolygonMode v) noexcept {
    switch (v) {
        case PolygonMode::Fill: return "Fill";
        case PolygonMode::Line: return "Line";
        case PolygonMode::Point: return "Point";
    }
    Unreachable();
}

std::string_view format_as(TextureDimension v) noexcept {
    switch (v) {
        case TextureDimension::UNKNOWN: return "UNKNOWN";
        case TextureDimension::Dim1D: return "1D";
        case TextureDimension::Dim2D: return "2D";
        case TextureDimension::Dim3D: return "3D";
        case TextureDimension::Dim1DArray: return "1DArray";
        case TextureDimension::Dim2DArray: return "2DArray";
        case TextureDimension::Cube: return "Cube";
        case TextureDimension::CubeArray: return "CubeArray";
    }
    Unreachable();
}

std::string_view format_as(PipelineStage v) noexcept {
    return v == PipelineStage::NONE ? "NONE" : EnumFlagBitNameOr(v);
}

std::string_view format_as(BufferState v) noexcept {
    // 位标志枚举, 取值超出默认反射范围, 需按位反射。
    return EnumFlagBitNameOr(v);
}

std::string_view format_as(ResolveMode v) noexcept { return magic_enum::enum_name(v); }

std::string_view format_as(TextureState v) noexcept {
    return EnumFlagBitNameOr(v);
}

std::string_view format_as(TextureViewUsage v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(GpuObjectTag v) noexcept {
    switch (v) {
        case GpuObjectTag::UNKNOWN: return "UNKNOWN";
        case GpuObjectTag::Device: return "Device";
        case GpuObjectTag::CmdQueue: return "CmdQueue";
        case GpuObjectTag::CmdBuffer: return "CmdBuffer";
        case GpuObjectTag::CmdAllocator: return "CmdAllocator";
        case GpuObjectTag::CmdEncoder: return "CmdEncoder";
        case GpuObjectTag::GraphicsCmdEncoder: return "GraphicsCmdEncoder";
        case GpuObjectTag::ComputeCmdEncoder: return "ComputeCmdEncoder";
        case GpuObjectTag::Fence: return "Fence";
        case GpuObjectTag::Shader: return "Shader";
        case GpuObjectTag::BindingGroupLayout: return "BindingGroupLayout";
        case GpuObjectTag::ShaderParameterSet: return "ShaderParameterSet";
        case GpuObjectTag::QueryPool: return "QueryPool";
        case GpuObjectTag::PipelineLayout: return "PipelineLayout";
        case GpuObjectTag::PipelineState: return "PipelineState";
        case GpuObjectTag::GraphicsPipelineState: return "GraphicsPipelineState";
        case GpuObjectTag::ComputePipelineState: return "ComputePipelineState";
        case GpuObjectTag::SwapChain: return "SwapChain";
        case GpuObjectTag::Resource: return "Resource";
        case GpuObjectTag::Buffer: return "Buffer";
        case GpuObjectTag::Texture: return "Texture";
        case GpuObjectTag::RenderPass: return "RenderPass";
        case GpuObjectTag::Framebuffer: return "Framebuffer";
        case GpuObjectTag::ResourceView: return "ResourceView";
        case GpuObjectTag::TextureView: return "TextureView";
        case GpuObjectTag::Sampler: return "Sampler";
        case GpuObjectTag::VkInstance: return "VkInstance";
        case GpuObjectTag::DXGIFactory: return "DXGIFactory";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(PresentMode v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(FenceStatus v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(SwapChainStatus v) noexcept {
    return EnumNameOr(v);
}

}  // namespace radray::gpu
