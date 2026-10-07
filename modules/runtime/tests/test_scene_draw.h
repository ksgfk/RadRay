#pragma once

#include <radray/render/rhi.h>

namespace radray::test {

void RunDraw(render::RenderBackend backend, uint32_t views, bool threaded, bool drop, bool material = false, bool depth = false);

}  // namespace radray::test
