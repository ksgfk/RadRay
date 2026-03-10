#pragma once

#include <mutex>
#include <span>

#include <radray/logger.h>
#include <radray/render/common.h>

namespace radray::runtime::test {

enum class TestBackend {
    D3D12,
    Vulkan,
};

std::string_view format_as(TestBackend backend) noexcept;

vector<TestBackend> GetEnabledDeviceBackends() noexcept;

vector<TestBackend> GetEnabledRuntimeShaderBackends() noexcept;

string JoinErrors(std::span<const string> errors, size_t maxCount = 8);

class LogCollector {
public:
    static void Callback(LogLevel level, std::string_view message, void* userData);

    vector<string> GetErrors() const;

    void Clear() noexcept;

private:
    mutable std::mutex _mutex{};
    vector<string> _errors{};
};

class ScopedVulkanInstance {
public:
    ~ScopedVulkanInstance() noexcept;

    unique_ptr<render::InstanceVulkan>& Ref() noexcept;

    void Reset() noexcept;

private:
    unique_ptr<render::InstanceVulkan> _instance{};
};

class RuntimeTestContext {
public:
    ~RuntimeTestContext() noexcept;

    bool Initialize(TestBackend backend, string* reason) noexcept;

    void Reset() noexcept;

    TestBackend GetBackend() const noexcept;

    std::string_view GetBackendName() const noexcept;

    shared_ptr<render::Device> GetDevice() const noexcept;

    render::Device* GetDevicePtr() const noexcept;

    render::CommandQueue* GetQueue() const noexcept;

    render::DeviceDetail GetDeviceDetail() const noexcept;

    vector<string> GetCapturedErrors() const;

    string JoinCapturedErrors(size_t maxCount = 8) const;

    void ClearCapturedErrors() noexcept;

private:
    TestBackend _backend{TestBackend::D3D12};
    LogCollector _logs{};
    ScopedVulkanInstance _vkInstance{};
    shared_ptr<render::Device> _device{};
    render::CommandQueue* _queue{nullptr};
};

}  // namespace radray::runtime::test
