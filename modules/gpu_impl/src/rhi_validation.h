#pragma once

#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray::gpu::detail {

Nullable<shared_ptr<Device>> CreateValidationDevice(shared_ptr<Device> device, const RhiValidationOptions& options);

}  // namespace radray::gpu::detail
