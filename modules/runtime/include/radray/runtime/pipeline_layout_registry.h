#pragma once

#include <radray/runtime/gpu_pipeline_layout.h>
#include <radray/shader/shader_layout.h>

namespace radray {

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
