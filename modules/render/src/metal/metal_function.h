#pragma once

#include <radray/render/shader.h>
#include "metal_helper.h"

namespace radray::render::metal {

class FunctionMetal : public Shader {
public:
    FunctionMetal(
        NS::SharedPtr<MTL::Function> func,
        std::string_view name,
        std::string_view entryPoint,
        ShaderStage stage) noexcept;
    ~FunctionMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _func.get() != nullptr; }
    void Destroy() noexcept override;

public:
    NS::SharedPtr<MTL::Function> _func;
};

}  // namespace radray::render::metal
