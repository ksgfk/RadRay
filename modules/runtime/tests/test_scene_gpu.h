#pragma once

#include <radray/render/rhi.h>

namespace radray::test {

void RunObjectBuffers(render::RenderBackend backend, uint32_t flights, bool threaded, bool delayed = false);
void RunAllocationRecovery(render::RenderBackend backend);

}  // namespace radray::test
