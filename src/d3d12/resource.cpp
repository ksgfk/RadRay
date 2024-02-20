#include <radray/d3d12/resource.h>

#include <radray/d3d12/device.h>

namespace radray::d3d12 {

Resource::Resource(Device* device) noexcept : device(device) {}

}  // namespace radray::d3d12
