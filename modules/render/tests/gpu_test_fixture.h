#pragma once

#include <cstring>
#include <optional>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <charconv>
#include <mutex>
#include <chrono>
#include <gtest/gtest.h>

#include <radray/render/rhi.h>
#include <radray/logger.h>

#if defined(RADRAY_ENABLE_D3D12)
#include <radray/render/backend/d3d12_impl.h>
#include "d3d12/d3d12_diagnostics.h"
#include <dxgidebug.h>
#include "rhi_validation.h"
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

#if defined(RADRAY_ENABLE_D3D12)
struct DiagnosticProbe final : d3d12::detail::DiagnosticObserver {
    using Message = d3d12::detail::DiagnosticMessage;
    using Source = d3d12::detail::DiagnosticSource;
    using Failure = d3d12::detail::DiagnosticFailure;

    struct Expectation {
        Source SourceType;
        GUID Producer{};
        int32_t Id{};
        uint32_t Severity{};
        Failure Fault{Failure::None};
        HRESULT Result{S_OK};
        size_t Begin{}, End{SIZE_MAX}, Count{1};
    };

    DiagnosticProbe(std::atomic<uint32_t>& errors, shared_ptr<DXGIDiagnosticContext> context)
        : Context(std::move(context)), Errors(errors) {}
    shared_ptr<DXGIDiagnosticContext> Context;
    std::atomic<uint32_t>& Errors;
    std::atomic<uint32_t> Failures{0}, Warnings{0};
    std::atomic<bool> FlushPassed{true};
    std::mutex Mutex;
    vector<Message> Messages;
    vector<Expectation> Expectations;

    void OnDiagnostic(const Message& message) noexcept override {
        if (message.IsCollectionFailure())
            ++Failures;
        else if (message.Severity <= D3D12_MESSAGE_SEVERITY_ERROR)
            ++Errors;
        else if (message.Severity == D3D12_MESSAGE_SEVERITY_WARNING)
            ++Warnings;
        std::lock_guard lock{Mutex};
        Messages.push_back(message);
    }

    bool Checkpoint() {
        const bool complete = d3d12::detail::FlushDiagnostics(Context);
        if (!complete) FlushPassed = false;
        return complete;
    }

    size_t CheckpointAndMark() {
        EXPECT_TRUE(Checkpoint());
        std::lock_guard lock{Mutex};
        return Messages.size();
    }

    size_t Expect(Expectation expectation) {
        std::lock_guard lock{Mutex};
        Expectations.push_back(expectation);
        return Expectations.size() - 1;
    }

    void EndExpectation(size_t index) {
        Checkpoint();
        std::lock_guard lock{Mutex};
        Expectations.at(index).End = Messages.size();
    }

    uint32_t ExpectedCount(bool failures) {
        std::lock_guard lock{Mutex};
        uint32_t count = 0;
        for (const auto& expected : Expectations) {
            if ((expected.Fault != Failure::None) == failures) count += static_cast<uint32_t>(expected.Count);
        }
        return count;
    }

    bool Verify() {
        std::lock_guard lock{Mutex};
        vector<size_t> counts(Expectations.size(), 0);
        bool passed = true;
        for (size_t index = 0; index < Messages.size(); ++index) {
            const auto& message = Messages[index];
            size_t matches = 0;
            for (size_t e = 0; e < Expectations.size(); ++e) {
                const auto& expected = Expectations[e];
                if (index < expected.Begin || index >= expected.End || message.Source != expected.SourceType ||
                    message.Producer != expected.Producer || message.Id != expected.Id || message.Severity != expected.Severity ||
                    message.Failure != expected.Fault || message.Result != expected.Result) continue;
                ++matches;
                ++counts[e];
            }
            if (matches > 1 || (matches == 0 && (message.IsCollectionFailure() || message.Severity <= D3D12_MESSAGE_SEVERITY_ERROR))) {
                passed = false;
                ADD_FAILURE() << "Unexpected or ambiguous GPU diagnostic at " << index << ": " << message.Description;
            }
        }
        for (size_t index = 0; index < Expectations.size(); ++index) {
            if (counts[index] == Expectations[index].Count) continue;
            passed = false;
            ADD_FAILURE() << "GPU diagnostic expectation " << index << " matched " << counts[index]
                          << " messages, expected " << Expectations[index].Count;
        }
        if (!FlushPassed && Failures == 0) {
            passed = false;
            ADD_FAILURE() << "Diagnostic checkpoint failed without a matching collection failure";
        }
        return passed;
    }

