#pragma once

#include <radray/render/rhi.h>

namespace radray::test {

void RunDraw(render::RenderBackend backend, uint32_t views, bool threaded, bool drop);

}  // namespace radray::test
