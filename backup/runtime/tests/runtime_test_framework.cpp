#include "runtime_test_framework.h"

#include <algorithm>

#include <fmt/format.h>
#include <fmt/ranges.h>

namespace radray::runtime::test {
namespace {

void StoreReason(string* reason, std::string_view message) {
    if (reason != nullptr) {
        *reason = string{message};
    }
}

bool CreateDeviceForBackend(
    TestBackend backend,
    LogCollector* logs,
    ScopedVulkanInstance& vkInstance,
    shared_ptr<render::Device>& device,
    string* reason) noexcept {
    switch (backend) {
        case TestBackend::D3D12: {
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
            render::D3D12DeviceDescriptor desc{};
            desc.AdapterIndex = std::nullopt;
            desc.IsEnableDebugLayer = true;
            desc.IsEnableGpuBasedValid = true;
            desc.LogCallback = &LogCollector::Callback;
            desc.LogUserData = logs;
            auto deviceOpt = render::CreateDevice(desc);
            if (!deviceOpt.HasValue()) {
                StoreReason(reason, "CreateDevice(D3D12) failed");
                return false;
            }
            device = deviceOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            StoreReason(reason, "D3D12 backend is not enabled for this build");
            return false;
#endif
        }
        case TestBackend::Vulkan: {
#if defined(RADRAY_ENABLE_VULKAN)
            render::VulkanInstanceDescriptor instanceDesc{};
            instanceDesc.AppName = "RuntimeTests";
            instanceDesc.AppVersion = 1;
            instanceDesc.EngineName = "RadRay";
            instanceDesc.EngineVersion = 1;
            instanceDesc.IsEnableDebugLayer = true;
            instanceDesc.IsEnableGpuBasedValid = false;
            instanceDesc.LogCallback = &LogCollector::Callback;
            instanceDesc.LogUserData = logs;
            auto instanceOpt = render::CreateVulkanInstance(instanceDesc);
            if (!instanceOpt.HasValue()) {
                StoreReason(reason, "CreateVulkanInstance failed");
                return false;
            }
            vkInstance.Ref() = instanceOpt.Release();

            render::VulkanCommandQueueDescriptor queueDesc{};
            queueDesc.Type = render::QueueType::Direct;
            queueDesc.Count = 1;
            render::VulkanDeviceDescriptor deviceDesc{};
            deviceDesc.PhysicalDeviceIndex = std::nullopt;
            deviceDesc.Queues = std::span{&queueDesc, 1};
            auto deviceOpt = render::CreateDevice(deviceDesc);
            if (!deviceOpt.HasValue()) {
                StoreReason(reason, "CreateDevice(Vulkan) failed");
                vkInstance.Reset();
                return false;
            }
            device = deviceOpt.Release();
            return true;
#else
            RADRAY_UNUSED(logs);
            StoreReason(reason, "Vulkan backend is not enabled for this build");
            return false;
#endif
        }
        default: {
            StoreReason(reason, "unsupported backend");
            return false;
        }
    }
}

}  // namespace

std::string_view format_as(TestBackend backend) noexcept {
    switch (backend) {
        case TestBackend::D3D12: return "d3d12";
        case TestBackend::Vulkan: return "vulkan";
        default: return "unknown";
    }
}

vector<TestBackend> GetEnabledDeviceBackends() noexcept {
    vector<TestBackend> backends{};
#if defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(TestBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    backends.push_back(TestBackend::Vulkan);
#endif
    return backends;
}

vector<TestBackend> GetEnabledRuntimeShaderBackends() noexcept {
    vector<TestBackend> backends{};
#if defined(RADRAY_ENABLE_DXC) && defined(RADRAY_ENABLE_D3D12) && defined(_WIN32)
    backends.push_back(TestBackend::D3D12);
#endif
#if defined(RADRAY_ENABLE_DXC) && defined(RADRAY_ENABLE_VULKAN) && defined(RADRAY_ENABLE_SPIRV_CROSS)
    backends.push_back(TestBackend::Vulkan);
#endif
    return backends;
}

string JoinErrors(std::span<const string> errors, size_t maxCount) {
    if (errors.empty()) {
        return {};
    }
    const size_t count = std::min(maxCount, errors.size());
    string result = fmt::format("{}", fmt::join(errors.begin(), errors.begin() + count, "\n"));
    if (errors.size() > count) {
        result += fmt::format("\n...({} more)", errors.size() - count);
    }
    return result;
}

void LogCollector::Callback(LogLevel level, std::string_view message, void* userData) {
    auto* self = static_cast<LogCollector*>(userData);
    if (self == nullptr) {
        return;
    }
    if (level != LogLevel::Err && level != LogLevel::Critical) {
        return;
    }
    std::lock_guard<std::mutex> lock(self->_mutex);
    self->_errors.emplace_back(message);
}

vector<string> LogCollector::GetErrors() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _errors;
}

void LogCollector::Clear() noexcept {
    std::lock_guard<std::mutex> lock(_mutex);
    _errors.clear();
}

ScopedVulkanInstance::~ScopedVulkanInstance() noexcept {
    this->Reset();
}

unique_ptr<render::InstanceVulkan>& ScopedVulkanInstance::Ref() noexcept {
    return _instance;
}

void ScopedVulkanInstance::Reset() noexcept {
    if (_instance != nullptr) {
        render::DestroyVulkanInstance(std::move(_instance));
    }
}

RuntimeTestContext::~RuntimeTestContext() noexcept {
    this->Reset();
}

bool RuntimeTestContext::Initialize(TestBackend backend, string* reason) noexcept {
    this->Reset();
    _backend = backend;
    SetLogCallback(&LogCollector::Callback, &_logs);
    if (!CreateDeviceForBackend(backend, &_logs, _vkInstance, _device, reason)) {
        return false;
    }
    auto queueOpt = _device->GetCommandQueue(render::QueueType::Direct, 0);
    if (!queueOpt.HasValue()) {
        StoreReason(reason, fmt::format("no direct queue available for {}", this->GetBackendName()));
        this->Reset();
        return false;
    }
    _queue = queueOpt.Get();
    return true;
}

void RuntimeTestContext::Reset() noexcept {
    if (_queue != nullptr && _device != nullptr) {
        _queue->Wait();
    }
    ClearLogCallback();
    _queue = nullptr;
    _device.reset();
    _vkInstance.Reset();
    _logs.Clear();
}

TestBackend RuntimeTestContext::GetBackend() const noexcept {
    return _backend;
}

std::string_view RuntimeTestContext::GetBackendName() const noexcept {
    return format_as(_backend);
}

shared_ptr<render::Device> RuntimeTestContext::GetDevice() const noexcept {
    return _device;
}

render::Device* RuntimeTestContext::GetDevicePtr() const noexcept {
    return _device.get();
}

render::CommandQueue* RuntimeTestContext::GetQueue() const noexcept {
    return _queue;
}

render::DeviceDetail RuntimeTestContext::GetDeviceDetail() const noexcept {
    return _device != nullptr ? _device->GetDetail() : render::DeviceDetail{};
}

vector<string> RuntimeTestContext::GetCapturedErrors() const {
    return _logs.GetErrors();
}

string RuntimeTestContext::JoinCapturedErrors(size_t maxCount) const {
    const auto errors = this->GetCapturedErrors();
    return test::JoinErrors(errors, maxCount);
}

void RuntimeTestContext::ClearCapturedErrors() noexcept {
    _logs.Clear();
}

}  // namespace radray::runtime::test
