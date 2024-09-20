#pragma once

#include <type_traits>
#include <span>
#include <string_view>
#include <variant>

#include <Metal/Metal.hpp>
#define IR_RUNTIME_METALCPP
#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include <radray/types.h>
#include <radray/platform.h>
#include <radray/rhi/ctypes.h>

namespace radray::rhi::metal {

using ConvertResult = std::variant<radray::vector<uint8_t>, radray::string>;

class IrConverter {
public:
    IrConverter();
    ~IrConverter() noexcept;

    ConvertResult DxilToMetallib(std::span<const uint8_t> dxil, RadrayShaderStage stage) const;

private:
    DynamicLibrary _lib;
    std::add_pointer_t<decltype(IRCompilerCreate)> CompilerCreate;
    std::add_pointer_t<decltype(IRCompilerDestroy)> CompilerDestroy;
    std::add_pointer_t<decltype(IRObjectCreateFromDXIL)> ObjectCreateFromDXIL;
    std::add_pointer_t<decltype(IRErrorDestroy)> ErrorDestroy;
    std::add_pointer_t<decltype(IRCompilerAllocCompileAndLink)> CompilerAllocCompileAndLink;
    std::add_pointer_t<decltype(IRErrorGetCode)> ErrorGetCode;
    std::add_pointer_t<decltype(IRObjectDestroy)> ObjectDestroy;
    std::add_pointer_t<decltype(IRMetalLibBinaryCreate)> MetalLibBinaryCreate;
    std::add_pointer_t<decltype(IRMetalLibBinaryDestroy)> MetalLibBinaryDestroy;
    std::add_pointer_t<decltype(IRObjectGetMetalLibBinary)> ObjectGetMetalLibBinary;
    std::add_pointer_t<decltype(IRMetalLibGetBytecodeSize)> MetalLibGetBytecodeSize;
    std::add_pointer_t<decltype(IRMetalLibGetBytecode)> MetalLibGetBytecode;

    IRCompiler* _compiler;
};

IRShaderStage EnumConvert(RadrayShaderStage v) noexcept;

}  // namespace radray::rhi::metal

std::string_view format_as(IRErrorCode v) noexcept;
