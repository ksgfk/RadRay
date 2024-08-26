#pragma once

#include "d3d12_helper.h"

namespace radray::rhi::d3d12 {

class Shader {
public:
    virtual ~Shader() noexcept = default;

    radray::vector<uint8_t> code;
    ComPtr<ID3D12ShaderReflection> refl;
    RadrayShaderStage stage;
};

class RasterShader : public Shader {
public:
    ~RasterShader() noexcept override = default;
};

}  // namespace radray::rhi::d3d12
