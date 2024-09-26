#pragma once

#if defined(_WIN32)
#ifndef RADRAYSC_DLL_EXPORT
#define RADRAYSC_DLL_EXPORT __declspec(dllexport)
#endif
#else
#ifndef RADRAYSC_DLL_EXPORT
#define RADRAYSC_DLL_EXPORT __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>
#define RADRAYSC_NOEXCEPT noexcept
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RADRAYSC_NOEXCEPT
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct ID3D12ShaderReflection;

typedef enum RadrayShaderCompilerType {
    RADRAY_SHADER_COMPILER_DXC,
    RADRAY_SHADER_COMPILER_MSC,
    RADRAY_SHADER_COMPILER_SPIRV_CROSS
} RadrayShaderCompilerType;

typedef enum RadrayShaderCompilerLogLevel {
    RADRAY_SHADER_COMPILER_LOG_DEBUG,
    RADRAY_SHADER_COMPILER_LOG_INFO,
    RADRAY_SHADER_COMPILER_LOG_ERROR
} RadrayShaderCompilerLogLevel;

typedef enum RadrayShaderCompilerMetalStage {
    RADRAY_SHADER_COMPILER_MTL_STAGE_INVALID,
    RADRAY_SHADER_COMPILER_MTL_STAGE_VERTEX,
    RADRAY_SHADER_COMPILER_MTL_STAGE_PIXEL,
    RADRAY_SHADER_COMPILER_MTL_STAGE_HULL,
    RADRAY_SHADER_COMPILER_MTL_STAGE_DOMAIN,
    RADRAY_SHADER_COMPILER_MTL_STAGE_MESH,
    RADRAY_SHADER_COMPILER_MTL_STAGE_AMPLIFICATION,
    RADRAY_SHADER_COMPILER_MTL_STAGE_GEOMETRY,
    RADRAY_SHADER_COMPILER_MTL_STAGE_COMPUTE,
    RADRAY_SHADER_COMPILER_MTL_STAGE_CLOSESTHIT,
    RADRAY_SHADER_COMPILER_MTL_STAGE_INTERSECTION,
    RADRAY_SHADER_COMPILER_MTL_STAGE_ANYHIT,
    RADRAY_SHADER_COMPILER_MTL_STAGE_MISS,
    RADRAY_SHADER_COMPILER_MTL_STAGE_RAYGENERATION,
    RADRAY_SHADER_COMPILER_MTL_STAGE_CALLABLE,
    RADRAY_SHADER_COMPILER_MTL_STAGE_STREAMOUT,
    RADRAY_SHADER_COMPILER_MTL_STAGE_STAGEIN
} RadrayShaderCompilerStage;

typedef void (*RadrayShaderCompilerLogFunc)(RadrayShaderCompilerLogLevel level, const char* str, size_t length, void* userPtr) RADRAYSC_NOEXCEPT;

typedef struct RadrayShaderCompilerCreateDescriptor {
    RadrayShaderCompilerLogFunc Log;
    void* UserPtr;
} RadrayShaderCompilerCreateDescriptor;

typedef struct RadrayCompilerError {
    const char* Str;
    size_t StrSize;
} RadrayCompilerError;

typedef struct RadrayCompilerBlob {
    const uint8_t* Data;
    size_t DataSize;
} RadrayCompilerBlob;

struct RadrayShaderCompiler;

typedef bool (*RadrayIsCompilerAvailableFunc)(RadrayShaderCompiler* this_, RadrayShaderCompilerType type) RADRAYSC_NOEXCEPT;

typedef void (*RadrayDestroyCompilerErrorFunc)(RadrayShaderCompiler* this_, RadrayCompilerError* error) RADRAYSC_NOEXCEPT;

typedef void (*RadrayDestroyCompilerBlobFunc)(RadrayShaderCompiler* this_, RadrayCompilerBlob* error) RADRAYSC_NOEXCEPT;

typedef bool (*RadrayDxcCompileHlslToDxilFunc)(
    RadrayShaderCompiler* this_,
    const char* hlslCode, size_t codeSize,
    const char* const* args, size_t argCount,
    RadrayCompilerBlob* dxil, RadrayCompilerBlob* refl,
    RadrayCompilerError* error) RADRAYSC_NOEXCEPT;

typedef bool (*RadrayMscConvertDxilToMetallibFunc)(
    RadrayShaderCompiler* this_,
    const uint8_t* dxilCode, size_t codeSize,
    RadrayShaderCompilerMetalStage stage,
    RadrayCompilerBlob* metallib,
    RadrayCompilerError* error) RADRAYSC_NOEXCEPT;

typedef bool (*RadrayDxcCreateD3D12ShaderReflectionFunc)(
    RadrayShaderCompiler* this_,
    const uint8_t* dxilCode, size_t codeSize,
    ID3D12ShaderReflection** result,
    RadrayCompilerError* error) RADRAYSC_NOEXCEPT;

typedef struct RadrayShaderCompiler {
    RadrayIsCompilerAvailableFunc IsAvailable;
    RadrayDestroyCompilerErrorFunc DestroyError;
    RadrayDestroyCompilerBlobFunc DestroyBlob;
    RadrayDxcCompileHlslToDxilFunc CompileHlslToDxil;
    RadrayMscConvertDxilToMetallibFunc ConvertDxilToMetallib;
    RadrayDxcCreateD3D12ShaderReflectionFunc CreateD3D12Reflection;
} RadrayShaderCompiler;

RADRAYSC_DLL_EXPORT RadrayShaderCompiler* RadrayCreateShaderCompiler(const RadrayShaderCompilerCreateDescriptor* desc) RADRAYSC_NOEXCEPT;
RADRAYSC_DLL_EXPORT void RadrayReleaseShaderCompiler(RadrayShaderCompiler* sc) RADRAYSC_NOEXCEPT;

#ifdef __cplusplus
}
#endif
