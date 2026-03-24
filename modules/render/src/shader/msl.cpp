#include <radray/render/shader/msl.h>

#include <radray/utility.h>

namespace radray::render {

std::string_view format_as(MslDataType v) noexcept {
    switch (v) {
        case MslDataType::None: return "None";
        case MslDataType::Struct: return "Struct";
        case MslDataType::Array: return "Array";
        case MslDataType::Float: return "Float";
        case MslDataType::Float2: return "Float2";
        case MslDataType::Float3: return "Float3";
        case MslDataType::Float4: return "Float4";
        case MslDataType::Float2x2: return "Float2x2";
        case MslDataType::Float2x3: return "Float2x3";
        case MslDataType::Float2x4: return "Float2x4";
        case MslDataType::Float3x2: return "Float3x2";
        case MslDataType::Float3x3: return "Float3x3";
        case MslDataType::Float3x4: return "Float3x4";
        case MslDataType::Float4x2: return "Float4x2";
        case MslDataType::Float4x3: return "Float4x3";
        case MslDataType::Float4x4: return "Float4x4";
        case MslDataType::Half: return "Half";
        case MslDataType::Half2: return "Half2";
        case MslDataType::Half3: return "Half3";
        case MslDataType::Half4: return "Half4";
        case MslDataType::Half2x2: return "Half2x2";
        case MslDataType::Half2x3: return "Half2x3";
        case MslDataType::Half2x4: return "Half2x4";
        case MslDataType::Half3x2: return "Half3x2";
        case MslDataType::Half3x3: return "Half3x3";
        case MslDataType::Half3x4: return "Half3x4";
        case MslDataType::Half4x2: return "Half4x2";
        case MslDataType::Half4x3: return "Half4x3";
        case MslDataType::Half4x4: return "Half4x4";
        case MslDataType::Int: return "Int";
        case MslDataType::Int2: return "Int2";
        case MslDataType::Int3: return "Int3";
        case MslDataType::Int4: return "Int4";
        case MslDataType::UInt: return "UInt";
        case MslDataType::UInt2: return "UInt2";
        case MslDataType::UInt3: return "UInt3";
        case MslDataType::UInt4: return "UInt4";
        case MslDataType::Short: return "Short";
        case MslDataType::Short2: return "Short2";
        case MslDataType::Short3: return "Short3";
        case MslDataType::Short4: return "Short4";
        case MslDataType::UShort: return "UShort";
        case MslDataType::UShort2: return "UShort2";
        case MslDataType::UShort3: return "UShort3";
        case MslDataType::UShort4: return "UShort4";
        case MslDataType::Char: return "Char";
        case MslDataType::Char2: return "Char2";
        case MslDataType::Char3: return "Char3";
        case MslDataType::Char4: return "Char4";
        case MslDataType::UChar: return "UChar";
        case MslDataType::UChar2: return "UChar2";
        case MslDataType::UChar3: return "UChar3";
        case MslDataType::UChar4: return "UChar4";
        case MslDataType::Bool: return "Bool";
        case MslDataType::Bool2: return "Bool2";
        case MslDataType::Bool3: return "Bool3";
        case MslDataType::Bool4: return "Bool4";
        case MslDataType::Long: return "Long";
        case MslDataType::Long2: return "Long2";
        case MslDataType::Long3: return "Long3";
        case MslDataType::Long4: return "Long4";
        case MslDataType::ULong: return "ULong";
        case MslDataType::ULong2: return "ULong2";
        case MslDataType::ULong3: return "ULong3";
        case MslDataType::ULong4: return "ULong4";
        case MslDataType::Texture: return "Texture";
        case MslDataType::Sampler: return "Sampler";
        case MslDataType::Pointer: return "Pointer";
    }
    radray::Unreachable();
}

std::string_view format_as(MslArgumentType v) noexcept {
    switch (v) {
        case MslArgumentType::Buffer: return "Buffer";
        case MslArgumentType::Texture: return "Texture";
        case MslArgumentType::Sampler: return "Sampler";
        case MslArgumentType::ThreadgroupMemory: return "ThreadgroupMemory";
    }
    radray::Unreachable();
}

std::string_view format_as(MslAccess v) noexcept {
    switch (v) {
        case MslAccess::ReadOnly: return "ReadOnly";
        case MslAccess::ReadWrite: return "ReadWrite";
        case MslAccess::WriteOnly: return "WriteOnly";
    }
    radray::Unreachable();
}

std::string_view format_as(MslTextureType v) noexcept {
    switch (v) {
        case MslTextureType::Tex1D: return "Tex1D";
        case MslTextureType::Tex1DArray: return "Tex1DArray";
        case MslTextureType::Tex2D: return "Tex2D";
        case MslTextureType::Tex2DArray: return "Tex2DArray";
        case MslTextureType::Tex2DMS: return "Tex2DMS";
        case MslTextureType::Tex3D: return "Tex3D";
        case MslTextureType::TexCube: return "TexCube";
        case MslTextureType::TexCubeArray: return "TexCubeArray";
        case MslTextureType::TexBuffer: return "TexBuffer";
    }
    radray::Unreachable();
}

std::string_view format_as(MslStage v) noexcept {
    switch (v) {
        case MslStage::Vertex: return "Vertex";
        case MslStage::Fragment: return "Fragment";
        case MslStage::Compute: return "Compute";
    }
    radray::Unreachable();
}

}  // namespace radray::render
