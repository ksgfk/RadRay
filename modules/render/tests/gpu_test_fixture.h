#pragma once

#include <cstring>
#include <optional>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <gtest/gtest.h>

#include <radray/render/rhi.h>
#include <radray/logger.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#endif
#if defined(RADRAY_ENABLE_VULKAN)
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render::test {

enum class DeviceSetupStatus : uint8_t { NotAttempted,
                                         Ready,
                                         BackendNotBuilt,
                                         NoAdapter,
                                         ValidationUnavailable,
                                         InitializationFailed };
enum class DeviceSetupStage : uint8_t { Backend,
                                        Factory,
                                        Instance,
                                        Adapter,
                                        Validation,
                                        Device,
                                        Queue,
                                        Complete };

inline std::string_view BackendName(RenderBackend backend) noexcept {
    if (backend == RenderBackend::D3D12) return "d3d12";
    if (backend == RenderBackend::Vulkan) return "vulkan";
    return "unknown";
}

inline bool ContainsBackend(std::string_view list, RenderBackend backend) noexcept {
    while (!list.empty()) {
        const size_t end = list.find(',');
        auto token = list.substr(0, end);
        while (!token.empty() && token.front() == ' ') token.remove_prefix(1);
        while (!token.empty() && token.back() == ' ') token.remove_suffix(1);
        const auto expected = BackendName(backend);
        if (token.size() == expected.size() && std::equal(token.begin(), token.end(), expected.begin(), [](char a, char b) {
                return (a >= 'A' && a <= 'Z' ? char(a - 'A' + 'a') : a) == b;
            })) return true;
        if (end == std::string_view::npos) break;
        list.remove_prefix(end + 1);
    }
    return false;
}

inline bool RequiredBackend(RenderBackend backend) noexcept {
    const char* value = std::getenv("RADRAY_TEST_REQUIRED_BACKENDS");
    return value && ContainsBackend(value, backend);
}

inline bool SetupMustFail(DeviceSetupStatus status, bool required) noexcept {
    return status == DeviceSetupStatus::InitializationFailed ||
           (required && status != DeviceSetupStatus::Ready);
}

struct DeviceContext {
    std::atomic<uint32_t> ValidationErrors{0};
    bool VulkanEnvInitialized{false};
    Nullable<void (*)() noexcept> ShutdownEnvironment{nullptr};
    unique_ptr<DXGIFactory> Factory;
    shared_ptr<Device> Device;
    CommandQueue* Queue{nullptr};
    DeviceSetupStatus Status{DeviceSetupStatus::NotAttempted};
    DeviceSetupStage Stage{DeviceSetupStage::Backend};
    string Reason;
    string AdapterName;
    bool ValidationEnabled{false}, SynchronizationValidationEnabled{false}, GpuValidationEnabled{false};

    void Reset() noexcept {
        if (Queue && Device) Queue->Wait();
        Queue = nullptr;
        Device.reset();
        Factory.reset();
        if (VulkanEnvInitialized) {
            if (ShutdownEnvironment) ShutdownEnvironment.Get()();
            VulkanEnvInitialized = false;
            ShutdownEnvironment = nullptr;
        }
        if (Status == DeviceSetupStatus::Ready) testing::Test::RecordProperty("observed_validation_errors", ValidationErrors.load());
    }
    ~DeviceContext() { Reset(); }
};

inline bool RejectDeviceSetup(DeviceContext& context, RenderBackend backend, DeviceSetupStatus status,
                              DeviceSetupStage stage, std::string_view reason, bool reportFailure = true) {
    context.Reset();
    context.Status = status;
    context.Stage = stage;
    context.Reason = reason;
    testing::Test::RecordProperty("setup_status", static_cast<int>(status));
    testing::Test::RecordProperty("setup_stage", static_cast<int>(stage));
    if (reportFailure && SetupMustFail(status, RequiredBackend(backend))) {
        ADD_FAILURE() << BackendName(backend) << " setup failed at stage " << static_cast<uint32_t>(stage) << ": " << reason;
    }
    return false;
}

inline void CaptureValidationMessage(LogLevel level, std::string_view message, void* userData) {
    if (level == LogLevel::Err || level == LogLevel::Critical) {
        static_cast<DeviceContext*>(userData)->ValidationErrors.fetch_add(1, std::memory_order_relaxed);
        RADRAY_ERR_LOG("GPU validation: {}", message);
    }
}

inline bool TryCreateDevice(
    RenderBackend backend,
    DeviceContext& context,
    bool enableValidation = false) {
    context.Reset();
    context.Status = DeviceSetupStatus::NotAttempted;
    context.ValidationEnabled = context.SynchronizationValidationEnabled = context.GpuValidationEnabled = false;
    context.Reason.clear();
    context.AdapterName.clear();
    const char* gpuValidation = std::getenv("RADRAY_TEST_GPU_VALIDATION");
    const bool enableGpuValidation = enableValidation && gpuValidation && std::string_view{gpuValidation} == "1";
    testing::Test::RecordProperty("backend", string{BackendName(backend)});
    if (backend == RenderBackend::D3D12) {
#if defined(RADRAY_ENABLE_D3D12)
        if (enableValidation) {
            d3d12::ComPtr<ID3D12Debug> debug;
            if (FAILED(::D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))
                return RejectDeviceSetup(context, backend, DeviceSetupStatus::ValidationUnavailable, DeviceSetupStage::Validation, "D3D12 debug layer is unavailable");
            if (enableGpuValidation) {
                d3d12::ComPtr<ID3D12Debug1> debug1;
                if (FAILED(debug.As(&debug1)))
                    return RejectDeviceSetup(context, backend, DeviceSetupStatus::ValidationUnavailable, DeviceSetupStage::Validation, "D3D12 GPU validation is unavailable");
            }
        }
        DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = enableValidation;
        factoryDesc.IsEnableGpuBasedValid = enableGpuValidation;
        factoryDesc.LogCallback = CaptureValidationMessage;
        factoryDesc.LogUserData = &context;
        auto factory = DXGIFactory::Create(factoryDesc);
        if (!factory.HasValue()) {
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Factory, "DXGI factory creation failed");
        }
        context.Factory = factory.Release();
        const auto adapters = context.Factory->GetAdapters();
        const auto selected = context.Factory->SelectHighPerformanceAdapter();
        if (!selected)
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::NoAdapter, DeviceSetupStage::Adapter, "No D3D12 adapter is available");
        for (const auto& adapter : adapters)
            if (adapter.Index == *selected) context.AdapterName = adapter.Name;

        D3D12DeviceDescriptor deviceDesc{};
        deviceDesc.Factory = context.Factory.get();
        deviceDesc.AdapterIndex = selected;
        auto device = Device::Create(DeviceDescriptor{deviceDesc});
        if (!device.HasValue()) {
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Device, "D3D12 device creation failed for an available adapter");
        }
        context.Device = device.Release();
