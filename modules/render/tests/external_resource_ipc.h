#pragma once
#include <radray/render/rhi.h>

namespace radray::render::test {
// Returns 77 for unavailable backend/capability, 78 for a recorded known-issue skip, and 1 for errors.
int RunExternalSupervisor(std::string_view executable, RenderBackend producer, RenderBackend consumer, uint32_t slots, bool interrupt, bool opaque = false);
int ExternalWorkerMain(int argc, char** argv);
}  // namespace radray::render::test
