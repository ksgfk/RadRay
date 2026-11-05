#pragma once

#include <filesystem>

#include <radray/render/rhi.h>

namespace radray::examples::mesh_drawing {

int RunSample(render::RenderBackend backend, const std::filesystem::path& shaderDirectory,
              const std::filesystem::path& output, uint32_t slotCount = 2, uint32_t frameCount = 6);
int SampleMain(int argc, char** argv);

}  // namespace radray::examples::mesh_drawing
