#include <radray/runtime/gpu_pipeline_layout.h>

namespace radray {

GpuPipelineLayout::GpuPipelineLayout(ConstructionKey, GroupOwners groups, unique_ptr<gpu::PipelineLayout> layout) noexcept
    : _groups(std::move(groups)), _layout(std::move(layout)) {}

gpu::PipelineLayout& GpuPipelineLayout::GetLayout() const noexcept { return *_layout; }

Nullable<shared_ptr<gpu::BindingGroupLayout>> GpuPipelineLayout::GetGroupLayout(uint32_t groupSlot) const noexcept {
    for (const auto& [slot, layout] : _groups)
        if (slot == groupSlot) return layout;
    return nullptr;
}

}  // namespace radray
