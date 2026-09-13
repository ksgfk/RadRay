#pragma once

#include <radray/nullable.h>
#include <radray/types.h>

namespace radray {
class FrameDrawResources;
struct FrameParameterDomain {
    Nullable<FrameDrawResources*> Owner{nullptr};
    uint64_t Epoch{0};
    uint32_t Index{UINT32_MAX};
    bool IsValid() const noexcept { return Owner && Epoch && Index != UINT32_MAX; }
};
}  // namespace radray
