#pragma once

#include <span>
#include <string_view>
#include <optional>

#include <radray/render/common.h>
#include <radray/render/shader/spirv.h>
#include <radray/render/shader/msl.h>

namespace radray::render {

struct SpirvBytecodeView {
    std::span<const byte> Data;
    std::string_view EntryPointName;
    ShaderStage Stage{ShaderStage::UNKNOWN};
};

struct SpirvToMslOption {
    uint32_t MslMajor{2};
    uint32_t MslMinor{0};
    uint32_t MslPatch{0};
    MslPlatform Platform{MslPlatform::MacOS};
    bool UseArgumentBuffers{false};
    bool ForceNativeArrays{false};
    std::optional<uint32_t> VertexStageBufferStartIndex{};
    std::optional<uint32_t> FragmentStageBufferStartIndex{};
    std::optional<uint32_t> PushConstantBufferIndex{};
};

struct SpirvToMslOutput {
    string MslSource;
    string EntryPointName;

    inline std::span<const byte> GetBlob() const noexcept {
        return std::as_bytes(std::span{MslSource.data(), MslSource.size()});
    }
};

struct SpirvAsMslReflectParams {
    std::span<const byte> SpirV;
    std::string_view EntryPoint;
    ShaderStage Stage;
    bool UseArgumentBuffers{false};
    std::optional<uint32_t> VertexStageBufferStartIndex{};
    std::optional<uint32_t> FragmentStageBufferStartIndex{};
    std::optional<uint32_t> PushConstantBufferIndex{};
};

}  // namespace radray::render

#ifdef RADRAY_ENABLE_SPIRV_CROSS

namespace radray::render {

std::optional<SpirvShaderDesc> ReflectSpirv(SpirvBytecodeView bytecode);

std::optional<SpirvToMslOutput> ConvertSpirvToMsl(
    std::span<const byte> spirvData,
    std::string_view entryPoint,
    ShaderStage stage,
    const SpirvToMslOption& option = {});

std::optional<MslShaderReflection> ReflectSpirvAsMsl(std::span<const SpirvAsMslReflectParams> msls);

}  // namespace radray::render

#endif
