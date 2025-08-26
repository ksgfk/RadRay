#include "metal_function.h"

namespace radray::render::metal {

FunctionMetal::FunctionMetal(
    NS::SharedPtr<MTL::Function> func,
    std::string_view name,
    std::string_view entryPoint,
    ShaderStage stage) noexcept
    : _func(std::move(func)) {
    Name = name;
    EntryPoint = entryPoint;
    Stage = stage;
}

void FunctionMetal::Destroy() noexcept {
    _func.reset();
}

}  // namespace radray::render::metal
