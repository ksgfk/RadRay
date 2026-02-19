#include "metal_cmd_buffer.h"

namespace radray::render::metal {

void CmdBufferMetal::Destroy() noexcept {
    _buffer.reset();
}

}  // namespace radray::render::metal
