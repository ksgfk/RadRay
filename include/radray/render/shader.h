#pragma once

#include <radray/render/common.h>

namespace radray::render {

class Shader : public RenderBase {
public:
    virtual ~Shader() noexcept = default;

public:
    string Name;
    string EntryPoint;
    ShaderStage Stage;
    ShaderBlobCategory Category;
};

}  // namespace radray::render
