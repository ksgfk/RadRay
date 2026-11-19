#pragma once

#include <filesystem>

#include <radray/types.h>
#include <radray/gpu/types.h>

namespace radray::examples::mesh_drawing {

int RunSample(gpu::GpuBackend backend, const std::filesystem::path& shaderDirectory,
              const std::filesystem::path& output, uint32_t slotCount = 2, uint32_t frameCount = 6, bool hotReload = false);
int SampleMain(int argc, char** argv);

}  // namespace radray::examples::mesh_drawing