#else
        return RejectDeviceSetup(context, backend, DeviceSetupStatus::BackendNotBuilt, DeviceSetupStage::Backend, "D3D12 backend was not built");
#endif
    } else if (backend == RenderBackend::Vulkan) {
#if defined(RADRAY_ENABLE_VULKAN)
        VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "radray_render_test";
        instanceDesc.EngineName = "radray";
        instanceDesc.IsEnableDebugLayer = enableValidation;
        instanceDesc.IsEnableSynchronizationValidation = enableValidation;
        instanceDesc.IsEnableGpuBasedValid = enableGpuValidation;
        instanceDesc.LogCallback = CaptureValidationMessage;
        instanceDesc.LogUserData = &context;
        auto instance = InstanceVulkan::InitEnv(instanceDesc);
        if (!instance) {
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Instance, "Vulkan instance initialization failed");
        }
        context.ShutdownEnvironment = InstanceVulkan::ShutdownEnv;
        context.VulkanEnvInitialized = true;
        const auto* native = static_cast<const vulkan::InstanceVulkanImpl*>(instance.Get());
        if (enableValidation && (std::find(native->_layers.begin(), native->_layers.end(), "VK_LAYER_KHRONOS_validation") == native->_layers.end() ||
                                 native->_debugMessenger == VK_NULL_HANDLE))
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::ValidationUnavailable, DeviceSetupStage::Validation, "Vulkan validation layer or debug messenger is unavailable");
        const auto selected = instance->SelectHighPerformancePhysicalDevice();
        if (!selected)
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::NoAdapter, DeviceSetupStage::Adapter, "No Vulkan physical device is available");
        for (const auto& adapter : instance->GetPhysicalDevices())
            if (adapter.Index == *selected) context.AdapterName = adapter.Name;

        const VulkanCommandQueueDescriptor queues[]{
            VulkanCommandQueueDescriptor{QueueType::Direct, 1}};
        VulkanDeviceDescriptor deviceDesc{};
        deviceDesc.PhysicalDeviceIndex = selected;
        deviceDesc.Queues = queues;
        auto device = Device::Create(DeviceDescriptor{deviceDesc});
        if (!device.HasValue()) {
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Device, "Vulkan device creation failed for an available adapter");
        }
        context.Device = device.Release();
