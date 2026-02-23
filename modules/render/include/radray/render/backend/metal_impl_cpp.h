#pragma once

#ifdef RADRAY_ENABLE_METAL

#include <radray/render/common.h>

namespace radray::render::metal {

Nullable<shared_ptr<Device>> CreateDevice(const MetalDeviceDescriptor& desc);

}  // namespace radray::render::metal

#endif
