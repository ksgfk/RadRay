#include "texture.h"

#include "device.h"

namespace radray::rhi::d3d12 {

Texture::Texture(
    std::shared_ptr<Device> device,
    const TextureConstructParams& tcParams)
    : ITexture(
          std::move(device),
          tcParams.width,
          tcParams.height,
          tcParams.depth,
          tcParams.mip,
          ToRhiFormat(tcParams.format),
          tcParams.dim),
      _resource(tcParams.texture),
      _alloc(tcParams.allocaton),
      _initState(tcParams.initState) {}

}  // namespace radray::rhi::d3d12
