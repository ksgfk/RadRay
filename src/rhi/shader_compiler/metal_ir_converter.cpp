#include "metal_ir_converter.h"

#include <Metal/Metal.hpp>
#define IR_RUNTIME_METALCPP
#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

ShaderCompilerImpl::MscImpl::MscImpl(ShaderCompilerImpl* sc) noexcept
    : _sc(sc) {
    _mscLib = sc->_desc.GetLibAddr("metalirconverter");
    if (_mscLib != nullptr) {
        _vtbl[0] = sc->_desc.GetProcAddr(_mscLib, "IRCompilerCreate");
        _vtbl[1] = sc->_desc.GetProcAddr(_mscLib, "IRCompilerDestroy");
        _vtbl[2] = sc->_desc.GetProcAddr(_mscLib, "IRObjectCreateFromDXIL");
        _vtbl[3] = sc->_desc.GetProcAddr(_mscLib, "IRErrorDestroy");
        _vtbl[4] = sc->_desc.GetProcAddr(_mscLib, "IRCompilerAllocCompileAndLink");
        _vtbl[5] = sc->_desc.GetProcAddr(_mscLib, "IRErrorGetCode");
        _vtbl[6] = sc->_desc.GetProcAddr(_mscLib, "IRObjectDestroy");
        _vtbl[7] = sc->_desc.GetProcAddr(_mscLib, "IRMetalLibBinaryCreate");
        _vtbl[8] = sc->_desc.GetProcAddr(_mscLib, "IRMetalLibBinaryDestroy");
        _vtbl[9] = sc->_desc.GetProcAddr(_mscLib, "IRObjectGetMetalLibBinary");
        _vtbl[10] = sc->_desc.GetProcAddr(_mscLib, "IRMetalLibGetBytecodeSize");
        _vtbl[11] = sc->_desc.GetProcAddr(_mscLib, "IRMetalLibGetBytecode");
    }
}

ShaderCompilerImpl::MscImpl::~MscImpl() noexcept {
    if (_compiler != nullptr) {
        // CompilerDestroy(_compiler);
        _compiler = nullptr;
    }
    if (_mscLib != nullptr) {
        _sc->_desc.CloseLib(_mscLib);
        _mscLib = nullptr;
    }
}

// #include "ir_converter.h"

// #include <stdexcept>

// #include <radray/logger.h>
// #include <radray/utility.h>

// namespace radray::rhi::metal {

// IrConverter::IrConverter() : _lib("metalirconverter") {
//     if (!_lib.IsValid()) {
// #ifdef RADRAY_ENABLE_MSC
//         RADRAY_THROW(std::runtime_error, "cannot load metalirconverter");
// #else
//         return;
// #endif
//     }
//     CompilerCreate = _lib.GetFunction<decltype(IRCompilerCreate)>("IRCompilerCreate");
//     CompilerDestroy = _lib.GetFunction<decltype(IRCompilerDestroy)>("IRCompilerDestroy");
//     ObjectCreateFromDXIL = _lib.GetFunction<decltype(IRObjectCreateFromDXIL)>("IRObjectCreateFromDXIL");
//     ErrorDestroy = _lib.GetFunction<decltype(IRErrorDestroy)>("IRErrorDestroy");
//     CompilerAllocCompileAndLink = _lib.GetFunction<decltype(IRCompilerAllocCompileAndLink)>("IRCompilerAllocCompileAndLink");
//     ErrorGetCode = _lib.GetFunction<decltype(IRErrorGetCode)>("IRErrorGetCode");
//     ObjectDestroy = _lib.GetFunction<decltype(IRObjectDestroy)>("IRObjectDestroy");
//     MetalLibBinaryCreate = _lib.GetFunction<decltype(IRMetalLibBinaryCreate)>("IRMetalLibBinaryCreate");
//     MetalLibBinaryDestroy = _lib.GetFunction<decltype(IRMetalLibBinaryDestroy)>("IRMetalLibBinaryDestroy");
//     ObjectGetMetalLibBinary = _lib.GetFunction<decltype(IRObjectGetMetalLibBinary)>("IRObjectGetMetalLibBinary");
//     MetalLibGetBytecodeSize = _lib.GetFunction<decltype(IRMetalLibGetBytecodeSize)>("IRMetalLibGetBytecodeSize");
//     MetalLibGetBytecode = _lib.GetFunction<decltype(IRMetalLibGetBytecode)>("IRMetalLibGetBytecode");

//     _compiler = CompilerCreate();
// }

// ConvertResult IrConverter::DxilToMetallib(std::span<const uint8_t> dxil, RadrayShaderStage stage_) const {
//     IRObject* pDXIL = nullptr;
//     IRError* pError = nullptr;
//     IRObject* pOutIR = nullptr;
//     IRMetalLibBinary* pMetallib = nullptr;

