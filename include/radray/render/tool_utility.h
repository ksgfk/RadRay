#pragma once

#include <radray/image_data.h>
#include <radray/render/common.h>

namespace radray::render {

TextureFormat ImageToTextureFormat(radray::ImageFormat fmt) noexcept;

}
