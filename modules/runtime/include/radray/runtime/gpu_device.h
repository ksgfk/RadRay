#pragma once

#include <radray/render/rhi.h>
#include <radray/render/external_resource.h>

namespace radray {

class Environment;

// See docs/architecture/runtime.md for the runtime ownership contract.
class DeviceContext final {
public:
    DeviceContext(
        shared_ptr<render::Device> device,
        render::CommandQueue* directQueue) noexcept;
    ~DeviceContext() noexcept;

    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;
    DeviceContext(DeviceContext&&) = delete;
    DeviceContext& operator=(DeviceContext&&) = delete;

    // Success transfers a complete object; failure logs and returns an empty owner.
    // environment must outlive the runtime; its factory/instance is only borrowed.
    [[nodiscard]] static Nullable<unique_ptr<DeviceContext>> Create(
        Environment& environment,
        const std::optional<render::ExternalDeviceIdentity>& host = {}) noexcept;

    render::Device& GetDevice() noexcept { return *_device; }
    render::CommandQueue& GetQueue() noexcept { return *_directQueue; }
    bool SupportsIndirect() const noexcept;

private:
    shared_ptr<render::Device> _device;
    render::CommandQueue* _directQueue;
};

}  // namespace radray
