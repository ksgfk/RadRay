#include "metal_shader_lib.h"

namespace radray::render::metal {

ShaderLibMetal::ShaderLibMetal(
    NS::SharedPtr<MTL::Library> lib,
    std::string_view name,
    std::string_view entryPoint,
    ShaderStage stage) noexcept
    : _library(std::move(lib)) {
    Name = name;
    EntryPoint = entryPoint;
    Stage = stage;
}

void ShaderLibMetal::Destroy() noexcept {
    _library.reset();
}

}  // namespace radray::render::metal
