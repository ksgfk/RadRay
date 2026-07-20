#include <radray/shader/common.h>

#include <xxhash.h>

namespace radray::shader {

ShaderHash HashShaderBytes(std::span<const byte> data) noexcept {
    const XXH128_hash_t hash = XXH3_128bits(data.data(), data.size());
    return ShaderHash{.Low = hash.low64, .High = hash.high64};
}

std::string_view format_as(ShaderStage value) noexcept {
    switch (value) {
        case ShaderStage::UNKNOWN: return "UNKNOWN";
        case ShaderStage::Vertex: return "Vertex";
        case ShaderStage::Pixel: return "Pixel";
        case ShaderStage::Compute: return "Compute";
        case ShaderStage::RayGen: return "RayGen";
        case ShaderStage::Miss: return "Miss";
        case ShaderStage::ClosestHit: return "ClosestHit";
        case ShaderStage::AnyHit: return "AnyHit";
        case ShaderStage::Intersection: return "Intersection";
        case ShaderStage::Callable: return "Callable";
        case ShaderStage::Graphics: return "Graphics";
        case ShaderStage::RayTracing: return "RayTracing";
    }
    return "UNKNOWN";
}

std::string_view format_as(ShaderBlobCategory value) noexcept {
    switch (value) {
        case ShaderBlobCategory::DXIL: return "DXIL";
        case ShaderBlobCategory::SPIRV: return "SPIR-V";
        case ShaderBlobCategory::MSL: return "MSL";
        case ShaderBlobCategory::METALLIB: return "METALLIB";
    }
    return "UNKNOWN";
}

std::string_view format_as(ShaderTarget value) noexcept {
    switch (value) {
        case ShaderTarget::DXIL: return "DXIL";
        case ShaderTarget::SPIRV: return "SPIR-V";
    }
    return "UNKNOWN";
}

}  // namespace radray::shader
