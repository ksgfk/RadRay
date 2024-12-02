#include "d3d12_shader.h"

namespace radray::render::d3d12 {

Dxil::Dxil(
    std::span<const byte> blob,
    const DxilReflection& refl,
    std::string_view entryPoint,
    std::string_view name,
    ShaderStage stage) noexcept
    : _dxil(blob.begin(), blob.end()),
      _refl(refl) {
    Name = name;
    EntryPoint = entryPoint;
    Stage = stage;
}

bool Dxil::IsValid() const noexcept { return _dxil.size() > 0; }

void Dxil::Destroy() noexcept {
    _dxil.clear();
    _dxil.resize(0);
}

D3D12_SHADER_BYTECODE Dxil::ToByteCode() const noexcept {
    return D3D12_SHADER_BYTECODE{_dxil.data(), _dxil.size()};
}

}  // namespace radray::render::d3d12
