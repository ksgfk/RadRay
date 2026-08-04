#include "shader_reflection_map.h"

#include <algorithm>
#include <limits>

namespace radray {

std::optional<render::ShaderParameterBindingType> MapHlslBindingType(
    const render::HlslInputBindDesc& bind) noexcept {
    switch (bind.Type) {
        case render::HlslShaderInputType::CBUFFER:
            return render::ShaderParameterBindingType::CBuffer;
        case render::HlslShaderInputType::TBUFFER:
            return render::ShaderParameterBindingType::TexelBuffer;
        case render::HlslShaderInputType::SAMPLER:
            return render::ShaderParameterBindingType::Sampler;
        case render::HlslShaderInputType::TEXTURE:
            return render::IsBufferDimension(bind.Dimension)
                       ? render::ShaderParameterBindingType::TexelBuffer
                       : render::ShaderParameterBindingType::Texture;
        case render::HlslShaderInputType::STRUCTURED:
        case render::HlslShaderInputType::BYTEADDRESS:
            return render::ShaderParameterBindingType::Buffer;
        case render::HlslShaderInputType::UAV_RWSTRUCTURED:
        case render::HlslShaderInputType::UAV_RWBYTEADDRESS:
        case render::HlslShaderInputType::UAV_APPEND_STRUCTURED:
        case render::HlslShaderInputType::UAV_CONSUME_STRUCTURED:
        case render::HlslShaderInputType::UAV_RWSTRUCTURED_WITH_COUNTER:
            return render::ShaderParameterBindingType::RWBuffer;
        case render::HlslShaderInputType::UAV_RWTYPED:
            return render::IsBufferDimension(bind.Dimension)
                       ? render::ShaderParameterBindingType::RWTexelBuffer
                       : render::ShaderParameterBindingType::RWTexture;
        default:
            return std::nullopt;
    }
}

std::optional<render::ShaderParameterBindingType> MapSpirvBindingType(
    const render::SpirvResourceBinding& bind) noexcept {
    switch (bind.Kind) {
        case render::SpirvResourceKind::UniformBuffer:
            return render::ShaderParameterBindingType::CBuffer;
        case render::SpirvResourceKind::StorageBuffer:
            return bind.WriteOnly || !bind.ReadOnly
                       ? render::ShaderParameterBindingType::RWBuffer
                       : render::ShaderParameterBindingType::Buffer;
        case render::SpirvResourceKind::SeparateSampler:
            return render::ShaderParameterBindingType::Sampler;
        case render::SpirvResourceKind::SeparateImage:
        case render::SpirvResourceKind::SampledImage:
            return bind.ImageInfo.has_value() && bind.ImageInfo->Dim == render::SpirvImageDim::Buffer
                       ? render::ShaderParameterBindingType::TexelBuffer
                       : render::ShaderParameterBindingType::Texture;
        case render::SpirvResourceKind::StorageImage:
            return bind.ImageInfo.has_value() && bind.ImageInfo->Dim == render::SpirvImageDim::Buffer
                       ? render::ShaderParameterBindingType::RWTexelBuffer
                       : render::ShaderParameterBindingType::RWTexture;
        default:
            return std::nullopt;
    }
}

std::optional<ReflectedBinding> MakeReflectedBinding(
    const render::HlslInputBindDesc& bind) noexcept {
    const std::optional<render::ShaderParameterBindingType> type = MapHlslBindingType(bind);
    if (!type.has_value()) {
        return std::nullopt;
    }
    return ReflectedBinding{
        bind.Name,
        bind.Space,
        bind.BindPoint,
        type.value(),
        bind.IsUnboundArray() ? 0u : bind.BindCount};
}

std::optional<ReflectedBinding> MakeReflectedBinding(
    const render::SpirvResourceBinding& bind) noexcept {
    const std::optional<render::ShaderParameterBindingType> type = MapSpirvBindingType(bind);
    if (!type.has_value()) {
        return std::nullopt;
    }
    return ReflectedBinding{
        bind.Name,
        bind.Set,
        bind.Binding,
        type.value(),
        bind.IsUnboundedArray ? 0u : (bind.ArraySize == 0 ? 1u : bind.ArraySize)};
}

void SplitSemantic(std::string_view raw, std::string_view& baseName, uint32_t& index) noexcept {
    size_t end = raw.size();
    while (end > 0 && raw[end - 1] >= '0' && raw[end - 1] <= '9') {
        --end;
    }
    baseName = raw.substr(0, end);
    index = 0;
    if (end < raw.size()) {
        uint64_t parsed = 0;
        for (size_t i = end; i < raw.size(); ++i) {
            parsed = parsed * 10 + static_cast<uint64_t>(raw[i] - '0');
            if (parsed > std::numeric_limits<uint32_t>::max()) {
                parsed = std::numeric_limits<uint32_t>::max();
                break;
            }
        }
        index = static_cast<uint32_t>(parsed);
    }
}

bool EqualsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = (a[i] >= 'a' && a[i] <= 'z') ? static_cast<char>(a[i] - 32) : a[i];
        const char cb = (b[i] >= 'a' && b[i] <= 'z') ? static_cast<char>(b[i] - 32) : b[i];
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

bool IsSystemSemantic(std::string_view baseName) noexcept {
    return baseName.size() >= 3 &&
           (baseName[0] == 'S' || baseName[0] == 's') &&
           (baseName[1] == 'V' || baseName[1] == 'v') &&
           baseName[2] == '_';
}

uint32_t EffectiveSemanticIndex(std::string_view rawName, uint32_t semanticIndex) noexcept {
    std::string_view baseName{};
    uint32_t nameIndex = 0;
    SplitSemantic(rawName, baseName, nameIndex);
    // DXC 通常已把索引拆到 SemanticIndex; 名字尾部还带数字时取名字里的。
    return nameIndex != 0 ? nameIndex : semanticIndex;
}

string UppercaseAscii(std::string_view value) {
    string result{value};
    std::ranges::transform(result, result.begin(), [](char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    });
    return result;
}

}  // namespace radray
