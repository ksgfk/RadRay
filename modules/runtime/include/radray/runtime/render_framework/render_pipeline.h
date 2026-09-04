#pragma once

#include <span>

#include <radray/runtime/asset_manager.h>
#include <radray/runtime/gpu_system.h>
#include <radray/types.h>

namespace radray {

struct AppUpdateContext;

struct RenderPipelineTarget {
    AppFrameTarget Target;
    render::TextureStates State{render::TextureState::Undefined};
    bool ContentDrawn{false};
};

struct RenderPipelineContext {
    AppFrameContext& Frame;
    // The host provides targets in RenderTarget state and transitions them to Present afterwards.
    std::span<RenderPipelineTarget> Targets;
};

class RenderPipeline {
public:
    RenderPipeline() noexcept = default;
    RenderPipeline(const RenderPipeline&) = delete;
    RenderPipeline(RenderPipeline&&) = delete;
    RenderPipeline& operator=(const RenderPipeline&) = delete;
    RenderPipeline& operator=(RenderPipeline&&) = delete;
    virtual ~RenderPipeline() noexcept;

    /// Game thread, after World::Tick and after this flight's previous GPU work has completed.
    /// Write only this flight's private input; append references needed until flight reuse.
    virtual void PrepareFrame(const AppUpdateContext& ctx, vector<StreamingAssetRefAny>& retainedAssets);

    /// Render thread. Consume only the immutable input prepared for ctx.Frame.FlightIndex().
    virtual void Render(RenderPipelineContext& ctx) = 0;
};

}  // namespace radray