#else
        return RejectDeviceSetup(context, backend, DeviceSetupStatus::BackendNotBuilt, DeviceSetupStage::Backend, "Vulkan backend was not built");
#endif
    } else {
        return RejectDeviceSetup(context, backend, DeviceSetupStatus::BackendNotBuilt, DeviceSetupStage::Backend, "Unsupported backend");
    }

    auto queue = context.Device->GetCommandQueue(QueueType::Direct, 0);
    if (!queue.HasValue()) {
        return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Queue, "Direct queue acquisition failed");
    }
    context.Queue = queue.Unwrap();
    context.Status = DeviceSetupStatus::Ready;
    context.Stage = DeviceSetupStage::Complete;
    context.ValidationEnabled = enableValidation;
    context.SynchronizationValidationEnabled = enableValidation && backend == RenderBackend::Vulkan;
    context.GpuValidationEnabled = enableGpuValidation;
    testing::Test::RecordProperty("adapter", context.AdapterName);
    testing::Test::RecordProperty("validation", enableValidation ? "enabled" : "disabled");
    testing::Test::RecordProperty("synchronization_validation", context.SynchronizationValidationEnabled ? "enabled" : "disabled");
    testing::Test::RecordProperty("gpu_validation", enableGpuValidation ? "enabled" : "disabled");
    return true;
}

inline bool TryCreateAnyDevice(DeviceContext& context) {
    if (TryCreateDevice(RenderBackend::D3D12, context)) {
        return true;
    }
    return TryCreateDevice(RenderBackend::Vulkan, context);
}

inline Nullable<unique_ptr<Buffer>> MakeUploadBuffer(
    Device& device,
    std::span<const byte> data,
    BufferUses usage) {
    BufferDescriptor desc{
        .Size = data.size(),
        .Memory = MemoryType::Upload,
        .Usage = usage | BufferUse::MapWrite,
        .Hints = ResourceHint::None};
    auto buffer = device.CreateBuffer(desc);
    if (!buffer.HasValue()) {
        return nullptr;
    }
    unique_ptr<Buffer> result = buffer.Release();
    void* mapped = result->Map(0, data.size());
    if (mapped == nullptr) {
        return nullptr;
    }
    std::memcpy(mapped, data.data(), data.size());
    result->FlushMappedRange(BufferRange{0, data.size()});
    result->Unmap();
    return result;
}

struct RenderTarget {
    unique_ptr<Texture> Tex;
    unique_ptr<TextureView> View;
};

inline std::optional<RenderTarget> MakeRenderTarget(
    Device* device,
    TextureFormat format,
    uint32_t width,
    uint32_t height,
    TextureUses usage = TextureUse::RenderTarget) {
    TextureDescriptor textureDesc{
        .Dim = TextureDimension::Dim2D,
        .Width = width,
        .Height = height,
        .DepthOrArraySize = 1,
        .MipLevels = 1,
        .SampleCount = 1,
        .Format = format,
        .Memory = MemoryType::Device,
        .Usage = usage,
        .Hints = ResourceHint::None};
    auto texture = device->CreateTexture(textureDesc);
    if (!texture.HasValue()) {
        return std::nullopt;
    }

    RenderTarget target{};
    target.Tex = texture.Release();
    TextureViewDescriptor viewDesc{
        .Target = target.Tex.get(),
        .Dim = TextureDimension::Dim2D,
        .Format = format,
        .Range = SubresourceRange{0, 1, 0, 1},
        .Usage = TextureViewUsage::RenderTarget};
    auto view = device->CreateTextureView(viewDesc);
    if (!view.HasValue()) {
        return std::nullopt;
    }
    target.View = view.Release();
    return target;
}

}  // namespace radray::render::test
