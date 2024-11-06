#pragma once

#include <radray/render/shader.h>
#include "metal_helper.h"

namespace radray::render::metal {

class ShaderLibMetal : public Shader {
public:
    ShaderLibMetal(
        NS::SharedPtr<MTL::Library> lib,
        std::string_view name,
        std::string_view entryPoint,
        ShaderStage stage) noexcept;
    ~ShaderLibMetal() noexcept override = default;

    bool IsValid() const noexcept override { return _library.get() != nullptr; }
    void Destroy() noexcept override;

public:
    NS::SharedPtr<MTL::Library> _library;
};

}  // namespace radray::render::metal
