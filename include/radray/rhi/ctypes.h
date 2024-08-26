#pragma once

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#endif

#define RADRAY_RHI_RESOURCE(name) \
    typedef struct name {         \
        void* Ptr;                \
        void* Native;             \
    } name;

#define RADRAY_IS_EMPTY_RESOURCE(res) res.Ptr == nullptr

#define RADRAY_RHI_MAX_VERTEX_ELEMENT 8
#define RADRAY_RHI_MAX_MRT 8

#ifdef __cplusplus
extern "C" {
#endif

typedef void* RadrayDevice;

RADRAY_RHI_RESOURCE(RadrayCommandQueue);
RADRAY_RHI_RESOURCE(RadrayCommandAllocator);
RADRAY_RHI_RESOURCE(RadrayCommandList);
RADRAY_RHI_RESOURCE(RadrayFence);
RADRAY_RHI_RESOURCE(RadraySwapChain);
RADRAY_RHI_RESOURCE(RadrayBuffer);
RADRAY_RHI_RESOURCE(RadrayTexture);
RADRAY_RHI_RESOURCE(RadraySampler);
RADRAY_RHI_RESOURCE(RadrayShader);
RADRAY_RHI_RESOURCE(RadrayRootSignature);
RADRAY_RHI_RESOURCE(RadrayGraphicsPipeline);

typedef struct RadrayBufferView {
    void* Handle;
} RadrayBufferView;

typedef struct RadrayTextureView {
    void* Handle;
} RadrayTextureView;

typedef enum RadrayBackand {
    RADRAY_BACKEND_D3D12,
    RADRAY_BACKEND_METAL,
    RADRAY_BACKEND_VULKAN,
} RadrayRhiBackand;

typedef enum RadrayQueueType {
    RADRAY_QUEUE_TYPE_DIRECT,
    RADRAY_QUEUE_TYPE_COMPUTE,
    RADRAY_QUEUE_TYPE_COPY,
} RadrayQueueType;

typedef enum RadrayFormat {
    RADRAY_FORMAT_UNKNOWN = 0,
    RADRAY_FORMAT_R8_SINT = 1,
    RADRAY_FORMAT_R8_UINT = 2,
    RADRAY_FORMAT_R8_UNORM = 3,
    RADRAY_FORMAT_RG8_SINT = 4,
    RADRAY_FORMAT_RG8_UINT = 5,
    RADRAY_FORMAT_RG8_UNORM = 6,
    RADRAY_FORMAT_RGBA8_SINT = 7,
    RADRAY_FORMAT_RGBA8_UINT = 8,
    RADRAY_FORMAT_RGBA8_UNORM = 9,
    RADRAY_FORMAT_R16_SINT = 10,
    RADRAY_FORMAT_R16_UINT = 11,
    RADRAY_FORMAT_R16_UNORM = 12,
    RADRAY_FORMAT_RG16_SINT = 13,
    RADRAY_FORMAT_RG16_UINT = 14,
    RADRAY_FORMAT_RG16_UNORM = 15,
    RADRAY_FORMAT_RGBA16_SINT = 16,
    RADRAY_FORMAT_RGBA16_UINT = 17,
    RADRAY_FORMAT_RGBA16_UNORM = 18,
    RADRAY_FORMAT_R32_SINT = 19,
    RADRAY_FORMAT_R32_UINT = 20,
    RADRAY_FORMAT_RG32_SINT = 21,
    RADRAY_FORMAT_RG32_UINT = 22,
    RADRAY_FORMAT_RGBA32_SINT = 23,
    RADRAY_FORMAT_RGBA32_UINT = 24,
    RADRAY_FORMAT_R16_FLOAT = 25,
    RADRAY_FORMAT_RG16_FLOAT = 26,
    RADRAY_FORMAT_RGBA16_FLOAT = 27,
    RADRAY_FORMAT_R32_FLOAT = 28,
    RADRAY_FORMAT_RG32_FLOAT = 29,
    RADRAY_FORMAT_RGBA32_FLOAT = 30,
    RADRAY_FORMAT_R10G10B10A2_UINT = 31,
    RADRAY_FORMAT_R10G10B10A2_UNORM = 32,
    RADRAY_FORMAT_R11G11B10_FLOAT = 33,

    RADRAY_FORMAT_D16_UNORM = 34,
    RADRAY_FORMAT_D32_FLOAT = 35,
    RADRAY_FORMAT_D24_UNORM_S8_UINT = 36,
    RADRAY_FORMAT_D32_FLOAT_S8_UINT = 37
} RadrayFormat;

typedef enum RadrayFilterMode {
    RADRAY_FILTER_MODE_NEAREST = 0,
    RADRAY_FILTER_MODE_LINEAR = 1
} RadrayFilterMode;

typedef enum RadrayAddressMode {
    RADRAY_ADDRESS_MODE_MIRROR,
    RADRAY_ADDRESS_MODE_REPEAT,
    RADRAY_ADDRESS_MODE_CLAMP_TO_EDGE,
    RADRAY_ADDRESS_MODE_CLAMP_TO_BORDER
} RadrayAddressMode;

typedef enum RadrayMipMapMode {
    RADRAY_MIPMAP_MODE_NEAREST,
    RADRAY_MIPMAP_MODE_LINEAR
} RadrayMipMapMode;

typedef enum RadrayTopology {
    RADRAY_TOPOLOGY_POINT_LIST,
    RADRAY_TOPOLOGY_LINE_LIST,
    RADRAY_TOPOLOGY_LINE_STRIP,
    RADRAY_TOPOLOGY_TRI_LIST,
    RADRAY_TOPOLOGY_TRI_STRIP,
    RADRAY_TOPOLOGY_PATCH_LIST
} RadrayTopology;

typedef enum RadrayBlendType {
    RADRAY_BLEND_TYPE_ZERO,
    RADRAY_BLEND_TYPE_ONE,
    RADRAY_BLEND_TYPE_SRC_COLOR,
    RADRAY_BLEND_TYPE_INV_SRC_COLOR,
    RADRAY_BLEND_TYPE_DST_COLOR,
    RADRAY_BLEND_TYPE_INV_DST_COLOR,
    RADRAY_BLEND_TYPE_SRC_ALPHA,
    RADRAY_BLEND_TYPE_INV_SRC_ALPHA,
    RADRAY_BLEND_TYPE_DST_ALPHA,
    RADRAY_BLEND_TYPE_INV_DST_ALPHA,
    RADRAY_BLEND_TYPE_SRC_ALPHA_SATURATE,
    RADRAY_BLEND_TYPE_BLEND_FACTOR,
    RADRAY_BLEND_TYPE_INV_BLEND_FACTOR,
} RadrayBlendType;

typedef enum RadrayBlendOp {
    RADRAY_BLEND_OP_ADD,
    RADRAY_BLEND_OP_SUBTRACT,
    RADRAY_BLEND_OP_REVERSE_SUBTRACT,
    RADRAY_BLEND_OP_MIN,
    RADRAY_BLEND_OP_MAX
} RadrayBlendOp;

typedef enum RadrayCullMode {
    RADRAY_CULL_MODE_NONE,
    RADRAY_CULL_MODE_BACK,
    RADRAY_CULL_MODE_FRONT,
    RADRAY_CULL_MODE_BOTH
} RadrayCullMode;

typedef enum RadrayFrontFace {
    RADRAY_FRONT_FACE_CCW,
    RADRAY_FRONT_FACE_CW
} RadrayFrontFace;

typedef enum RadrayFillMode {
    RADRAY_FILL_MODE_SOLID,
    RADRAY_FILL_MODE_WIREFRAME
} RadrayFillMode;

typedef enum RadrayVertexInputRate {
    RADRAY_INPUT_RATE_VERTEX,
    RADRAY_INPUT_RATE_INSTANCE
} RadrayVertexInputRate;

typedef enum RadrayCompareMode {
    RADRAY_COMPARE_NEVER,
    RADRAY_COMPARE_LESS,
    RADRAY_COMPARE_EQUAL,
    RADRAY_COMPARE_LEQUAL,
    RADRAY_COMPARE_GREATER,
    RADRAY_COMPARE_NOTEQUAL,
    RADRAY_COMPARE_GEQUAL,
    RADRAY_COMPARE_ALWAYS
} RadrayCompareMode;

typedef enum RadrayStencilOp {
    RADRAY_STENCIL_OP_KEEP,
    RADRAY_STENCIL_OP_SET_ZERO,
    RADRAY_STENCIL_OP_REPLACE,
    RADRAY_STENCIL_OP_INVERT,
    RADRAY_STENCIL_OP_INCR,
    RADRAY_STENCIL_OP_DECR,
    RADRAY_STENCIL_OP_INCR_SAT,
    RADRAY_STENCIL_OP_DECR_SAT
} RadrayStencilOp;

typedef enum RadrayTextureDimension {
    RADRAY_TEXTURE_DIM_UNKNOWN,
    RADRAY_TEXTURE_DIM_1D,
    RADRAY_TEXTURE_DIM_2D,
    RADRAY_TEXTURE_DIM_3D,
    RADRAY_TEXTURE_DIM_CUBE,
    RADRAY_TEXTURE_DIM_1D_ARRAY,
    RADRAY_TEXTURE_DIM_2D_ARRAY,
    RADRAY_TEXTURE_DIM_CUBE_ARRAY
} RadrayTextureDimension;

typedef enum RadrayFenceState {
    RADRAY_FENCE_STATE_COMPLETE,
    RADRAY_FENCE_STATE_INCOMPLETE,
    RADRAY_FENCE_STATE_NOTSUBMITTED
} RadrayFenceState;

typedef enum RadrayResourceState {
    RADRAY_RESOURCE_STATE_UNKNOWN = 0,
    RADRAY_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER = 0x1,
    RADRAY_RESOURCE_STATE_INDEX_BUFFER = 0x2,
    RADRAY_RESOURCE_STATE_RENDER_TARGET = 0x4,
    RADRAY_RESOURCE_STATE_UNORDERED_ACCESS = 0x8,
    RADRAY_RESOURCE_STATE_DEPTH_WRITE = 0x10,
    RADRAY_RESOURCE_STATE_DEPTH_READ = 0x20,
    RADRAY_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 0x40,
    RADRAY_RESOURCE_STATE_PIXEL_SHADER_RESOURCE = 0x80,
    RADRAY_RESOURCE_STATE_SHADER_RESOURCE = 0x40 | 0x80,
    RADRAY_RESOURCE_STATE_STREAM_OUT = 0x100,
    RADRAY_RESOURCE_STATE_INDIRECT_ARGUMENT = 0x200,
    RADRAY_RESOURCE_STATE_COPY_DEST = 0x400,
    RADRAY_RESOURCE_STATE_COPY_SOURCE = 0x800,
    RADRAY_RESOURCE_STATE_GENERIC_READ = (((((0x1 | 0x2) | 0x40) | 0x80) | 0x200) | 0x800),
    RADRAY_RESOURCE_STATE_PRESENT = 0x1000,
    RADRAY_RESOURCE_STATE_COMMON = 0x2000
} RadrayResourceState;

typedef uint32_t RadrayResourceStates;

typedef enum RadrayResourceType {
    RADRAY_RESOURCE_TYPE_UNKNOWN = 0,

    RADRAY_RESOURCE_TYPE_BUFFER = 0x1,
    RADRAY_RESOURCE_TYPE_BUFFER_RW = 0x2,
    RADRAY_RESOURCE_TYPE_CBUFFER = 0x4,
    RADRAY_RESOURCE_TYPE_VERTEX_BUFFER = 0x8,
    RADRAY_RESOURCE_TYPE_INDEX_BUFFER = 0x10,

    RADRAY_RESOURCE_TYPE_TEXTURE = 0x20,
    RADRAY_RESOURCE_TYPE_TEXTURE_RW = 0x40,
    RADRAY_RESOURCE_TYPE_RENDER_TARGET = 0x80,
    RADRAY_RESOURCE_TYPE_DEPTH_STENCIL = 0x100,

    RADRAY_RESOURCE_TYPE_SAMPLER = 0x200,

    RADRAY_RESOURCE_TYPE_RAYTRACING = 0x400
} RadrayResourceType;

typedef uint32_t RadrayResourceTypes;

typedef enum RadrayHeapUsage {
    RADRAY_HEAP_USAGE_DEFAULT,
    RADRAY_HEAP_USAGE_UPLOAD,
    RADRAY_HEAP_USAGE_READBACK
} RadrayGpuHeapType;

typedef enum RadrayBufferCreateFlag {
    RADRAY_BUFFER_CREATE_FLAG_COMMITTED = 0x1
} RadrayBufferCreateFlag;

typedef uint32_t RadrayBufferCreateFlags;

typedef enum RadrayTextureMSAACount {
    RADRAY_TEXTURE_MSAA_1 = 1,
    RADRAY_TEXTURE_MSAA_2 = 2,
    RADRAY_TEXTURE_MSAA_4 = 4,
    RADRAY_TEXTURE_MSAA_8 = 8,
    RADRAY_TEXTURE_MSAA_16 = 16
} RadrayTextureMSAACount;

typedef enum RadrayTextureCreateFlag {
    RADRAY_TEXTURE_CREATE_FLAG_COMMITTED = 0x1
} RadrayTextureCreateFlag;

typedef uint32_t RadrayTextureCreateFlags;

typedef enum RadrayShaderStage {
    RADRAY_SHADER_STAGE_UNKNOWN = 0,

    RADRAY_SHADER_STAGE_VERTEX = 0x00000001,
    RADRAY_SHADER_STAGE_HULL = 0x00000002,
    RADRAY_SHADER_STAGE_DOMAIN = 0x00000004,
    RADRAY_SHADER_STAGE_GEOMETRY = 0x00000008,
    RADRAY_SHADER_STAGE_PIXEL = 0x00000010,
    RADRAY_SHADER_STAGE_COMPUTE = 0x00000020,
    RADRAY_SHADER_STAGE_RAYTRACING = 0x00000040,

    RADRAY_SHADER_STAGE_ALL_GRAPHICS = (uint32_t)RADRAY_SHADER_STAGE_VERTEX | (uint32_t)RADRAY_SHADER_STAGE_HULL | (uint32_t)RADRAY_SHADER_STAGE_DOMAIN | (uint32_t)RADRAY_SHADER_STAGE_GEOMETRY | (uint32_t)RADRAY_SHADER_STAGE_PIXEL
} RadrayShaderStage;

typedef uint32_t RadrayShaderStages;

typedef enum RadrayVertexSemantic {
    RADRAY_VERTEX_SEMANTIC_POSITION,
    RADRAY_VERTEX_SEMANTIC_NORMAL,
    RADRAY_VERTEX_SEMANTIC_TEXCOORD,
    RADRAY_VERTEX_SEMANTIC_TANGENT,
    RADRAY_VERTEX_SEMANTIC_COLOR,
    RADRAY_VERTEX_SEMANTIC_PSIZE,
    RADRAY_VERTEX_SEMANTIC_BINORMAL,
    RADRAY_VERTEX_SEMANTIC_BLENDINDICES,
    RADRAY_VERTEX_SEMANTIC_BLENDWEIGHT,
    RADRAY_VERTEX_SEMANTIC_POSITIONT
} RadrayVertexSemantic;

typedef struct RadrayDeviceDescriptorD3D12 {
    uint32_t AdapterIndex;
    bool IsEnableDebugLayer;
} RadrayDeviceDescriptorD3D12;

typedef struct RadrayDeviceDescriptorMetal {
    uint32_t DeviceIndex;
} RadrayDeviceDescriptorMetal;

typedef struct RadraySwapChainDescriptor {
    RadrayCommandQueue PresentQueue;
    uint64_t NativeWindow;
    uint32_t Width;
    uint32_t Height;
    uint32_t BackBufferCount;
    RadrayFormat Format;
    bool EnableSync;
} RadraySwapChainDescriptor;

typedef struct RadrayBufferDescriptor {
    const uint8_t* Name;
    uint64_t Size;
    RadrayHeapUsage Usage;
    RadrayResourceStates InitStates;
    RadrayResourceTypes MaybeTypes;
    RadrayBufferCreateFlags Flags;
} RadrayBufferDescriptor;

typedef struct RadrayBufferViewDescriptor {
    RadrayBuffer Buffer;
    RadrayResourceType Type;
    RadrayFormat Format;
    /** SRV or UAV param */
    uint64_t FirstElementOffset;
    /** SRV or UAV param */
    uint32_t ElementCount;
    /** SRV or UAV param */
    uint32_t ElementStride;
} RadrayBufferViewDescriptor;

typedef union RadrayClearValue {
    struct {
        float R;
        float G;
        float B;
        float A;
    };
    struct {
        float Depth;
        uint32_t Stencil;
    };
} RadrayClearValue;

typedef struct RadrayTextureDescriptor {
    const uint8_t* Name;
    uint32_t Width;
    uint32_t Height;
    uint32_t Depth;
    uint32_t ArraySize;
    RadrayFormat Format;
    uint32_t MipLevels;
    RadrayTextureMSAACount SampleCount;
    uint32_t Quality;
    RadrayClearValue ClearValue;
    RadrayResourceStates InitStates;
    RadrayResourceTypes MaybeTypes;
    RadrayTextureCreateFlags Flags;
} RadrayTextureDescriptor;

typedef struct RadrayTextureViewDescriptor {
    RadrayTexture Texture;
    RadrayFormat Format;
    RadrayResourceType Type;
    RadrayTextureDimension Dimension;
    uint32_t BaseArrayLayer;
    uint32_t ArrayLayerCount;
    uint32_t BaseMipLevel;
    uint32_t MipLevelCount;
} RadrayTextureViewDescriptor;

typedef struct RadrayCompileRasterizationShaderDescriptor {
    const char* Name;
    const char* Data;
    size_t DataLength;
    const char* EntryPoint;
    RadrayShaderStage Stage;
    uint32_t ShaderModel;
    const char* const* Defines;
    size_t DefineCount;
    bool IsOptimize;
} RadrayCompileRasterizationShaderDescriptor;

typedef struct RadrayRootSignatureDescriptor {
    const RadrayShader* Shaders;
    size_t ShaderCount;
    const RadraySampler* StaticSamplers;
    const char* const* StaticSamplerNames;
    size_t StaticSamplerCount;
} RadrayRootSignatureDescriptor;

typedef struct RadrayVertexElement {
    RadrayVertexSemantic Semantic;
    uint32_t SemanticIndex;
    RadrayFormat Format;
    uint32_t Binding;
    uint32_t Offset;
    uint32_t Stride;
    RadrayVertexInputRate Rate;
} RadrayVertexElement;

typedef struct RadrayVertexLayout {
    uint32_t ElementCount;
    RadrayVertexElement Elements[RADRAY_RHI_MAX_VERTEX_ELEMENT];
} RadrayVertexLayout;

typedef struct RadrayBlendStateDescriptor {
    RadrayBlendType SrcFactors;
    RadrayBlendType DstFactors;
    RadrayBlendType SrcAlphaFactors;
    RadrayBlendType DstAlphaFactors;
    RadrayBlendOp BlendModes;
    RadrayBlendOp BlendAlphaModes;
    int32_t Masks;
    bool AlphaTocoverage;
    bool IndependentBlend;
} RadrayBlendStateDescriptor;

typedef struct RadrayDepthStencilDescriptor {
    RadrayCompareMode DepthFunc;
    RadrayCompareMode StencilFrontFunc;
    RadrayStencilOp StencilFrontFail;
    RadrayStencilOp DepthFrontFail;
    RadrayStencilOp StencilFrontPass;
    RadrayCompareMode StencilBackFunc;
    RadrayStencilOp StencilBackFail;
    RadrayStencilOp DepthBackFail;
    RadrayStencilOp StencilBackPass;
    bool IsEnableDepthTest;
    bool IsEnableDepthWrite;
    bool IsEnableStencil;
} RadrayDepthStencilDescriptor;

typedef struct RadrayRasterizerStateDescriptor {
    RadrayCullMode Cull;
    RadrayFillMode Fill;
    RadrayFrontFace FrontFace;
    int32_t DepthBias;
    float SlopeScaledDepthBias;
    bool IsEnableMultiSample;
    bool IsEnableScissor;
    bool IsEnableDepthClamp;
} RadrayRasterizerStateDescriptor;

typedef struct RadrayGraphicsPipelineDescriptor {
    RadrayRootSignature RootSignature;
    RadrayShader VertexShader;
    RadrayShader HullShader;
    RadrayShader DomainShader;
    RadrayShader GeometryShader;
    RadrayShader PixelShader;
    RadrayVertexLayout VertexLayout;
    RadrayBlendStateDescriptor Blend;
    RadrayDepthStencilDescriptor DepthStencil;
    RadrayRasterizerStateDescriptor Raster;
    RadrayFormat ColorFormats[RADRAY_RHI_MAX_MRT];
    uint32_t RenderTargetCount;
    RadrayTextureMSAACount SampleCount;
    uint32_t SampleQuality;
    RadrayFormat DepthStencilFormat;
    RadrayTopology PrimitiveTopology;
} RadrayGraphicsPipelineDescriptor;

RadrayDevice RadrayCreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc);

RadrayDevice RadrayCreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc);

void RadrayReleaseDevice(RadrayDevice device);

#ifdef __cplusplus
}
#endif
