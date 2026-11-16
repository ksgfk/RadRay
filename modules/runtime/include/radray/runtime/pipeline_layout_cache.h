#pragma once

#include <radray/runtime/shader_layout.h>

namespace radray {

class PipelineLayoutCache final {
public:
    explicit PipelineLayoutCache(render::Device& device);
    // All PSOs, parameter sets and recorded uses must have ended before destruction.
    ~PipelineLayoutCache() noexcept;
    PipelineLayoutCache(const PipelineLayoutCache&) = delete;
    PipelineLayoutCache& operator=(const PipelineLayoutCache&) = delete;

    // Input is borrowed for this call only. The returned object lives with this cache.
    Nullable<render::PipelineLayout*> Acquire(const ShaderPipelineLayoutDesc& desc);

private:
    class Impl;
    unique_ptr<Impl> _impl;
};

}  // namespace radray
