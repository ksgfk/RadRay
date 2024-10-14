#include <radray/rhi/helper.h>

namespace radray::rhi {
}  // namespace radray::rhi

std::string_view format_as(RadrayBackand v) noexcept {
    switch (v) {
        case RADRAY_BACKEND_D3D12: return "D3D12";
        case RADRAY_BACKEND_METAL: return "Metal";
        case RADRAY_BACKEND_VULKAN: return "Vulakn";
    }
}

std::string_view format_as(RadrayQueueType v) noexcept {
    switch (v) {
        case RADRAY_QUEUE_TYPE_DIRECT: return "Direct";
        case RADRAY_QUEUE_TYPE_COMPUTE: return "Compute";
        case RADRAY_QUEUE_TYPE_COPY: return "Copy";
    }
}

std::string_view format_as(RadrayFormat v) noexcept {
    switch (v) {
        case RADRAY_FORMAT_R8_SINT: return "R8_SINT";
        case RADRAY_FORMAT_R8_UINT: return "R8_UINT";
        case RADRAY_FORMAT_R8_UNORM: return "R8_UNORM";
        case RADRAY_FORMAT_RG8_SINT: return "RG8_SINT";
        case RADRAY_FORMAT_RG8_UINT: return "RG8_UINT";
        case RADRAY_FORMAT_RG8_UNORM: return "RG8_UNORM";
        case RADRAY_FORMAT_RGBA8_SINT: return "RGBA8_SINT";
        case RADRAY_FORMAT_RGBA8_UINT: return "RGBA8_UINT";
        case RADRAY_FORMAT_RGBA8_UNORM: return "RGBA8_UNORM";
        case RADRAY_FORMAT_R16_SINT: return "R16_SINT";
        case RADRAY_FORMAT_R16_UINT: return "R16_UINT";
        case RADRAY_FORMAT_R16_UNORM: return "R16_UNORM";
        case RADRAY_FORMAT_RG16_SINT: return "RG16_SINT";
        case RADRAY_FORMAT_RG16_UINT: return "RG16_UINT";
        case RADRAY_FORMAT_RG16_UNORM: return "RG16_UNORM";
        case RADRAY_FORMAT_RGBA16_SINT: return "RGBA16_SINT";
        case RADRAY_FORMAT_RGBA16_UINT: return "RGBA16_UINT";
        case RADRAY_FORMAT_RGBA16_UNORM: return "RGBA16_UNORM";
        case RADRAY_FORMAT_R32_SINT: return "R32_SINT";
        case RADRAY_FORMAT_R32_UINT: return "R32_UINT";
        case RADRAY_FORMAT_RG32_SINT: return "RG32_SINT";
        case RADRAY_FORMAT_RG32_UINT: return "RG32_UINT";
        case RADRAY_FORMAT_RGBA32_SINT: return "RGBA32_SINT";
        case RADRAY_FORMAT_RGBA32_UINT: return "RGBA32_UINT";
        case RADRAY_FORMAT_R16_FLOAT: return "R16_FLOAT";
        case RADRAY_FORMAT_RG16_FLOAT: return "RG16_FLOAT";
        case RADRAY_FORMAT_RGBA16_FLOAT: return "RGBA16_FLOAT";
        case RADRAY_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case RADRAY_FORMAT_RG32_FLOAT: return "RG32_FLOAT";
        case RADRAY_FORMAT_RGBA32_FLOAT: return "RGBA32_FLOAT";
        case RADRAY_FORMAT_R10G10B10A2_UINT: return "R10G10B10A2_UINT";
        case RADRAY_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
        case RADRAY_FORMAT_R11G11B10_FLOAT: return "R11G11B10_FLOAT";
        case RADRAY_FORMAT_D16_UNORM: return "D16_UNORM";
        case RADRAY_FORMAT_D32_FLOAT: return "D32_FLOAT";
        case RADRAY_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
        case RADRAY_FORMAT_D32_FLOAT_S8_UINT: return "D32_FLOAT_S8_UINT";
        case RADRAY_FORMAT_BGRA8_UNORM: return "BGRA8_UNORM";
        case RADRAY_FORMAT_UNKNOWN: return "UNKNOWN";
    }
}

