#pragma once

#include <radray/types.h>
#include <radray/utility.h>

namespace radray::render {

enum class Backend {
    D3D12,
    Vulkan,
    Metal
};

enum class TextureFormat {
    UNKNOWN,

    R8_SINT,
    R8_UINT,
    R8_SNORM,
    R8_UNORM,

    R16_SINT,
    R16_UINT,
    R16_SNORM,
    R16_UNORM,
    R16_FLOAT,

    RG8_SINT,
    RG8_UINT,
    RG8_SNORM,
    RG8_UNORM,

    R32_SINT,
    R32_UINT,
    R32_FLOAT,

    RG16_SINT,
    RG16_UINT,
    RG16_SNORM,
    RG16_UNORM,
    RG16_FLOAT,

    RGBA8_SINT,
    RGBA8_UINT,
    RGBA8_SNORM,
    RGBA8_UNORM,
    RGBA8_UNORM_SRGB,
    BGRA8_UNORM,
    BGRA8_UNORM_SRGB,

    RGB10A2_UINT,
    RGB10A2_UNORM,
    RG11B10_FLOAT,

    RG32_SINT,
    RG32_UINT,
    RG32_FLOAT,

    RGBA16_SINT,
    RGBA16_UINT,
    RGBA16_SNORM,
    RGBA16_UNORM,
    RGBA16_FLOAT,

    RGBA32_SINT,
    RGBA32_UINT,
    RGBA32_FLOAT,

    S8,
    D16_UNORM,
    D32_FLOAT,
    D24_UNORM_S8_UINT,
    D32_FLOAT_S8_UINT,
};

enum class QueueType : uint32_t {
    Direct,
    Compute,
    Copy,
};

enum class ShaderStage {
    Vertex,
    Pixel,
    Compute
};

bool IsDepthStencilFormat(TextureFormat format) noexcept;

class RenderBase : public Noncopyable {
public:
    virtual ~RenderBase() noexcept = default;

    virtual bool IsValid() const noexcept = 0;

    virtual void Destroy() noexcept = 0;
};

std::string_view format_as(Backend v) noexcept;
std::string_view format_as(TextureFormat v) noexcept;
std::string_view format_as(QueueType v) noexcept;

}  // namespace radray::render
