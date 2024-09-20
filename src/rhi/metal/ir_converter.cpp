#include "ir_converter.h"

#include <stdexcept>

#include <radray/logger.h>

namespace radray::rhi::metal {

IrConverter::IrConverter() : _lib("metalirconverter") {
    if (!_lib.IsValid()) {
#ifdef RADRAY_ENABLE_MSC
        RADRAY_THROW(std::runtime_error, "cannot load metalirconverter");
#else
        return;
#endif
    }
    CompilerCreate = _lib.GetFunction<decltype(IRCompilerCreate)>("IRCompilerCreate");
    CompilerDestroy = _lib.GetFunction<decltype(IRCompilerDestroy)>("IRCompilerDestroy");

    _compiler = CompilerCreate();
}

radray::vector<uint8_t> IrConverter::DxilToMetallib(std::span<const uint8_t> dxil) const {
    // TODO:
    RADRAY_THROW(std::runtime_error, "no impl");
}

IrConverter::~IrConverter() noexcept {
    if (_compiler != nullptr) {
        CompilerDestroy(_compiler);
        _compiler = nullptr;
    }
}

}  // namespace radray::rhi::metal