    size_t Count(size_t start, Source source, int32_t id) {
        std::lock_guard lock{Mutex};
        return std::count_if(Messages.begin() + start, Messages.end(), [&](const auto& message) {
            return !message.IsCollectionFailure() && message.Source == source && message.Id == id;
        });
    }

    bool HasMarker(Source source, std::string_view marker) {
        std::lock_guard lock{Mutex};
        return std::ranges::any_of(Messages, [&](const auto& message) {
            return !message.IsCollectionFailure() && message.Source == source && message.Channel != 0 &&
                   message.Severity == D3D12_MESSAGE_SEVERITY_INFO && message.Description.find(marker) != string::npos;
        });
    }

    bool SelfTest(ID3D12InfoQueue* queue) {
        const auto marker = fmt::format("radray-diagnostic-self-test-{}-{}", reinterpret_cast<uintptr_t>(this),
                                        std::chrono::steady_clock::now().time_since_epoch().count());
        d3d12::ComPtr<IDXGIInfoQueue> dxgi;
        if (FAILED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&dxgi))) ||
            FAILED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, marker.c_str())) ||
            FAILED(dxgi->AddApplicationMessage(DXGI_INFO_QUEUE_MESSAGE_SEVERITY_INFO, marker.c_str()))) return false;
        return Checkpoint() && HasMarker(Source::D3D12, marker) && HasMarker(Source::Dxgi, marker);
    }
};
#endif

struct DeviceContext {
    std::atomic<uint32_t> ValidationErrors{0};
    std::atomic<uint32_t> RhiValidationErrors{0};
    bool Finalized{false};
    bool VulkanEnvInitialized{false};
    Nullable<void (*)() noexcept> ShutdownEnvironment{nullptr};
#if defined(RADRAY_ENABLE_D3D12)
    shared_ptr<DXGIDiagnosticContext> DiagnosticContext;
    shared_ptr<DiagnosticProbe> Diagnostics;
    shared_ptr<d3d12::detail::DiagnosticSubscription> DiagnosticSubscription;
    string DiagnosticMode;
    string DiagnosticSelection{"native"};
    string DiagnosticInterfaceResult;
    bool DiagnosticSelfTest{false};
#endif
    unique_ptr<DXGIFactory> Factory;
    shared_ptr<Device> Device;
    CommandQueue* Queue{nullptr};
    DeviceSetupStatus Status{DeviceSetupStatus::NotAttempted};
    DeviceSetupStage Stage{DeviceSetupStage::Backend};
    string Reason;
    string AdapterName;
    bool ValidationEnabled{false}, SynchronizationValidationEnabled{false}, GpuValidationEnabled{false};

    void WaitAllQueues() noexcept {
        if (!Device) return;
        const auto& queues = Device->GetCapabilities().Queues;
        for (size_t type = 0; type < queues.size(); ++type) {
            for (uint32_t slot = 0; slot < queues[type].CreatedCount; ++slot) {
                auto queue = Device->GetCommandQueue(static_cast<QueueType>(type), slot);
                if (!queue) RADRAY_ABORT("Created GPU queue {}:{} is unavailable during test cleanup", type, slot);
                queue->Wait();
            }
        }
    }

