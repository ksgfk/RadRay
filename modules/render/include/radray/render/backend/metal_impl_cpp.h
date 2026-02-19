#pragma once

#include <radray/render/common.h>

namespace radray::render::metal {

Nullable<shared_ptr<Device>> CreateDevice(const MetalDeviceDescriptor& desc);

}  // namespace radray::render::metal
