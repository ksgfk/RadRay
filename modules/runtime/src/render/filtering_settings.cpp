#include <radray/runtime/render/renderer_list.h>

#include <radray/runtime/render/renderer.h>
#include <radray/runtime/render/material.h>

namespace radray::srp {

bool FilteringSettings::Test(const Renderer& r) const noexcept {
    if (!r.IsVisible()) {
        return false;
    }
    if ((r.LayerMask() & LayerMask) == 0u) {
        return false;
    }
    Material* mat = r.GetMaterial();
    if (mat == nullptr) {
        return false;
    }
    return QueueRange.Contains(mat->Queue());
}

}  // namespace radray::srp
