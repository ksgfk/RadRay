#pragma once

#include <radray/render/device.h>
#include "metal_helper.h"
#include "metal_cmd_queue.h"

namespace radray::render::metal {

class DeviceMetal : public radray::render::Device {
public:
    DeviceMetal() noexcept = default;
    ~DeviceMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _device.get() != nullptr; }
    void Destroy() noexcept override;

    Backend GetBackend() noexcept override { return Backend::Metal; }

    std::optional<CommandQueue*> GetCommandQueue(QueueType type, uint32_t slot) noexcept override;

public:
    NS::SharedPtr<MTL::Device> _device;
    std::array<radray::vector<radray::unique_ptr<CmdQueueMetal>>, 3> _queues;
};

std::optional<std::shared_ptr<DeviceMetal>> CreateDevice(const MetalDeviceDescriptor& desc) noexcept;

}  // namespace radray::render::metal