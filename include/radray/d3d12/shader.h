#pragma once

#include <vector>
#include <unordered_map>
#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

class Device;

enum class ShaderVariableType {
    ConstantBuffer,
    StructuredBuffer,
    RWStructuredBuffer,
    SamplerHeap,
    CBVBufferHeap,
    SRVBufferHeap,
    UAVBufferHeap,
    SRVTextureHeap,
    UAVTextureHeap
};

struct ShaderProperty {
    ShaderVariableType type;
    uint32 spaceIndex;
    uint32 registerIndex;
    uint32 arraySize;
};

class Shader {
public:
    Shader(Device* device) noexcept;
    virtual ~Shader() noexcept = default;

public:
    Device* device;
    std::vector<ShaderProperty> properties;
    std::unordered_map<std::string, size_t> nameToIndex;
};

}  // namespace radray::d3d12

template <>
struct fmt::formatter<radray::d3d12::ShaderVariableType> : fmt::formatter<std::string> {
    auto format(radray::d3d12::ShaderVariableType const& val, format_context& ctx) const -> decltype(ctx.out());
};
