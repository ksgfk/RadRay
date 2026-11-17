#pragma once

#include <radray/gpu/types.h>

namespace radray {

class GpuPipelineLayout final {
    class ConstructionKey {};
    using GroupOwners = vector<std::pair<uint32_t, shared_ptr<gpu::BindingGroupLayout>>>;

public:
    GpuPipelineLayout(ConstructionKey, GroupOwners groups, unique_ptr<gpu::PipelineLayout> layout) noexcept;
    GpuPipelineLayout(const GpuPipelineLayout&) = delete;
    GpuPipelineLayout& operator=(const GpuPipelineLayout&) = delete;
    gpu::PipelineLayout& GetLayout() const noexcept;
    Nullable<shared_ptr<gpu::BindingGroupLayout>> GetGroupLayout(uint32_t groupSlot) const noexcept;

private:
    friend class PipelineLayoutRegistry;
    GroupOwners _groups;
    unique_ptr<gpu::PipelineLayout> _layout;
};

}  // namespace radray
