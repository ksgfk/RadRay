#pragma once

#include <type_traits>
#include <span>

#include <Metal/Metal.hpp>
#define IR_RUNTIME_METALCPP
#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

#include <radray/types.h>
#include <radray/platform.h>

namespace radray::rhi::metal {

class IrConverter {
public:
    IrConverter();
    ~IrConverter() noexcept;

    radray::vector<uint8_t> DxilToMetallib(std::span<const uint8_t> dxil) const;

private:
    DynamicLibrary _lib;
    std::add_pointer_t<decltype(IRCompilerCreate)> CompilerCreate;
    std::add_pointer_t<decltype(IRCompilerDestroy)> CompilerDestroy;

    IRCompiler* _compiler;
};

}  // namespace radray::rhi::metal
