#include <radray/render/device.h>

namespace radray::render {

std::optional<radray::shared_ptr<Device>> CreateDevice(const DeviceDescriptor& desc) {
    return std::nullopt;
}

}  // namespace radray::render
