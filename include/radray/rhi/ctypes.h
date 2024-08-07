#pragma once

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#include <stdbool.h>
#endif

#define RADRAY_RHI_RESOURCE(name) \
    typedef struct name {         \
        void* Ptr;                \
        void* Native;             \
    } name;

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
    RADRAY_RESOURCE_TYPE_TEXTURE_CUBE = 0x200
} RadrayResourceType;

typedef uint32_t RadrayResourceTypes;

typedef enum RadrayHeapUsage {
    RADRAY_HEAP_USAGE_DEFAULT,
    RADRAY_HEAP_USAGE_UPLOAD,
    RADRAY_HEAP_USAGE_READBACK
} RadrayGpuHeapType;

typedef enum RadrayBufferCreateFlag {
    RADRAY_BUFFER_CREATE_FLAG_COMMITTED = 0x1,
    RADRAY_BUFFER_CREATE_FLAG_PERSISTENT_MAP = 0x2
} RadrayBufferCreateFlag;

typedef uint32_t RadrayBufferCreateFlags;

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
    uint64_t Size;
    RadrayHeapUsage Usage;
    RadrayFormat Format;
    RadrayResourceTypes Types;
    RadrayResourceStates InitStates;
    RadrayBufferCreateFlags Flags;
} RadrayBufferDescriptor;

RadrayDevice RadrayCreateDeviceD3D12(const RadrayDeviceDescriptorD3D12* desc);

RadrayDevice RadrayCreateDeviceMetal(const RadrayDeviceDescriptorMetal* desc);

void RadrayReleaseDevice(RadrayDevice device);

#ifdef __cplusplus
}
#endif
