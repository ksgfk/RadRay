#pragma once

#include <radray/render/common.h>

namespace radray::render {

class ShaderResource {
public:
    radray::string Name;
    ShaderResourceType Type;
    TextureDimension Dim;
    uint32_t Space;
    uint32_t BindPoint;
    uint32_t BindCount;
    ShaderStages Stages;
};

class ShaderResourcesDescriptor {
public:
    struct StaticSamplerDescriptor : public SamplerDescriptor {
        size_t Index;
    };

    radray::vector<ShaderResource> BindResources;
    radray::vector<StaticSamplerDescriptor> StaticSamplers;
};

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

public:
    radray::string Name;
    radray::string EntryPoint;
    ShaderStage Stage;
};

}  // namespace radray::render
