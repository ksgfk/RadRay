#pragma once

#include <radray/render/shader.h>

namespace radray::render::d3d12 {

class Dxil : public Shader {
public:
    Dxil(std::span<const byte> blob,
         std::span<const byte> refl,
         std::string_view entryPoint,
         std::string_view name,
         ShaderStage stage) noexcept;
    ~Dxil() noexcept override = default;

    bool IsValid() const noexcept override;
    void Destroy() noexcept override;

public:
    radray::vector<byte> _dxil;
    radray::vector<byte> _refl;
};

}  // namespace radray::render::d3d12
