#ifdef RADRAYSC_ENABLE_MSC

#include "metal_ir_converter.h"
#include "shader_compiler_impl.h"

#include <Metal/Metal.hpp>
#define IR_RUNTIME_METALCPP
#include <metal_irconverter/metal_irconverter.h>
#include <metal_irconverter_runtime/metal_irconverter_runtime.h>

static std::string_view format_as(IRErrorCode v) noexcept {
    switch (v) {
        case IRErrorCodeNoError: return "NoError";
        case IRErrorCodeShaderRequiresRootSignature: return "ShaderRequiresRootSignature";
        case IRErrorCodeUnrecognizedRootSignatureDescriptor: return "UnrecognizedRootSignatureDescriptor";
        case IRErrorCodeUnrecognizedParameterTypeInRootSignature: return "UnrecognizedParameterTypeInRootSignature";
        case IRErrorCodeResourceNotReferencedByRootSignature: return "ResourceNotReferencedByRootSignature";
        case IRErrorCodeShaderIncompatibleWithDualSourceBlending: return "ShaderIncompatibleWithDualSourceBlending";
        case IRErrorCodeUnsupportedWaveSize: return "UnsupportedWaveSize";
        case IRErrorCodeUnsupportedInstruction: return "UnsupportedInstruction";
        case IRErrorCodeCompilationError: return "CompilationError";
        case IRErrorCodeFailedToSynthesizeStageInFunction: return "FailedToSynthesizeStageInFunction";
        case IRErrorCodeFailedToSynthesizeStreamOutFunction: return "FailedToSynthesizeStreamOutFunction";
        case IRErrorCodeFailedToSynthesizeIndirectIntersectionFunction: return "FailedToSynthesizeIndirectIntersectionFunction";
        case IRErrorCodeUnableToVerifyModule: return "UnableToVerifyModule";
        case IRErrorCodeUnableToLinkModule: return "UnableToLinkModule";
        case IRErrorCodeUnrecognizedDXILHeader: return "UnrecognizedDXILHeader";
        case IRErrorCodeInvalidRaytracingAttribute: return "InvalidRaytracingAttribute";
        case IRErrorCodeUnknown: return "Unknown";
    }
}

static IRShaderStage EnumConvert(RadrayShaderCompilerMetalStage stage) noexcept {
    switch (stage) {
        case RADRAY_SHADER_COMPILER_MTL_STAGE_VERTEX: return IRShaderStageVertex;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_PIXEL: return IRShaderStageFragment;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_HULL: return IRShaderStageHull;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_DOMAIN: return IRShaderStageDomain;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_MESH: return IRShaderStageMesh;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_AMPLIFICATION: return IRShaderStageAmplification;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_GEOMETRY: return IRShaderStageGeometry;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_COMPUTE: return IRShaderStageCompute;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_CLOSESTHIT: return IRShaderStageClosestHit;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_INTERSECTION: return IRShaderStageIntersection;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_ANYHIT: return IRShaderStageAnyHit;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_MISS: return IRShaderStageMiss;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_RAYGENERATION: return IRShaderStageRayGeneration;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_CALLABLE: return IRShaderStageCallable;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_STREAMOUT: return IRShaderStageStreamOut;
        case RADRAY_SHADER_COMPILER_MTL_STAGE_STAGEIN: return IRShaderStageStageIn;
    }
}

ShaderCompilerImpl::MscImpl::MscImpl(ShaderCompilerImpl* sc) noexcept
    : _sc(sc) {
    _ir = IRCompilerCreate();
}

ShaderCompilerImpl::MscImpl::~MscImpl() noexcept {
    if (_ir != nullptr) {
        IRCompilerDestroy(_ir);
        _ir = nullptr;
    }
}

ConvertResultMetallib ShaderCompilerImpl::MscImpl::DxilToMetallib(
    std::span<const uint8_t> dxil,
    RadrayShaderCompilerMetalStage stage_) const noexcept {
    IRObject* pDXIL = nullptr;
    IRError* pError = nullptr;
    IRObject* pOutIR = nullptr;
    IRMetalLibBinary* pMetallib = nullptr;

    MScopeGuard pDXILGuard([&]() { if (pDXIL != nullptr) IRObjectDestroy(pDXIL); });
    MScopeGuard pErrorGuard([&]() { if (pError != nullptr) IRErrorDestroy(pError); });
    MScopeGuard pOutIRGuard([&]() { if (pOutIR != nullptr) IRObjectDestroy(pOutIR); });
    MScopeGuard pMetallibGuard([&]() { if (pMetallib != nullptr) IRMetalLibBinaryDestroy(pMetallib); });

    pDXIL = IRObjectCreateFromDXIL(dxil.data(), dxil.size(), IRBytecodeOwnershipNone);
    pOutIR = IRCompilerAllocCompileAndLink(_ir, nullptr, pDXIL, &pError);
    if (!pOutIR) {
        auto code = IRErrorGetCode(pError);
        return std::string{"cannot convert dxil to metal ir, reason="} + std::string{format_as((IRErrorCode)code)};
    }
    pMetallib = IRMetalLibBinaryCreate();
    IRShaderStage stage = EnumConvert(stage_);
    IRObjectGetMetalLibBinary(pOutIR, stage, pMetallib);
    size_t metallibSize = IRMetalLibGetBytecodeSize(pMetallib);
    std::vector<uint8_t> result(metallibSize);
    IRMetalLibGetBytecode(pMetallib, result.data());
    return _sc->CreateBlob(result.data(), result.size());
}

#endif
