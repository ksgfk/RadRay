#pragma once

#include <radray/render/shader.h>

#include "vulkan_helper.h"

namespace radray::render::vulkan {

class ShaderModuleVulkan : public Shader {
public:
    ShaderModuleVulkan(
        DeviceVulkan* device,
        VkShaderModule shaderModule,
        radray::string name,
        radray::string entryPoint,
        ShaderStage stage,
        ShaderBlobCategory category) noexcept;

    ~ShaderModuleVulkan() noexcept;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

public:
    DeviceVulkan* _device;
    VkShaderModule _shaderModule;
};

}  // namespace radray::render::vulkan
