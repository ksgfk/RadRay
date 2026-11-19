#include <radray/runtime/gpu_env.h>
#include <radray/gpu_impl/factory.h>

#include <radray/logger.h>
#include <radray/scope_guard.h>

namespace radray {

Environment::Environment(
    gpu::GpuBackend backend,
    bool validation,
    unique_ptr<gpu::DXGIFactory> factory,
    Nullable<gpu::InstanceVulkan*> instance) noexcept
    : _backend(backend), _owner(std::this_thread::get_id()),
      _factory(std::move(factory)), _instance(instance), _validation(validation) {}

Environment::~Environment() noexcept {
    _factory.reset();
#if defined(RADRAY_ENABLE_VULKAN)
    if (_instance) gpu::ShutdownVulkanEnv();
#endif
    RADRAY_INFO_LOG("Environment: destroyed");
}

bool Environment::IsOwnerThread() const noexcept {
    return _owner == std::this_thread::get_id();
}

Nullable<unique_ptr<Environment>> Environment::Create(
    gpu::GpuBackend backend, bool validation,
    std::string_view appName) noexcept {
    Nullable<gpu::InstanceVulkan*> instance = nullptr;
    auto cleanup = MakeScopeGuard([&]() noexcept {
#if defined(RADRAY_ENABLE_VULKAN)
        if (instance) gpu::ShutdownVulkanEnv();
#endif
    });
    unique_ptr<gpu::DXGIFactory> factory;
    switch (backend) {
#if defined(RADRAY_ENABLE_D3D12)
        case gpu::GpuBackend::D3D12: {
            RADRAY_INFO_LOG("Environment: creating DXGI factory");
            gpu::DXGIFactoryDescriptor desc{};
            desc.IsEnableDebugLayer = validation;
            if (validation) {
                auto diagnostics = gpu::CreateDXGIDiagnosticContext();
                if (!diagnostics) {
                    RADRAY_ERR_LOG("D3D12: cannot create diagnostic context");
                    return nullptr;
                }
                desc.DiagnosticContext = diagnostics.Release();
            }
            auto created = gpu::CreateDXGIFactory(desc);
            if (!created) {
                RADRAY_ERR_LOG("D3D12: DXGI factory creation failed; see preceding RadRay diagnostics");
                return nullptr;
            }
            factory = created.Release();
            break;
        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        case gpu::GpuBackend::Vulkan: {
            RADRAY_INFO_LOG("Environment: creating Vulkan environment");
            gpu::VulkanInstanceDescriptor desc{};
            desc.AppName = appName;
            desc.EngineName = "RadRay";
            desc.IsEnableDebugLayer = validation;
            desc.IsEnableSynchronizationValidation = validation;
            auto created = gpu::InitVulkanEnv(desc);
            if (!created) {
                RADRAY_ERR_LOG("Vulkan: environment creation failed (loader, instance or active environment)");
                return nullptr;
            }
            instance = created.Get();
            break;
        }
#endif
        default:
            RADRAY_ERR_LOG("Unsupported RHI backend");
            return nullptr;
    }
    unique_ptr<Environment> environment{new (std::nothrow) Environment{
        backend, validation, std::move(factory), instance}};
    if (!environment) {
        RADRAY_ERR_LOG("Cannot allocate Environment");
        return nullptr;
    }
    cleanup.Dismiss();
    RADRAY_INFO_LOG("Environment: ready backend={} driver validation={}", backend, validation);
    return environment;
}

gpu::GpuBackend Environment::GetBackend() const noexcept {
    return _backend;
}

bool Environment::IsValidationEnabled() const noexcept {
    return _validation;
}

gpu::DXGIFactory& Environment::GetFactory() noexcept {
    return *_factory;
}

gpu::InstanceVulkan& Environment::GetVulkanInstance() noexcept {
    return *_instance;
}

}  // namespace radray
