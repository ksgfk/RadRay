#pragma once

#include <cstdint>

namespace radray {

struct AppUpdateContext;

/// Optional game-thread participant in the fixed Application frame order. Installed with
/// Application::AddExtension before the main loop starts; destroyed by the Application before
/// World / RenderSystem / GpuSystem in reverse installation order.
/// Slots run on the application thread in installation order. Frame order: docs/architecture/frame-and-gpu.md
class ApplicationExtension {
public:
    virtual ~ApplicationExtension() noexcept = default;
    /// After RenderSystem::BeginUpdateForFlight; this flight's previous GPU work has completed.
    virtual void OnBeginUpdate(uint32_t /*flight*/) {}
    /// After ApplicationScheduler::Pump and before WindowManager::DispatchInput. Raw window
    /// input is still pending; extensions may decide input capture here.
    virtual void OnBeforeInput(const AppUpdateContext& /*ctx*/) {}
    /// After World::Tick and before RenderSystem::PrepareFrame.
    virtual void OnAfterWorldTick(const AppUpdateContext& /*ctx*/) {}
};

}  // namespace radray