std::string_view format_as(RadrayFilterMode v) noexcept {
    switch (v) {
        case RADRAY_FILTER_MODE_NEAREST: return "Nearest";
        case RADRAY_FILTER_MODE_LINEAR: return "Linear";
    }
}

std::string_view format_as(RadrayAddressMode v) noexcept {
    switch (v) {
        case RADRAY_ADDRESS_MODE_MIRROR: return "Mirror";
        case RADRAY_ADDRESS_MODE_REPEAT: return "Repeat";
        case RADRAY_ADDRESS_MODE_CLAMP_TO_EDGE: return "ClampEdge";
        case RADRAY_ADDRESS_MODE_CLAMP_TO_BORDER: return "ClampBorder";
    }
}

std::string_view format_as(RadrayMipMapMode v) noexcept {
    switch (v) {
        case RADRAY_MIPMAP_MODE_NEAREST: return "Nearest";
        case RADRAY_MIPMAP_MODE_LINEAR: return "Linear";
    }
}

std::string_view format_as(RadrayPrimitiveTopology v) noexcept {
    switch (v) {
        case RADRAY_PRIMITIVE_TOPOLOGY_POINT_LIST: return "PointList";
        case RADRAY_PRIMITIVE_TOPOLOGY_LINE_LIST: return "LineList";
        case RADRAY_PRIMITIVE_TOPOLOGY_LINE_STRIP: return "LineStrip";
        case RADRAY_PRIMITIVE_TOPOLOGY_TRI_LIST: return "TriangleList";
        case RADRAY_PRIMITIVE_TOPOLOGY_TRI_STRIP: return "TriangleStrip";
    }
}

std::string_view format_as(RadrayBlendType v) noexcept {
    switch (v) {
        case RADRAY_BLEND_TYPE_ZERO: return "Zero";
        case RADRAY_BLEND_TYPE_ONE: return "One";
        case RADRAY_BLEND_TYPE_SRC_COLOR: return "SrcColor";
        case RADRAY_BLEND_TYPE_INV_SRC_COLOR: return "InvSrcColor";
        case RADRAY_BLEND_TYPE_DST_COLOR: return "DstColor";
        case RADRAY_BLEND_TYPE_INV_DST_COLOR: return "InvDstColor";
        case RADRAY_BLEND_TYPE_SRC_ALPHA: return "SrcAlpha";
        case RADRAY_BLEND_TYPE_INV_SRC_ALPHA: return "InvSrcAlpha";
        case RADRAY_BLEND_TYPE_DST_ALPHA: return "DstAlpha";
        case RADRAY_BLEND_TYPE_INV_DST_ALPHA: return "InvDstAlpha";
        case RADRAY_BLEND_TYPE_SRC_ALPHA_SATURATE: return "SrcAlphaSat";
        case RADRAY_BLEND_TYPE_BLEND_FACTOR: return "BlendFactor";
        case RADRAY_BLEND_TYPE_INV_BLEND_FACTOR: return "InvBlendFactor";
    }
}

std::string_view format_as(RadrayBlendOp v) noexcept {
    switch (v) {
        case RADRAY_BLEND_OP_ADD: return "Add";
        case RADRAY_BLEND_OP_SUBTRACT: return "Sub";
        case RADRAY_BLEND_OP_REVERSE_SUBTRACT: return "RevSub";
        case RADRAY_BLEND_OP_MIN: return "Min";
        case RADRAY_BLEND_OP_MAX: return "Max";
    }
}

std::string_view format_as(RadrayCullMode v) noexcept {
    switch (v) {
        case RADRAY_CULL_MODE_NONE: return "None";
        case RADRAY_CULL_MODE_BACK: return "Back";
        case RADRAY_CULL_MODE_FRONT: return "Front";
        case RADRAY_CULL_MODE_BOTH: return "Both";
    }
}

std::string_view format_as(RadrayFrontFace v) noexcept {
    switch (v) {
        case RADRAY_FRONT_FACE_CCW: return "CCW";
        case RADRAY_FRONT_FACE_CW: return "CW";
    }
}

std::string_view format_as(RadrayFillMode v) noexcept {
    switch (v) {
        case RADRAY_FILL_MODE_SOLID: return "Solid";
        case RADRAY_FILL_MODE_WIREFRAME: return "Wireframe";
    }
}

std::string_view format_as(RadrayVertexInputRate v) noexcept {
    switch (v) {
        case RADRAY_INPUT_RATE_VERTEX: return "Vertex";
        case RADRAY_INPUT_RATE_INSTANCE: return "Instance";
    }
}

