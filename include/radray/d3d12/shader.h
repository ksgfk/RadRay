#pragma once

#include <vector>
#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

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
    virtual ~Shader() noexcept = default;

public:
    std::vector<ShaderProperty> properties;
};

}  // namespace radray::d3d12
