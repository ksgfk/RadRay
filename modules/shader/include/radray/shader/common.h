#pragma once

#include <compare>
#include <span>
#include <string_view>

#include <radray/enum_flags.h>
#include <radray/types.h>

namespace radray::shader {

enum class ShaderStage : uint32_t {
    UNKNOWN = 0x0,
    Vertex = 0x1,
    Pixel = Vertex << 1,
    Compute = Pixel << 1,
    RayGen = Compute << 1,
    Miss = RayGen << 1,
    ClosestHit = Miss << 1,
    AnyHit = ClosestHit << 1,
    Intersection = AnyHit << 1,
    Callable = Intersection << 1,
    Graphics = Vertex | Pixel,
    RayTracing = RayGen | Miss | ClosestHit | AnyHit | Intersection | Callable,
};

enum class ShaderBlobCategory : int32_t {
    DXIL,
    SPIRV,
    MSL,
    METALLIB,
};

enum class ShaderTarget : uint8_t {
    DXIL,
    SPIRV,
};

struct ShaderHash {
    uint64_t Low{0};
    uint64_t High{0};

    friend bool operator==(const ShaderHash&, const ShaderHash&) noexcept = default;
    friend auto operator<=>(const ShaderHash&, const ShaderHash&) noexcept = default;
};

ShaderHash HashShaderBytes(std::span<const byte> data) noexcept;

std::string_view format_as(ShaderStage value) noexcept;
std::string_view format_as(ShaderBlobCategory value) noexcept;
std::string_view format_as(ShaderTarget value) noexcept;

}  // namespace radray::shader

namespace radray {

template <>
struct is_flags<shader::ShaderStage> : public std::true_type {};
template <>
struct is_compound_enum_flags<shader::ShaderStage> : public std::true_type {};

using ShaderStages = EnumFlags<shader::ShaderStage>;

}  // namespace radray