    void Reset() noexcept {
        if (Finalized) return;
        Finalized = true;
        WaitAllQueues();
        bool structuredDiagnostics = false;
#if defined(RADRAY_ENABLE_D3D12)
        if (Diagnostics && DiagnosticSubscription) Diagnostics->Checkpoint();
#endif
        Queue = nullptr;
        Device.reset();
        Factory.reset();
#if defined(RADRAY_ENABLE_D3D12)
        if (DiagnosticSubscription) {
            structuredDiagnostics = true;
            Diagnostics->Checkpoint();
            DiagnosticSubscription.reset();
            const bool matched = Diagnostics->Verify();
            testing::Test::RecordProperty("diagnostic_mode", DiagnosticMode);
            testing::Test::RecordProperty("diagnostic_selection", DiagnosticSelection);
            testing::Test::RecordProperty("diagnostic_infoqueue1_hresult", DiagnosticInterfaceResult);
            testing::Test::RecordProperty("diagnostic_d3d12_strategy", DiagnosticMode == "callback" ? "callback" : "checked_clear");
            testing::Test::RecordProperty("diagnostic_dxgi_strategy", "checked_clear");
            testing::Test::RecordProperty("diagnostic_flush", Diagnostics->FlushPassed ? "passed" : "failed");
            testing::Test::RecordProperty("diagnostic_self_test", DiagnosticSelfTest ? "passed" : "failed");
            testing::Test::RecordProperty("diagnostic_health", Diagnostics->Failures == 0 ? "complete" : "incomplete");
            testing::Test::RecordProperty("diagnostic_collection_failures", Diagnostics->Failures.load());
            testing::Test::RecordProperty("diagnostic_expectations", matched ? "passed" : "failed");
            testing::Test::RecordProperty("diagnostic_finalization", matched ? "passed" : "failed");
            testing::Test::RecordProperty("observed_validation_warnings", Diagnostics->Warnings.load());
            testing::Test::RecordProperty("expected_validation_errors", Diagnostics->ExpectedCount(false));
            testing::Test::RecordProperty("expected_collection_failures", Diagnostics->ExpectedCount(true));
            Diagnostics.reset();
        }
        DiagnosticContext.reset();
#endif
        if (VulkanEnvInitialized) {
            if (ShutdownEnvironment) ShutdownEnvironment.Get()();
            VulkanEnvInitialized = false;
            ShutdownEnvironment = nullptr;
        }
        if (ValidationEnabled && !structuredDiagnostics) EXPECT_EQ(ValidationErrors.load(), 0u);
        if (Status == DeviceSetupStatus::Ready) testing::Test::RecordProperty("observed_validation_errors", ValidationErrors.load());
    }
    ~DeviceContext() { Reset(); }
};

// Declare after all referenced local GPU objects and before the first Submit.
// It also runs on an early ASSERT return, before those objects are destroyed.
struct GpuWorkGuard {
    DeviceContext& Context;
    ~GpuWorkGuard() { Context.WaitAllQueues(); }
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
        auto& context = *static_cast<DeviceContext*>(userData);
#if defined(RADRAY_ENABLE_D3D12)
        if (!context.DiagnosticSubscription)
#endif
            context.ValidationErrors.fetch_add(1, std::memory_order_relaxed);
        RADRAY_ERR_LOG("GPU validation: {}", message);
    }
}

inline void CaptureRhiValidationMessage(LogLevel, std::string_view, void* userData) {
    static_cast<DeviceContext*>(userData)->RhiValidationErrors.fetch_add(1, std::memory_order_relaxed);
}

