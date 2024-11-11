#include "d3d12_shader.h"

namespace radray::render::d3d12 {

Dxil::Dxil(
    std::span<const byte> blob,
    std::span<const byte> refl,
    std::string_view entryPoint,
    std::string_view name,
    ShaderStage stage) noexcept
    : _dxil(blob.begin(), blob.end()),
      _refl(refl.begin(), refl.end()) {
    Name = name;
    EntryPoint = entryPoint;
    Stage = stage;
}

bool Dxil::IsValid() const noexcept { return _dxil.size() > 0; }

void Dxil::Destroy() noexcept {
    _dxil.clear();
    _dxil.resize(0);
}

}  // namespace radray::render::d3d12