//     auto pDXILGuard = MakeScopeGuard([&]() { if (pDXIL != nullptr) ObjectDestroy(pDXIL); });
//     auto pErrorGuard = MakeScopeGuard([&]() { if (pError != nullptr) ErrorDestroy(pError); });
//     auto pOutIRGuard = MakeScopeGuard([&]() { if (pOutIR != nullptr) ObjectDestroy(pOutIR); });
//     auto pMetallibGuard = MakeScopeGuard([&]() { if (pMetallib != nullptr) MetalLibBinaryDestroy(pMetallib); });

//     pDXIL = ObjectCreateFromDXIL(dxil.data(), dxil.size(), IRBytecodeOwnershipNone);
//     pOutIR = CompilerAllocCompileAndLink(_compiler, nullptr, pDXIL, &pError);
//     if (!pOutIR) {
//         auto code = ErrorGetCode(pError);
//         return radray::format("cannot convert dxil to metal ir, reason={} (code={})", (IRErrorCode)code, code);
//     }
//     pMetallib = MetalLibBinaryCreate();
//     IRShaderStage stage = EnumConvert(stage_);
//     ObjectGetMetalLibBinary(pOutIR, stage, pMetallib);
//     size_t metallibSize = MetalLibGetBytecodeSize(pMetallib);
//     radray::vector<uint8_t> result(metallibSize);
//     MetalLibGetBytecode(pMetallib, result.data());
//     return result;
// }

// IrConverter::~IrConverter() noexcept {
//     if (_compiler != nullptr) {
//         CompilerDestroy(_compiler);
//         _compiler = nullptr;
//     }
// }

// IRShaderStage EnumConvert(RadrayShaderStage v) noexcept {
//     switch (v) {
//         case RADRAY_SHADER_STAGE_UNKNOWN: return IRShaderStageInvalid;
//         case RADRAY_SHADER_STAGE_VERTEX: return IRShaderStageVertex;
//         case RADRAY_SHADER_STAGE_HULL: return IRShaderStageHull;
//         case RADRAY_SHADER_STAGE_DOMAIN: return IRShaderStageDomain;
//         case RADRAY_SHADER_STAGE_GEOMETRY: return IRShaderStageGeometry;
//         case RADRAY_SHADER_STAGE_PIXEL: return IRShaderStageFragment;
//         case RADRAY_SHADER_STAGE_COMPUTE: return IRShaderStageCompute;
//         case RADRAY_SHADER_STAGE_RAYTRACING: return IRShaderStageInvalid;
//         case RADRAY_SHADER_STAGE_ALL_GRAPHICS: return IRShaderStageInvalid;
//     }
// }

// }  // namespace radray::rhi::metal

// std::string_view format_as(IRErrorCode v) noexcept {
//     switch (v) {
//         case IRErrorCodeNoError: return "NoError";
//         case IRErrorCodeShaderRequiresRootSignature: return "ShaderRequiresRootSignature";
//         case IRErrorCodeUnrecognizedRootSignatureDescriptor: return "UnrecognizedRootSignatureDescriptor";
//         case IRErrorCodeUnrecognizedParameterTypeInRootSignature: return "UnrecognizedParameterTypeInRootSignature";
//         case IRErrorCodeResourceNotReferencedByRootSignature: return "ResourceNotReferencedByRootSignature";
//         case IRErrorCodeShaderIncompatibleWithDualSourceBlending: return "ShaderIncompatibleWithDualSourceBlending";
//         case IRErrorCodeUnsupportedWaveSize: return "UnsupportedWaveSize";
//         case IRErrorCodeUnsupportedInstruction: return "UnsupportedInstruction";
//         case IRErrorCodeCompilationError: return "CompilationError";
//         case IRErrorCodeFailedToSynthesizeStageInFunction: return "FailedToSynthesizeStageInFunction";
//         case IRErrorCodeFailedToSynthesizeStreamOutFunction: return "FailedToSynthesizeStreamOutFunction";
//         case IRErrorCodeFailedToSynthesizeIndirectIntersectionFunction: return "FailedToSynthesizeIndirectIntersectionFunction";
//         case IRErrorCodeUnableToVerifyModule: return "UnableToVerifyModule";
//         case IRErrorCodeUnableToLinkModule: return "UnableToLinkModule";
//         case IRErrorCodeUnrecognizedDXILHeader: return "UnrecognizedDXILHeader";
//         case IRErrorCodeInvalidRaytracingAttribute: return "InvalidRaytracingAttribute";
//         case IRErrorCodeUnknown: return "Unknown";
//     }
// }
