#pragma once

#include <radray/runtime/render_system.h>
#include <radray/runtime/render_framework/scene_manager.h>
#include <radray/runtime/gpu_system.h>

namespace radray::example {

unique_ptr<StaticMesh> CreateCube(GpuSystem& gpu);

}  // namespace radray::example