std::string_view format_as(RadrayCompareMode v) noexcept {
    switch (v) {
        case RADRAY_COMPARE_NEVER: return "Never";
        case RADRAY_COMPARE_LESS: return "Less";
        case RADRAY_COMPARE_EQUAL: return "Equal";
        case RADRAY_COMPARE_LEQUAL: return "LessEqual";
        case RADRAY_COMPARE_GREATER: return "Greater";
        case RADRAY_COMPARE_NOTEQUAL: return "NotEqual";
        case RADRAY_COMPARE_GEQUAL: return "GreatEqual";
        case RADRAY_COMPARE_ALWAYS: return "Always";
    }
}

std::string_view format_as(RadrayStencilOp v) noexcept {
    switch (v) {
        case RADRAY_STENCIL_OP_KEEP: return "Keep";
        case RADRAY_STENCIL_OP_SET_ZERO: return "SetZero";
        case RADRAY_STENCIL_OP_REPLACE: return "Replace";
        case RADRAY_STENCIL_OP_INVERT: return "Invert";
        case RADRAY_STENCIL_OP_INCR: return "Inc";
        case RADRAY_STENCIL_OP_DECR: return "Dec";
        case RADRAY_STENCIL_OP_INCR_SAT: return "IncSat";
        case RADRAY_STENCIL_OP_DECR_SAT: return "DecSat";
    }
}

std::string_view format_as(RadrayTextureDimension v) noexcept {
    switch (v) {
        case RADRAY_TEXTURE_DIM_1D: return "1D";
        case RADRAY_TEXTURE_DIM_2D: return "2D";
        case RADRAY_TEXTURE_DIM_3D: return "3D";
        case RADRAY_TEXTURE_DIM_CUBE: return "Cube";
        case RADRAY_TEXTURE_DIM_1D_ARRAY: return "1DArray";
        case RADRAY_TEXTURE_DIM_2D_ARRAY: return "2DArray";
        case RADRAY_TEXTURE_DIM_CUBE_ARRAY: return "CubeArray";
        case RADRAY_TEXTURE_DIM_UNKNOWN: return "UNKNOWN";
    }
}

std::string_view format_as(RadrayFenceState v) noexcept {
    switch (v) {
        case RADRAY_FENCE_STATE_COMPLETE: return "Complete";
        case RADRAY_FENCE_STATE_INCOMPLETE: return "Incomplete";
        case RADRAY_FENCE_STATE_NOTSUBMITTED: return "NotSubmitted";
    }
}

std::string_view format_as(RadrayResourceState v) noexcept {
    switch (v) {
        case RADRAY_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER: return "Vertex&CB";
        case RADRAY_RESOURCE_STATE_INDEX_BUFFER: return "IBuffer";
        case RADRAY_RESOURCE_STATE_RENDER_TARGET: return "RT";
        case RADRAY_RESOURCE_STATE_UNORDERED_ACCESS: return "UA";
        case RADRAY_RESOURCE_STATE_DEPTH_WRITE: return "DepthWrite";
        case RADRAY_RESOURCE_STATE_DEPTH_READ: return "DepthRead";
        case RADRAY_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE: return "NoPSRes";
        case RADRAY_RESOURCE_STATE_PIXEL_SHADER_RESOURCE: return "PSRes";
        case RADRAY_RESOURCE_STATE_SHADER_RESOURCE: return "ShaderRes";
        case RADRAY_RESOURCE_STATE_STREAM_OUT: return "StreamOut";
        case RADRAY_RESOURCE_STATE_INDIRECT_ARGUMENT: return "IndirectArg";
        case RADRAY_RESOURCE_STATE_COPY_DEST: return "CopyDst";
        case RADRAY_RESOURCE_STATE_COPY_SOURCE: return "CopySrc";
        case RADRAY_RESOURCE_STATE_GENERIC_READ: return "GenericRead";
        case RADRAY_RESOURCE_STATE_PRESENT: return "Present";
        case RADRAY_RESOURCE_STATE_COMMON: return "Common";
        case RADRAY_RESOURCE_STATE_UNKNOWN: return "UNKNOWN";
    }
}

