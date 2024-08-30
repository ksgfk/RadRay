#pragma once

#include <string_view>

#include <radray/rhi/ctypes.h>

namespace radray::rhi {

}  // namespace radray::rhi

std::string_view format_as(RadrayBackand v) noexcept;
std::string_view format_as(RadrayQueueType v) noexcept;
std::string_view format_as(RadrayFormat v) noexcept;
std::string_view format_as(RadrayFilterMode v) noexcept;
std::string_view format_as(RadrayAddressMode v) noexcept;
std::string_view format_as(RadrayMipMapMode v) noexcept;
std::string_view format_as(RadrayTopology v) noexcept;
std::string_view format_as(RadrayBlendType v) noexcept;
std::string_view format_as(RadrayBlendOp v) noexcept;
std::string_view format_as(RadrayCullMode v) noexcept;
std::string_view format_as(RadrayFrontFace v) noexcept;
std::string_view format_as(RadrayFillMode v) noexcept;
std::string_view format_as(RadrayVertexInputRate v) noexcept;
std::string_view format_as(RadrayCompareMode v) noexcept;
std::string_view format_as(RadrayStencilOp v) noexcept;
std::string_view format_as(RadrayTextureDimension v) noexcept;
std::string_view format_as(RadrayFenceState v) noexcept;
std::string_view format_as(RadrayResourceState v) noexcept;
std::string_view format_as(RadrayResourceType v) noexcept;
std::string_view format_as(RadrayHeapUsage v) noexcept;
std::string_view format_as(RadrayBufferCreateFlag v) noexcept;
std::string_view format_as(RadrayTextureMSAACount v) noexcept;
std::string_view format_as(RadrayTextureCreateFlag v) noexcept;
std::string_view format_as(RadrayShaderStage v) noexcept;
std::string_view format_as(RadrayVertexSemantic v) noexcept;
