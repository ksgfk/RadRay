#pragma once

#include <radray/render/rhi.h>

namespace radray::render::detail {

Nullable<shared_ptr<Device>> CreateValidationDevice(shared_ptr<Device> device, const RhiValidationOptions& options);

}  // namespace radray::render::detail
