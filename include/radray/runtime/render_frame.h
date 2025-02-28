#pragma once

#include <radray/render/device.h>
#include <radray/render/fence.h>

namespace radray::runtime {

class RenderFrame {
public:
    RenderFrame(render::Device* device) noexcept;

private:
    render::Device* _device;
    shared_ptr<render::Fence> _fence;
    uint64_t _fenceValue;
};

}  // namespace radray::runtime
