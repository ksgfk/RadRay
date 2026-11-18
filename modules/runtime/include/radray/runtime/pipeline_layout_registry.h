#pragma once

#include <radray/nullable.h>
#include <radray/types.h>

namespace radray::gpu {
class Device;
}

namespace radray::shader {
struct ShaderPipelineLayoutDesc;
}

namespace radray {

class GpuPipelineLayout;

class PipelineLayoutRegistry final {
public:
    explicit PipelineLayoutRegistry(gpu::Device& device);
    ~PipelineLayoutRegistry() noexcept;
    PipelineLayoutRegistry(const PipelineLayoutRegistry&) = delete;
    PipelineLayoutRegistry& operator=(const PipelineLayoutRegistry&) = delete;

    // Serial access. Owners may outlive the registry and must release before the device.
    Nullable<shared_ptr<GpuPipelineLayout>> Acquire(const shader::ShaderPipelineLayoutDesc& desc);

private:
    class Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
