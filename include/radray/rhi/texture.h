#pragma once

#include <radray/object.h>
#include <radray/rhi/resource.h>

namespace radray::rhi {

class ITexture : public IResource {
public:
    ITexture(
        std::shared_ptr<IDevice> device,
        uint32_t width,
        uint32_t height,
        uint32_t depth,
        uint32_t mip,
        RhiFormat format,
        TextureDimension dim)
        : IResource(std::move(device)),
          _width(width),
          _height(height),
          _depth(depth),
          _mip(mip),
          _format(format),
          _dim(dim) {}
    ~ITexture() noexcept override = default;

    uint32_t GetWidth() const noexcept { return _width; }
    uint32_t GetHeight() const noexcept { return _height; }
    uint32_t GetDepth() const noexcept { return _depth; }
    uint32_t GetMip() const noexcept { return _mip; }
    RhiFormat GetFormat() const noexcept { return _format; }
    TextureDimension GetDimension() const noexcept { return _dim; }

private:
    uint32_t _width;
    uint32_t _height;
    uint32_t _depth;
    uint32_t _mip;
    RhiFormat _format;
    TextureDimension _dim;
};

}  // namespace radray::rhi
