#include "vulkan_shader_module.h"

#include "vulkan_device.h"

namespace radray::render::vulkan {

static void DestroyShaderModuleVulkan(ShaderModuleVulkan& shaderModule) noexcept {
    if (shaderModule.IsValid()) {
        shaderModule._device->CallVk(&FTbVk::vkDestroyShaderModule, shaderModule._shaderModule, shaderModule._device->GetAllocationCallbacks());
        shaderModule._shaderModule = VK_NULL_HANDLE;
    }
}

ShaderModuleVulkan::ShaderModuleVulkan(
    DeviceVulkan* device,
    VkShaderModule shaderModule,
    radray::string name,
    radray::string entryPoint,
    ShaderStage stage,
    ShaderBlobCategory category) noexcept
    : _device(device),
      _shaderModule(shaderModule) {
    this->Name = std::move(name);
    this->EntryPoint = std::move(entryPoint);
    this->Stage = stage;
    this->Category = category;
}

ShaderModuleVulkan::~ShaderModuleVulkan() noexcept {
    DestroyShaderModuleVulkan(*this);
}

bool ShaderModuleVulkan::IsValid() const noexcept {
    return _shaderModule != VK_NULL_HANDLE;
}

void ShaderModuleVulkan::Destroy() noexcept {
    DestroyShaderModuleVulkan(*this);
}

}  // namespace radray::render::vulkan
