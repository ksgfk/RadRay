#pragma once

// runtime 所有权契约见 docs/architecture/runtime.md。

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>
#include <radray/runtime/gpu_env.h>

namespace radray {

struct RDGDeviceConstraints;

class DeviceContext final {
public:
    DeviceContext(
        shared_ptr<gpu::Device> device,
        gpu::CommandQueue* directQueue) noexcept;
    ~DeviceContext() noexcept;

    DeviceContext(const DeviceContext&) = delete;
    DeviceContext& operator=(const DeviceContext&) = delete;
    DeviceContext(DeviceContext&&) = delete;
    DeviceContext& operator=(DeviceContext&&) = delete;

    // 成功时转交完整对象；失败时记录日志并返回空所有者。
    // environment 必须晚于 runtime 销毁；其工厂或实例仅被借用。
    [[nodiscard]] static Nullable<unique_ptr<DeviceContext>> Create(
        Environment& environment) noexcept;

    gpu::Device& GetDevice() noexcept;
    gpu::CommandQueue& GetQueue() noexcept;
    Nullable<gpu::PipelineCache*> GetPipelineCache() const noexcept;
    vector<byte> GetPipelineCacheData() const noexcept;
    bool SupportsIndirect() const noexcept;
    RDGDeviceConstraints GetRDGConstraints() const noexcept;

private:
    shared_ptr<gpu::Device> _device;
    gpu::CommandQueue* _directQueue;
    unique_ptr<gpu::PipelineCache> _pipelineCache;
};

}  // namespace radray