std::string_view format_as(RadrayResourceType v) noexcept {
    switch (v) {
        case RADRAY_RESOURCE_TYPE_BUFFER: return "Buffer";
        case RADRAY_RESOURCE_TYPE_BUFFER_RW: return "BufferRW";
        case RADRAY_RESOURCE_TYPE_CBUFFER: return "CBuffer";
        case RADRAY_RESOURCE_TYPE_VERTEX_BUFFER: return "VertexBuffer";
        case RADRAY_RESOURCE_TYPE_INDEX_BUFFER: return "IndexBuffer";
        case RADRAY_RESOURCE_TYPE_TEXTURE: return "Texture";
        case RADRAY_RESOURCE_TYPE_TEXTURE_RW: return "TextureRW";
        case RADRAY_RESOURCE_TYPE_RENDER_TARGET: return "RT";
        case RADRAY_RESOURCE_TYPE_DEPTH_STENCIL: return "DS";
        case RADRAY_RESOURCE_TYPE_SAMPLER: return "Sampler";
        case RADRAY_RESOURCE_TYPE_RAYTRACING: return "RayTracing";
        case RADRAY_RESOURCE_TYPE_UNKNOWN: return "UNKNOWN";
    }
}

std::string_view format_as(RadrayHeapUsage v) noexcept {
    switch (v) {
        case RADRAY_HEAP_USAGE_DEFAULT: return "Default";
        case RADRAY_HEAP_USAGE_UPLOAD: return "Upload";
        case RADRAY_HEAP_USAGE_READBACK: return "ReadBack";
    }
}

std::string_view format_as(RadrayBufferCreateFlag v) noexcept {
    switch (v) {
        case RADRAY_BUFFER_CREATE_FLAG_COMMITTED: return "Committed";
    }
}

std::string_view format_as(RadrayTextureMSAACount v) noexcept {
    switch (v) {
        case RADRAY_TEXTURE_MSAA_1: return "x1";
        case RADRAY_TEXTURE_MSAA_2: return "x2";
        case RADRAY_TEXTURE_MSAA_4: return "x4";
        case RADRAY_TEXTURE_MSAA_8: return "x8";
        case RADRAY_TEXTURE_MSAA_16: return "x16";
    }
}

std::string_view format_as(RadrayTextureCreateFlag v) noexcept {
    switch (v) {
        case RADRAY_TEXTURE_CREATE_FLAG_COMMITTED: return "Committed";
    }
}

std::string_view format_as(RadrayShaderStage v) noexcept {
    switch (v) {
        case RADRAY_SHADER_STAGE_VERTEX: return "Vertex";
        case RADRAY_SHADER_STAGE_HULL: return "Hull";
        case RADRAY_SHADER_STAGE_DOMAIN: return "Domain";
        case RADRAY_SHADER_STAGE_GEOMETRY: return "Geometry";
        case RADRAY_SHADER_STAGE_PIXEL: return "Pixel";
        case RADRAY_SHADER_STAGE_COMPUTE: return "Compute";
        case RADRAY_SHADER_STAGE_RAYTRACING: return "RayTracing";
        case RADRAY_SHADER_STAGE_ALL_GRAPHICS: return "AllGraphics";
        case RADRAY_SHADER_STAGE_UNKNOWN: return "UNKNOWN";
    }
}

std::string_view format_as(RadrayVertexSemantic v) noexcept {
    switch (v) {
        case RADRAY_VERTEX_SEMANTIC_POSITION: return "POSITION";
        case RADRAY_VERTEX_SEMANTIC_NORMAL: return "NORMAL";
        case RADRAY_VERTEX_SEMANTIC_TEXCOORD: return "TEXCOORD";
        case RADRAY_VERTEX_SEMANTIC_TANGENT: return "TANGENT";
        case RADRAY_VERTEX_SEMANTIC_COLOR: return "COLOR";
        case RADRAY_VERTEX_SEMANTIC_PSIZE: return "PSIZE";
        case RADRAY_VERTEX_SEMANTIC_BINORMAL: return "BINORMAL";
        case RADRAY_VERTEX_SEMANTIC_BLENDINDICES: return "BLENDINDICES";
        case RADRAY_VERTEX_SEMANTIC_BLENDWEIGHT: return "BLENDWEIGHT";
        case RADRAY_VERTEX_SEMANTIC_POSITIONT: return "POSITIONT";
    }
}
