#pragma once

#include <radray/render/common.h>

namespace radray::render {

class RootSignature : public RenderBase {
public:
    virtual ~RootSignature() noexcept = default;
};

}  // namespace radray::render
