#include <radray/render/common.h>

#include <radray/logger.h>
#include <radray/utility.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_METAL
#include <radray/render/backend/metal_impl_cpp.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc) {
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
            } else if constexpr (std::is_same_v<T, MetalDeviceDescriptor>) {
#ifdef RADRAY_ENABLE_METAL
                return metal::CreateDevice(arg);
#else
                RADRAY_ERR_LOG("metal disable");
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

Nullable<unique_ptr<InstanceVulkan>> CreateVulkanInstance(const VulkanInstanceDescriptor& desc) {
#ifdef RADRAY_ENABLE_VULKAN
    return vulkan::CreateVulkanInstanceImpl(desc);
#else
    RADRAY_UNUSED(desc);
    RADRAY_ERR_LOG("Vulkan disable");
    return nullptr;
#endif
}

void DestroyVulkanInstance(unique_ptr<InstanceVulkan> instance) noexcept {
#ifdef RADRAY_ENABLE_VULKAN
    return vulkan::DestroyVulkanInstanceImpl(std::move(instance));
#else
    RADRAY_UNUSED(instance);
    RADRAY_ERR_LOG("Vulkan disable");
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
    switch (v) {
        case RenderBackend::D3D12: return "D3D12";
        case RenderBackend::Vulkan: return "Vulkan";
        case RenderBackend::Metal: return "Metal";
        case RenderBackend::MAX_COUNT: return "UNKNOWN";
    }
    Unreachable();
}

std::string_view format_as(TextureFormat v) noexcept {
    switch (v) {
        case TextureFormat::UNKNOWN: return "UNKNOWN";
        case TextureFormat::R8_SINT: return "R8_SINT";
        case TextureFormat::R8_UINT: return "R8_UINT";
        case TextureFormat::R8_SNORM: return "R8_SNORM";
        case TextureFormat::R8_UNORM: return "R8_UNORM";
        case TextureFormat::R16_SINT: return "R16_SINT";
        case TextureFormat::R16_UINT: return "R16_UINT";
        case TextureFormat::R16_SNORM: return "R16_SNORM";
        case TextureFormat::R16_UNORM: return "R16_UNORM";
        case TextureFormat::R16_FLOAT: return "R16_FLOAT";
        case TextureFormat::RG8_SINT: return "RG8_SINT";
        case TextureFormat::RG8_UINT: return "RG8_UINT";
        case TextureFormat::RG8_SNORM: return "RG8_SNORM";
        case TextureFormat::RG8_UNORM: return "RG8_UNORM";
        case TextureFormat::R32_SINT: return "R32_SINT";
        case TextureFormat::R32_UINT: return "R32_UINT";
        case TextureFormat::R32_FLOAT: return "R32_FLOAT";
        case TextureFormat::RG16_SINT: return "RG16_SINT";
        case TextureFormat::RG16_UINT: return "RG16_UINT";
        case TextureFormat::RG16_SNORM: return "RG16_SNORM";
        case TextureFormat::RG16_UNORM: return "RG16_UNORM";
        case TextureFormat::RG16_FLOAT: return "RG16_FLOAT";
        case TextureFormat::RGBA8_SINT: return "RGBA8_SINT";
        case TextureFormat::RGBA8_UINT: return "RGBA8_UINT";
        case TextureFormat::RGBA8_SNORM: return "RGBA8_SNORM";
        case TextureFormat::RGBA8_UNORM: return "RGBA8_UNORM";
        case TextureFormat::RGBA8_UNORM_SRGB: return "RGBA8_UNORM_SRGB";
        case TextureFormat::BGRA8_UNORM: return "BGRA8_UNORM";
        case TextureFormat::BGRA8_UNORM_SRGB: return "BGRA8_UNORM_SRGB";
        case TextureFormat::RGB10A2_UINT: return "RGB10A2_UINT";
        case TextureFormat::RGB10A2_UNORM: return "RGB10A2_UNORM";
        case TextureFormat::RG11B10_FLOAT: return "RG11B10_FLOAT";
        case TextureFormat::RG32_SINT: return "RG32_SINT";
        case TextureFormat::RG32_UINT: return "RG32_UINT";
        case TextureFormat::RG32_FLOAT: return "RG32_FLOAT";
        case TextureFormat::RGBA16_SINT: return "RGBA16_SINT";
        case TextureFormat::RGBA16_UINT: return "RGBA16_UINT";
        case TextureFormat::RGBA16_SNORM: return "RGBA16_SNORM";
        case TextureFormat::RGBA16_UNORM: return "RGBA16_UNORM";
        case TextureFormat::RGBA16_FLOAT: return "RGBA16_FLOAT";
        case TextureFormat::RGBA32_SINT: return "RGBA32_SINT";
        case TextureFormat::RGBA32_UINT: return "RGBA32_UINT";
        case TextureFormat::RGBA32_FLOAT: return "RGBA32_FLOAT";
        case TextureFormat::D16_UNORM: return "D16_UNORM";
        case TextureFormat::D32_FLOAT: return "D32_FLOAT";
        case TextureFormat::D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case TextureFormat::D32_FLOAT_S8_UINT: return "D32_FLOAT_S8_UINT";
    }
    Unreachable();
}

std::string_view format_as(QueueType v) noexcept {
    switch (v) {
        case radray::render::QueueType::Direct: return "Direct";
        case radray::render::QueueType::Compute: return "Compute";
        case radray::render::QueueType::Copy: return "Copy";
        case QueueType::MAX_COUNT: return "UNKNOWN";
    }
    Unreachable();
}

std::string_view format_as(ShaderBlobCategory v) noexcept {
    switch (v) {
        case ShaderBlobCategory::DXIL: return "DXIL";
        case ShaderBlobCategory::SPIRV: return "SPIR-V";
        case ShaderBlobCategory::MSL: return "MSL";
        case ShaderBlobCategory::METALLIB: return "METALLIB";
    }
    Unreachable();
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
    switch (v) {
        case BufferState::UNKNOWN: return "UNKNOWN";
        case BufferState::Undefined: return "Undefined";
        case BufferState::Common: return "Common";
        case BufferState::CopySource: return "CopySource";
        case BufferState::CopyDestination: return "CopyDestination";
        case BufferState::Vertex: return "Vertex";
        case BufferState::Index: return "Index";
        case BufferState::CBuffer: return "CBuffer";
        case BufferState::ShaderRead: return "ShaderRead";
        case BufferState::UnorderedAccess: return "UnorderedAccess";
        case BufferState::Indirect: return "Indirect";
        case BufferState::HostRead: return "HostRead";
        case BufferState::HostWrite: return "HostWrite";
        case BufferState::AccelerationStructureBuildInput: return "AccelerationStructureBuildInput";
        case BufferState::AccelerationStructureBuildScratch: return "AccelerationStructureBuildScratch";
        case BufferState::AccelerationStructureRead: return "AccelerationStructureRead";
        case BufferState::ShaderTable: return "ShaderTable";
    }
    Unreachable();
}

std::string_view format_as(TextureState v) noexcept {
    switch (v) {
        case TextureState::UNKNOWN: return "UNKNOWN";
        case TextureState::Undefined: return "Undefined";
        case TextureState::Common: return "Common";
        case TextureState::Present: return "Present";
        case TextureState::CopySource: return "CopySource";
        case TextureState::CopyDestination: return "CopyDestination";
        case TextureState::ShaderRead: return "ShaderRead";
        case TextureState::RenderTarget: return "RenderTarget";
        case TextureState::DepthRead: return "DepthRead";
        case TextureState::DepthWrite: return "DepthWrite";
        case TextureState::UnorderedAccess: return "UnorderedAccess";
    }
    Unreachable();
}

std::string_view format_as(TextureViewUsage v) noexcept {
    switch (v) {
        case TextureViewUsage::UNKNOWN: return "UNKNOWN";
        case TextureViewUsage::Resource: return "Resource";
        case TextureViewUsage::RenderTarget: return "RenderTarget";
        case TextureViewUsage::DepthRead: return "DepthRead";
        case TextureViewUsage::DepthWrite: return "DepthWrite";
        case TextureViewUsage::UnorderedAccess: return "UnorderedAccess";
    }
    Unreachable();
}

std::string_view format_as(BufferViewUsage v) noexcept {
    switch (v) {
        case BufferViewUsage::CBuffer: return "CBuffer";
        case BufferViewUsage::ReadOnlyStorage: return "ReadOnlyStorage";
        case BufferViewUsage::ReadWriteStorage: return "ReadWriteStorage";
        case BufferViewUsage::TexelReadOnly: return "TexelReadOnly";
        case BufferViewUsage::TexelReadWrite: return "TexelReadWrite";
    }
    Unreachable();
}

std::string_view format_as(ResourceBindType v) noexcept {
    switch (v) {
        case ResourceBindType::CBuffer: return "CBuffer";
        case ResourceBindType::Buffer: return "Buffer";
        case ResourceBindType::TexelBuffer: return "TexelBuffer";
        case ResourceBindType::RWBuffer: return "RWBuffer";
        case ResourceBindType::RWTexelBuffer: return "RWTexelBuffer";
        case ResourceBindType::Texture: return "Texture";
        case ResourceBindType::RWTexture: return "RWTexture";
        case ResourceBindType::Sampler: return "Sampler";
        case ResourceBindType::AccelerationStructure: return "AccelerationStructure";
        case ResourceBindType::UNKNOWN: return "UNKNOWN";
    }
    Unreachable();
}

std::string_view format_as(AccelerationStructureType v) noexcept {
    switch (v) {
        case AccelerationStructureType::BottomLevel: return "BottomLevel";
        case AccelerationStructureType::TopLevel: return "TopLevel";
    }
    Unreachable();
}

std::string_view format_as(AccelerationStructureBuildMode v) noexcept {
    switch (v) {
        case AccelerationStructureBuildMode::Build: return "Build";
        case AccelerationStructureBuildMode::Update: return "Update";
    }
    Unreachable();
}

std::string_view format_as(AccelerationStructureBuildFlag v) noexcept {
    switch (v) {
        case AccelerationStructureBuildFlag::None: return "None";
        case AccelerationStructureBuildFlag::PreferFastTrace: return "PreferFastTrace";
        case AccelerationStructureBuildFlag::PreferFastBuild: return "PreferFastBuild";
        case AccelerationStructureBuildFlag::AllowUpdate: return "AllowUpdate";
        case AccelerationStructureBuildFlag::AllowCompaction: return "AllowCompaction";
    }
    Unreachable();
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
        case RenderObjectTag::RayTracingCmdEncoder: return "RayTracingCmdEncoder";
        case RenderObjectTag::Fence: return "Fence";
        case RenderObjectTag::Shader: return "Shader";
        case RenderObjectTag::RootSignature: return "RootSignature";
        case RenderObjectTag::PipelineState: return "PipelineState";
        case RenderObjectTag::GraphicsPipelineState: return "GraphicsPipelineState";
        case RenderObjectTag::ComputePipelineState: return "ComputePipelineState";
        case RenderObjectTag::RayTracingPipelineState: return "RayTracingPipelineState";
        case RenderObjectTag::SwapChain: return "SwapChain";
        case RenderObjectTag::Resource: return "Resource";
        case RenderObjectTag::Buffer: return "Buffer";
        case RenderObjectTag::Texture: return "Texture";
        case RenderObjectTag::AccelerationStructure: return "AccelerationStructure";
        case RenderObjectTag::ShaderBindingTable: return "ShaderBindingTable";
        case RenderObjectTag::ResourceView: return "ResourceView";
        case RenderObjectTag::BufferView: return "BufferView";
        case RenderObjectTag::TextureView: return "TextureView";
        case RenderObjectTag::AccelerationStructureView: return "AccelerationStructureView";
        case RenderObjectTag::DescriptorSet: return "DescriptorSet";
        case RenderObjectTag::Sampler: return "Sampler";
        case RenderObjectTag::VkInstance: return "VkInstance";
        case RenderObjectTag::BindlessArray: return "BindlessArray";
    }
    Unreachable();
}

std::string_view format_as(PresentMode v) noexcept {
    switch (v) {
        case PresentMode::FIFO: return "FIFO";
        case PresentMode::Mailbox: return "Mailbox";
        case PresentMode::Immediate: return "Immediate";
    }
    Unreachable();
}

std::string_view format_as(ShaderStage v) noexcept {
    switch (v) {
        case ShaderStage::UNKNOWN: return "UNKNOWN";
        case ShaderStage::Vertex: return "Vertex";
        case ShaderStage::Pixel: return "Pixel";
        case ShaderStage::Compute: return "Compute";
        case ShaderStage::RayGen: return "RayGen";
        case ShaderStage::Miss: return "Miss";
        case ShaderStage::ClosestHit: return "ClosestHit";
        case ShaderStage::AnyHit: return "AnyHit";
        case ShaderStage::Intersection: return "Intersection";
        case ShaderStage::Callable: return "Callable";
        case ShaderStage::Graphics: return "Graphics";
        case ShaderStage::RayTracing: return "RayTracing";
    }
    Unreachable();
}

}  // namespace radray::render
