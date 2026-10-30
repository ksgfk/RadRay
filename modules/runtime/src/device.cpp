#include <radray/runtime/device.h>

#include <radray/logger.h>
#include <radray/runtime/environment.h>

namespace radray {

namespace {
template <class Infos, class CreateDevice>
uint32_t MatchHost(
    shared_ptr<render::Device>& device,
    const Infos& infos,
    uint32_t preferred,
    CreateDevice create,
    const std::optional<render::ExternalDeviceIdentity>& host) {
    if (!host) return preferred;
    const auto matches = [&](render::Device& candidate) {
        auto extension = candidate.GetExternalResourceExtension();
        if (!extension) return false;
        auto identity = extension->GetDeviceIdentity();
        auto* value = std::get_if<render::ExternalDeviceIdentity>(&identity);
        return value && *value == *host;
    };
    if (matches(*device)) return preferred;
    for (const auto& info : infos) {
        if (info.Index == preferred) continue;
        auto candidate = create(info.Index);
        if (candidate && matches(*candidate.Get())) {
            device = candidate.Release();
            return info.Index;
        }
    }
    RADRAY_WARN_LOG("No external-resource adapter matches the requested identity; using the preferred device");
    return preferred;
}
}  // namespace

DeviceContext::DeviceContext(
    shared_ptr<render::Device> device,
    render::CommandQueue* directQueue) noexcept
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
    Environment& environment,
    const std::optional<render::ExternalDeviceIdentity>& host) noexcept {
    const auto backend = environment.GetBackend();
    shared_ptr<render::Device> device;
    RADRAY_INFO_LOG("DeviceContext: creating {}", backend);

    switch (backend) {
#if defined(RADRAY_ENABLE_D3D12)
        case render::RenderBackend::D3D12: {
            auto* factory = &environment.GetFactory();
            auto adapter = factory->SelectHighPerformanceAdapter();
            if (!adapter) {
                RADRAY_ERR_LOG("D3D12: no suitable high-performance adapter");
                return nullptr;
            }
            render::D3D12DeviceDescriptor desc{};
            desc.Factory = factory;
            desc.AdapterIndex = adapter;
            desc.QueueCounts = {1, 0, 0};
            desc.EnableExternalResource = true;
            RADRAY_INFO_LOG("DeviceContext: creating D3D12 device on adapter {}", *adapter);
            auto createdDevice = render::Device::Create(render::DeviceDescriptor{desc});
            if (!createdDevice) {
                RADRAY_ERR_LOG("D3D12: device creation failed; see preceding RadRay diagnostics");
                return nullptr;
            }
            device = createdDevice.Release();
            MatchHost(device, factory->GetAdapters(), *adapter, [&](uint32_t index) {
                desc.AdapterIndex = index;
                return render::Device::Create(render::DeviceDescriptor{desc}); }, host);
            break;
        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        case render::RenderBackend::Vulkan: {
            auto* instance = &environment.GetVulkanInstance();
            auto adapter = instance->SelectHighPerformancePhysicalDevice();
            if (!adapter) {
                RADRAY_ERR_LOG("Vulkan: no suitable high-performance physical device");
                return nullptr;
            }
            const render::VulkanCommandQueueDescriptor queues[]{{render::QueueType::Direct, 1}};
            render::VulkanDeviceDescriptor desc{};
            desc.PhysicalDeviceIndex = adapter;
            desc.Queues = queues;
            desc.EnableExternalResource = true;
            RADRAY_INFO_LOG("DeviceContext: creating Vulkan device on adapter {}", *adapter);
            auto createdDevice = render::Device::Create(render::DeviceDescriptor{desc});
            if (!createdDevice) {
                RADRAY_ERR_LOG("Vulkan: device creation failed; see preceding RadRay diagnostics");
                return nullptr;
            }
            device = createdDevice.Release();
            MatchHost(device, instance->GetPhysicalDevices(), *adapter, [&](uint32_t index) {
                desc.PhysicalDeviceIndex = index;
                return render::Device::Create(render::DeviceDescriptor{desc}); }, host);
            break;
        }
#endif
        default:
            RADRAY_ERR_LOG("Unsupported RHI backend");
            return nullptr;
    }

    auto directQueue = device->GetCommandQueue(render::QueueType::Direct, 0);
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
                    render::RhiValidationOptions{}.IsEnabled, environment.IsValidationEnabled());
    return runtime;
}

}  // namespace radray
