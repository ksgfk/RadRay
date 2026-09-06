#include <radray/render/rhi.h>

#include <radray/logger.h>
#include <radray/utility.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

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

TextureDescriptorValidationResult ValidateBufferTextureCopyRegion(
    const BufferDescriptor& source, const TextureDescriptor& destination,
    const BufferTextureCopyRegion& r, const DeviceDetail& detail) {
    const auto bpp = GetTextureFormatBytesPerPixel(destination.Format);
    if (destination.Dim != TextureDimension::Dim2D || destination.SampleCount != 1 || bpp == 0 ||
        IsDepthStencilFormat(destination.Format) || !source.Usage.HasFlag(BufferUse::CopySource) ||
        !destination.Usage.HasFlag(TextureUse::CopyDestination))
        return {false, "Region upload requires an uncompressed color 2D texture, single sample and copy usages"};
    if (r.MipLevel >= destination.MipLevels || r.MipLevel >= 32 || r.ArrayLayer >= destination.DepthOrArraySize || r.Width == 0 || r.Height == 0)
        return {false, "Region upload subresource or extent is invalid"};
    const auto width = std::max(1u, destination.Width >> r.MipLevel);
    const auto height = std::max(1u, destination.Height >> r.MipLevel);
    if (r.X > width || r.Width > width - r.X || r.Y > height || r.Height > height - r.Y)
        return {false, "Region upload exceeds the destination mip"};
    const uint64_t rowBytes = uint64_t{r.Width} * bpp;
    if (r.SourceOffset % std::max(uint64_t{1}, detail.TextureDataPlacementAlignment) != 0 ||
        r.SourceOffset % bpp != 0 || r.RowPitch < rowBytes || r.RowPitch % bpp != 0 ||
        r.RowPitch % std::max(1u, detail.TextureDataPitchAlignment) != 0)
        return {false, "Region upload source offset or row pitch is not aligned"};
    const uint64_t size = uint64_t{r.RowPitch} * (r.Height - 1) + rowBytes;
    if (r.SourceOffset > source.Size || size > source.Size - r.SourceOffset)
        return {false, "Region upload exceeds the source buffer"};
    return {true, {}};
}

}  // namespace radray::render

namespace radray::render {

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

Nullable<shared_ptr<Device>> Device::Create(const DeviceDescriptor& desc) {
    return std::visit(
        [](auto&& arg) -> Nullable<shared_ptr<Device>> {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, D3D12DeviceDescriptor>) {
#ifdef RADRAY_ENABLE_D3D12
                return d3d12::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("D3D12 disable");
                return nullptr;
#endif
            } else if constexpr (std::is_same_v<T, VulkanDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_VULKAN
                return vulkan::CreateDeviceVulkan(arg);
#else
                RADRAY_ERR_LOG("Vulkan disable");
                return nullptr;
#endif
            }
        },
        desc);
}

Nullable<InstanceVulkan*> InstanceVulkan::InitEnv(const VulkanInstanceDescriptor& desc) {
#ifdef RADRAY_ENABLE_VULKAN
    return vulkan::InitVulkanEnvImpl(desc);
#else
    RADRAY_UNUSED(desc);
    RADRAY_ERR_LOG("Vulkan disable");
    return nullptr;
#endif
}

void InstanceVulkan::ShutdownEnv() noexcept {
#ifdef RADRAY_ENABLE_VULKAN
    return vulkan::ShutdownVulkanEnvImpl();
#else
    RADRAY_ERR_LOG("Vulkan disable");
#endif
}

