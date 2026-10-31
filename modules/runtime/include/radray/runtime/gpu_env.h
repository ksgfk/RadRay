#pragma once

#include <string_view>
#include <thread>

#include <radray/render/rhi.h>

namespace radray {

// See docs/architecture/runtime.md for environment and device lifetimes.
class Environment final {
public:
    ~Environment() noexcept;
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;
    Environment(Environment&&) = delete;
    Environment& operator=(Environment&&) = delete;

    // Logs and returns an empty owner on failure. Creates no GPU device.
    [[nodiscard]] static Nullable<unique_ptr<Environment>> Create(
        render::RenderBackend backend, bool validation,
        std::string_view appName = "RadRay") noexcept;

    render::RenderBackend GetBackend() const noexcept { return _backend; }
    bool IsValidationEnabled() const noexcept { return _validation; }
    bool IsOwnerThread() const noexcept;

private:
    friend class DeviceContext;
    // Only use the accessor for the selected backend. Borrowed until environment destruction.
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
