#pragma once

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray::gpu {

Nullable<shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc, const RhiValidationOptions& validation = {});
Nullable<InstanceVulkan*> InitVulkanEnv(const VulkanInstanceDescriptor& desc);
void ShutdownVulkanEnv() noexcept;
Nullable<shared_ptr<DXGIDiagnosticContext>> CreateDXGIDiagnosticContext();
Nullable<unique_ptr<DXGIFactory>> CreateDXGIFactory(const DXGIFactoryDescriptor& desc);

}  // namespace radray::gpu
