#pragma once

#include <vector>
#include <radray/d3d12/utility.h>

namespace radray::d3d12 {

enum class ShaderVariableType {
    ConstantBuffer,
    StructuredBuffer,
    RWStructuredBuffer,
    CBVBufferHeap,
    SRVBufferHeap,
    UAVBufferHeap,
    SamplerHeap,
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

class RasterShader : public Shader {
public:
    ~RasterShader() noexcept override = default;

public:
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<uint8> vertBinary;
    std::vector<uint8> pixelBinary;
};

}  // namespace radray::d3d12
