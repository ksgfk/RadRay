#include "metal_shader_lib.h"

namespace radray::render::metal {

void ShaderLibMetal::Destroy() noexcept {
    _library.reset();
}

}  // namespace radray::render::metal
