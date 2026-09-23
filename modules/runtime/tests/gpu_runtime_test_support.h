#pragma once

#include "gpu_test_fixture.h"

#include <radray/runtime/startup_result.h>
#include <radray/runtime/application.h>

namespace radray::test {

inline bool CanSkipRuntimeStartup(render::RenderBackend backend, const RuntimeStartupResult& startup) noexcept {
    const bool unavailable = startup.Status == RuntimeStartupStatus::BackendNotBuilt ||
                             startup.Status == RuntimeStartupStatus::NoAdapter ||
                             startup.Status == RuntimeStartupStatus::ValidationUnavailable;
    return unavailable && !render::test::RequiredBackend(backend);
}

struct RuntimeRunResult {
    int ExitCode;
    RuntimeStartupResult Startup;
};

inline RuntimeRunResult RunApplication(Application& app, const ApplicationRuntimeDescriptor& desc) {
    RuntimeRunResult result{};
    result.ExitCode = app.Run(desc, result.Startup);
    return result;
}

}  // namespace radray::test