Nullable<unique_ptr<DXGIFactory>> DXGIFactory::Create(const DXGIFactoryDescriptor& desc) {
#ifdef RADRAY_ENABLE_D3D12
    return d3d12::CreateDXGIFactory(desc);
#else
    RADRAY_UNUSED(desc);
    RADRAY_ERR_LOG("D3D12 disable");
    return nullptr;
#endif
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

uint32_t GetTextureFormatBytesPerPixel(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::R8_SINT:
        case TextureFormat::R8_UINT:
        case TextureFormat::R8_SNORM:
        case TextureFormat::R8_UNORM: return 1;
        case TextureFormat::R16_SINT:
        case TextureFormat::R16_UINT:
        case TextureFormat::R16_SNORM:
        case TextureFormat::R16_UNORM:
        case TextureFormat::R16_FLOAT:
        case TextureFormat::RG8_SINT:
        case TextureFormat::RG8_UINT:
        case TextureFormat::RG8_SNORM:
        case TextureFormat::RG8_UNORM:
        case TextureFormat::D16_UNORM: return 2;
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
        case TextureFormat::D24_UNORM_S8_UINT: return 4;
        case TextureFormat::RG32_SINT:
        case TextureFormat::RG32_UINT:
        case TextureFormat::RG32_FLOAT:
        case TextureFormat::RGBA16_SINT:
        case TextureFormat::RGBA16_UINT:
        case TextureFormat::RGBA16_SNORM:
        case TextureFormat::RGBA16_UNORM:
        case TextureFormat::RGBA16_FLOAT:
        case TextureFormat::D32_FLOAT_S8_UINT: return 8;
        case TextureFormat::RGBA32_SINT:
        case TextureFormat::RGBA32_UINT:
        case TextureFormat::RGBA32_FLOAT: return 16;
        case TextureFormat::UNKNOWN: return 0;
    }
    Unreachable();
}

std::string_view format_as(RenderBackend v) noexcept {
    // MAX_COUNT 是哨兵而非真实后端, 不暴露其成员名。
    if (v == RenderBackend::MAX_COUNT) {
        return "UNKNOWN";
    }
    return EnumNameOr(v);
}

std::string_view format_as(TextureFormat v) noexcept {
    return EnumNameOr(v);
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

std::string_view format_as(BufferState v) noexcept {
    // 位标志枚举, 取值超出默认反射范围, 需按位反射。
    return EnumFlagBitNameOr(v);
}

std::string_view format_as(TextureState v) noexcept {
    return EnumFlagBitNameOr(v);
}

std::string_view format_as(TextureViewUsage v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(RenderObjectTag v) noexcept {
    switch (v) {
        case RenderObjectTag::UNKNOWN: return "UNKNOWN";
        case RenderObjectTag::Device: return "Device";
        case RenderObjectTag::CmdQueue: return "CmdQueue";
        case RenderObjectTag::CmdBuffer: return "CmdBuffer";
        case RenderObjectTag::CmdEncoder: return "CmdEncoder";
        case RenderObjectTag::GraphicsCmdEncoder: return "GraphicsCmdEncoder";
        case RenderObjectTag::ComputeCmdEncoder: return "ComputeCmdEncoder";
        case RenderObjectTag::Fence: return "Fence";
        case RenderObjectTag::Shader: return "Shader";
        case RenderObjectTag::PipelineLayout: return "PipelineLayout";
        case RenderObjectTag::PipelineState: return "PipelineState";
        case RenderObjectTag::GraphicsPipelineState: return "GraphicsPipelineState";
        case RenderObjectTag::ComputePipelineState: return "ComputePipelineState";
        case RenderObjectTag::SwapChain: return "SwapChain";
        case RenderObjectTag::Resource: return "Resource";
        case RenderObjectTag::Buffer: return "Buffer";
        case RenderObjectTag::Texture: return "Texture";
        case RenderObjectTag::RenderPass: return "RenderPass";
        case RenderObjectTag::Framebuffer: return "Framebuffer";
        case RenderObjectTag::ResourceView: return "ResourceView";
        case RenderObjectTag::TextureView: return "TextureView";
        case RenderObjectTag::Sampler: return "Sampler";
        case RenderObjectTag::VkInstance: return "VkInstance";
        case RenderObjectTag::DXGIFactory: return "DXGIFactory";
        default: return "UNKNOWN";
    }
}

std::string_view format_as(PresentMode v) noexcept {
    return EnumNameOr(v);
}

std::string_view format_as(SwapChainStatus v) noexcept {
    return EnumNameOr(v);
}

}  // namespace radray::render
