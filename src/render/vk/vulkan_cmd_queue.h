#pragma once

#include <radray/render/command_queue.h>

#include "vulkan_helper.h"

namespace radray::render::vulkan {

struct QueueIndexInFamily {
    uint32_t Family;
    uint32_t IndexInFamily;
};

class QueueVulkan : public CommandQueue {
public:
    QueueVulkan(
        DeviceVulkan* device,
        VkQueue queue,
        QueueIndexInFamily inFamily,
        QueueType type) noexcept
        : _device(device),
          _queue(queue),
          _inFamily(inFamily),
          _type(type) {}

    ~QueueVulkan() noexcept override;

    bool IsValid() const noexcept override;

    void Destroy() noexcept override;

    Nullable<shared_ptr<CommandBuffer>> CreateCommandBuffer() noexcept override;

    void Submit(std::span<CommandBuffer*> buffers, Nullable<Fence> singalFence) noexcept override;

    void Wait() noexcept override;

    void WaitFences(std::span<Fence*> fences) noexcept override;

public:
    DeviceVulkan* _device;
    VkQueue _queue;
    QueueIndexInFamily _inFamily;
    QueueType _type;
};

}  // namespace radray::render::vulkan
