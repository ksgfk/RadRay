#pragma once

#include <radray/render/root_signature.h>

namespace radray::render::metal {

// placeholder, no use
class RootSigMetal : public RootSignature {
public:
    ~RootSigMetal() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;
};

}  // namespace radray::render::metal