inline bool TryCreateDevice(
    RenderBackend backend,
    DeviceContext& context,
    bool enableValidation = false,
    bool enableRhiValidation = false,
    [[maybe_unused]] array<uint32_t, static_cast<size_t>(QueueType::MAX_COUNT)> queueCounts = {1, 0, 0},
    [[maybe_unused]] shared_ptr<DXGIDiagnosticContext> diagnosticContext = nullptr) {
    context.Reset();
    context.Finalized = false;
    context.ValidationErrors = 0;
    context.RhiValidationErrors = 0;
#if defined(RADRAY_ENABLE_D3D12)
    context.Diagnostics.reset();
    context.DiagnosticMode.clear();
    context.DiagnosticSelection = "native";
    context.DiagnosticInterfaceResult.clear();
    context.DiagnosticSelfTest = false;
#endif
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
        if (enableValidation) {
            context.DiagnosticContext = diagnosticContext ? std::move(diagnosticContext) : DXGIDiagnosticContext::Create().Unwrap();
            context.Diagnostics = make_shared<DiagnosticProbe>(context.ValidationErrors, context.DiagnosticContext);
            context.DiagnosticSubscription = d3d12::detail::ObserveDiagnostics(context.DiagnosticContext, context.Diagnostics);
        }
        DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = enableValidation;
        factoryDesc.IsEnableGpuBasedValid = enableGpuValidation;
        factoryDesc.LogCallback = CaptureValidationMessage;
        factoryDesc.LogUserData = &context;
        factoryDesc.DiagnosticContext = context.DiagnosticContext;
        auto factory = DXGIFactory::Create(factoryDesc);
        if (!factory.HasValue()) {
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Factory, "DXGI factory creation failed");
        }
        context.Factory = factory.Release();
        const auto adapters = context.Factory->GetAdapters();
        auto selected = context.Factory->SelectHighPerformanceAdapter();
        if (const char* requested = std::getenv("RADRAY_TEST_D3D12_ADAPTER_INDEX")) {
            const std::string_view text{requested};
            uint32_t index{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                std::ranges::none_of(adapters, [index](const auto& adapter) { return adapter.Index == index; }))
                return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Adapter, "Requested D3D12 adapter index is invalid or unavailable");
            selected = index;
        }
        if (!selected)
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::NoAdapter, DeviceSetupStage::Adapter, "No D3D12 adapter is available");
        for (const auto& adapter : adapters)
            if (adapter.Index == *selected) context.AdapterName = adapter.Name;

        D3D12DeviceDescriptor deviceDesc{};
        deviceDesc.Factory = context.Factory.get();
        deviceDesc.AdapterIndex = selected;
        deviceDesc.QueueCounts = queueCounts;
        auto device = Device::Create(DeviceDescriptor{deviceDesc}, {.IsEnabled = false});
        if (!device.HasValue()) {
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Device, "D3D12 device creation failed for an available adapter");
        }
        context.Device = device.Release();
        if (enableValidation) {
            auto* native = static_cast<d3d12::DeviceD3D12*>(context.Device.get());
            d3d12::ComPtr<ID3D12InfoQueue> queue;
            context.ValidationEnabled = context.Factory->IsValidationEnabled() && native->_diagnosticChannel != nullptr;
            if (!context.ValidationEnabled || FAILED(native->_device.As(&queue)))
                return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Validation, "D3D12 diagnostic channel is unavailable");
            d3d12::ComPtr<ID3D12InfoQueue1> callbackQueue;
            const HRESULT callbackStatus = queue.As(&callbackQueue);
            context.DiagnosticInterfaceResult = fmt::format("{:#010x}", static_cast<uint32_t>(callbackStatus));
            context.DiagnosticMode = d3d12::detail::GetDiagnosticMode(native->_diagnosticChannel) == d3d12::detail::DiagnosticMode::Callback ? "callback" : "polling";
            context.DiagnosticSelfTest = context.Diagnostics->SelfTest(queue.Get());
            if (!context.DiagnosticSelfTest || !d3d12::detail::IsDiagnosticHealthy(native->_diagnosticChannel) ||
                !d3d12::detail::IsDiagnosticHealthy(native->_diagnostics))
                return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Validation, "D3D12/DXGI diagnostic self-test failed");
        }
        if (enableRhiValidation) {
            auto validated = detail::CreateValidationDevice(context.Device, {.IsEnabled = true, .LogCallback = CaptureRhiValidationMessage, .LogUserData = &context});
            if (!validated) return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Device, "RHI validation wrapper creation failed");
            context.Device = validated.Release();
        }
        testing::Test::RecordProperty("d3d12_adapter_index", *selected);
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
        auto selected = instance->SelectHighPerformancePhysicalDevice();
        if (const char* requested = std::getenv("RADRAY_TEST_VULKAN_ADAPTER_INDEX")) {
            const std::string_view text{requested};
            uint32_t index{};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), index);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Adapter, "RADRAY_TEST_VULKAN_ADAPTER_INDEX must be an unsigned integer");
            const auto adapters = instance->GetPhysicalDevices();
            if (std::ranges::none_of(adapters, [index](const auto& adapter) { return adapter.Index == index; }))
                return RejectDeviceSetup(context, backend, DeviceSetupStatus::InitializationFailed, DeviceSetupStage::Adapter, "Requested Vulkan adapter index is unavailable");
            selected = index;
        }
        if (!selected)
            return RejectDeviceSetup(context, backend, DeviceSetupStatus::NoAdapter, DeviceSetupStage::Adapter, "No Vulkan physical device is available");
        for (const auto& adapter : instance->GetPhysicalDevices())
            if (adapter.Index == *selected) context.AdapterName = adapter.Name;
        testing::Test::RecordProperty("vulkan_adapter_index", *selected);

        const VulkanCommandQueueDescriptor queues[]{
            VulkanCommandQueueDescriptor{QueueType::Direct, 1}};
        VulkanDeviceDescriptor deviceDesc{};
        deviceDesc.PhysicalDeviceIndex = selected;
        deviceDesc.Queues = queues;
        auto device = Device::Create(DeviceDescriptor{deviceDesc}, {.IsEnabled = enableRhiValidation, .LogCallback = CaptureRhiValidationMessage, .LogUserData = &context});
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
    if (backend == RenderBackend::Vulkan) context.ValidationEnabled = enableValidation;
    context.SynchronizationValidationEnabled = enableValidation && backend == RenderBackend::Vulkan;
    context.GpuValidationEnabled = enableGpuValidation;
    testing::Test::RecordProperty("adapter", context.AdapterName);
    testing::Test::RecordProperty("validation", context.ValidationEnabled ? "enabled" : "disabled");
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
