#pragma once

// runtime 所有权契约见 docs/architecture/runtime.md。

#include <radray/render/rhi.h>
#include <radray/render/external_resource.h>

namespace radray {

class Environment;

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

    // 成功时转交完整对象；失败时记录日志并返回空所有者。
    // environment 必须晚于 runtime 销毁；其工厂或实例仅被借用。
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
