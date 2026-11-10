#pragma once

// 环境与设备生命周期见 docs/architecture/runtime.md。

#include <string_view>
#include <thread>

#include <radray/render/rhi.h>

namespace radray {

class Environment final {
public:
    ~Environment() noexcept;
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    Environment(Environment&&) = delete;
    Environment& operator=(Environment&&) = delete;

    // 失败时记录日志并返回空所有者；不创建 GPU 设备。
    [[nodiscard]] static Nullable<unique_ptr<Environment>> Create(
        render::RenderBackend backend, bool validation,
        std::string_view appName = "RadRay") noexcept;

    render::RenderBackend GetBackend() const noexcept { return _backend; }
    bool IsValidationEnabled() const noexcept { return _validation; }
    bool IsOwnerThread() const noexcept;

private:
    friend class DeviceContext;
    // 仅可调用已选后端的访问器；返回的借用在环境销毁时失效。
    render::DXGIFactory& GetFactory() noexcept { return *_factory; }
    render::InstanceVulkan& GetVulkanInstance() noexcept { return *_instance; }

private:
    Environment(
        render::RenderBackend backend, bool validation,
        unique_ptr<render::DXGIFactory> factory,
        Nullable<render::InstanceVulkan*> instance) noexcept;

    render::RenderBackend _backend;
    std::thread::id _owner;
    unique_ptr<render::DXGIFactory> _factory;
    Nullable<render::InstanceVulkan*> _instance;
    bool _validation;
};

}  // namespace radray
