#pragma once
#include "gpu_test_fixture.h"

namespace radray::test {
/// Test-only host signaling holds real Direct submissions in flight; production keeps its fence API.
/// Declare after all submitted resources, so an early assertion releases and drains before they die.
class GpuSubmissionGate {
public:
    GpuSubmissionGate(render::Device& device, render::CommandQueue& queue) : Queue(queue), Fence(device.CreateFence().Release()) {}
    ~GpuSubmissionGate() {
        if (Fence) {
            Release(3);
            Queue.Wait();
        }
    }
    bool Release(uint64_t value) {
        if (!Fence) return false;
        if (value <= Released) return true;
#if defined(RADRAY_ENABLE_D3D12)
        if (auto* native = dynamic_cast<render::d3d12::FenceD3D12*>(Fence.get())) {
            if (FAILED(native->_fence->Signal(value))) return false;
            Released = value;
            return true;
        }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
        if (auto* native = dynamic_cast<render::vulkan::FenceVulkan*>(Fence.get())) {
            const VkSemaphoreSignalInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, nullptr, native->_fence->_semaphore, value};
            if (native->_device->_ftb.vkSignalSemaphore(native->_device->_device, &info) != VK_SUCCESS) return false;
            Released = value;
            return true;
        }
#endif
        return false;
    }
    render::CommandQueue& Queue;
    unique_ptr<render::Fence> Fence;
    uint64_t Released{0};
};
}  // namespace radray::test
