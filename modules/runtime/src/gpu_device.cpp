#include <radray/runtime/gpu_device.h>
#include <radray/gpu_impl/factory.h>
#include <radray/runtime/rdg.h>

#include <radray/logger.h>
#include <radray/runtime/gpu_env.h>

namespace radray {

RDGDeviceConstraints DeviceContext::GetRDGConstraints() const noexcept {
    const auto detail = _device->GetDetail();
    const auto& limits = _device->GetCapabilities().Limits;
    return {detail.BufferCopyOffsetAlignment, detail.TextureDataPlacementAlignment, detail.TextureDataPitchAlignment,
            detail.CBufferAlignment, limits.CBufferOffsetAlignment, limits.StorageBufferOffsetAlignment,
            limits.TexelBufferOffsetAlignment, limits.MaxBufferSize, limits.MaxUniformBufferRange, limits.MaxStorageBufferRange};
}

DeviceContext::DeviceContext(
    shared_ptr<gpu::Device> device,
    gpu::CommandQueue* directQueue) noexcept
    : _device(std::move(device)),
      _directQueue(directQueue) {}

DeviceContext::~DeviceContext() noexcept {
    RADRAY_INFO_LOG("DeviceContext: waiting for Direct queue");
    _directQueue->Wait();
    _device.reset();
}

bool DeviceContext::SupportsIndirect() const noexcept {
    const auto& cap = _device->GetCapabilities();
    return cap.Features.IndirectDraw && cap.Features.IndirectFirstInstance;
}

Nullable<unique_ptr<DeviceContext>> DeviceContext::Create(
    Environment& environment) noexcept {
    const auto backend = environment.GetBackend();
    shared_ptr<gpu::Device> device;
    RADRAY_INFO_LOG("DeviceContext: creating {}", backend);

    switch (backend) {
#if defined(RADRAY_ENABLE_D3D12)
        case gpu::RenderBackend::D3D12: {
            auto* factory = &environment.GetFactory();
            auto adapter = factory->SelectHighPerformanceAdapter();
            if (!adapter) {
                RADRAY_ERR_LOG("D3D12: no suitable high-performance adapter");
                return nullptr;
            }
            gpu::D3D12DeviceDescriptor desc{};
            desc.Factory = factory;
            desc.AdapterIndex = adapter;
            desc.QueueCounts = {1, 0, 0};
            RADRAY_INFO_LOG("DeviceContext: creating D3D12 device on adapter {}", *adapter);
            auto createdDevice = gpu::CreateDevice(gpu::DeviceDescriptor{desc});
            if (!createdDevice) {
                RADRAY_ERR_LOG("D3D12: device creation failed; see preceding RadRay diagnostics");
                return nullptr;
            }
            device = createdDevice.Release();
            break;
        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        case gpu::RenderBackend::Vulkan: {
            auto* instance = &environment.GetVulkanInstance();
            auto adapter = instance->SelectHighPerformancePhysicalDevice();
            if (!adapter) {
                RADRAY_ERR_LOG("Vulkan: no suitable high-performance physical device");
                return nullptr;
            }
            const gpu::VulkanCommandQueueDescriptor queues[]{{gpu::QueueType::Direct, 1}};
            gpu::VulkanDeviceDescriptor desc{};
            desc.PhysicalDeviceIndex = adapter;
            desc.Queues = queues;
            RADRAY_INFO_LOG("DeviceContext: creating Vulkan device on adapter {}", *adapter);
            auto createdDevice = gpu::CreateDevice(gpu::DeviceDescriptor{desc});
            if (!createdDevice) {
                RADRAY_ERR_LOG("Vulkan: device creation failed; see preceding RadRay diagnostics");
                return nullptr;
            }
            device = createdDevice.Release();
            break;
        }
#endif
        default:
            RADRAY_ERR_LOG("Unsupported RHI backend");
            return nullptr;
    }

    auto directQueue = device->GetCommandQueue(gpu::QueueType::Direct, 0);
    if (!directQueue) {
        RADRAY_ERR_LOG("RHI Direct queue is unavailable");
        return nullptr;
    }
    auto runtime = make_unique<DeviceContext>(std::move(device), directQueue.Get());
    if (!runtime) {
        RADRAY_ERR_LOG("Cannot allocate DeviceContext");
        return nullptr;
    }
    RADRAY_INFO_LOG("DeviceContext: ready backend={} GPU={} Direct queues=1 RHI validation={} driver validation={}",
                    runtime->_device->GetBackend(), runtime->_device->GetCapabilities().Detail.GpuName,
                    gpu::RhiValidationOptions{}.IsEnabled, environment.IsValidationEnabled());
    return runtime;
}

gpu::Device& DeviceContext::GetDevice() noexcept {
    return *_device;
}

gpu::CommandQueue& DeviceContext::GetQueue() noexcept {
    return *_directQueue;
}

}  // namespace radray
