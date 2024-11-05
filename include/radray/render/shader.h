#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

public:
    radray::string Name;
    radray::string EntryPoint;
    ShaderStage Stage;
};

}  // namespace radray::render
