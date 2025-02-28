#include <radray/runtime/render_frame.h>

namespace radray::runtime {

RenderFrame::RenderFrame(render::Device* device) noexcept
    : _device(device),
      _fenceValue(0) {}

}  // namespace radray::runtime
