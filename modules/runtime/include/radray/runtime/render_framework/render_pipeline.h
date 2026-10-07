#pragma once

#include <chrono>
#include <radray/runtime/cmd_allocator.h>
#include <radray/runtime/gpu_frame_resources.h>
#include <radray/runtime/render_framework/scene_view.h>
#include <radray/runtime/render_framework/render_output.h>

namespace radray {
class RenderSystem;
class SceneManager;
class AppFrameContext;

struct PipelineFrameInfo {
    uint32_t FlightIndex;
    uint64_t FrameSerial;
    std::chrono::duration<float> DeltaTime;
};
struct PipelineContext {
    RenderSystem& Renderer;
    PipelineFrameInfo Frame;
    ICmdAllocator& Commands;
    GpuFrameResources& Resources;
    Nullable<SceneManager*> Scenes{nullptr};
};
struct RenderPipelineRequest {
    std::span<const SceneViewRequest> Views;
    std::optional<RenderOutput> Output;
};
enum class PipelineRecordStatus : uint8_t { Recorded,
                                            NoWork,
                                            RecoverableFailure };
struct PipelineRecordResult {
    PipelineRecordStatus Status{PipelineRecordStatus::NoWork};
    /// Returned, unregistered commands, in dependency/submission order. Preserve on recoverable failure.
    vector<render::CommandBuffer*> Commands;
    std::optional<RenderOutputState> OutputState;
};
class RenderPipeline {
public:
    virtual ~RenderPipeline() noexcept = default;
    virtual PipelineRecordResult Record(PipelineContext& context, const RenderPipelineRequest& request) = 0;
};
PipelineContext MakePipelineContext(RenderSystem& renderer, AppFrameContext& frame, Nullable<SceneManager*> scenes = nullptr);
/// Complete the requested exit state, preserving every returned command on recoverable failure.
/// The caller merges results and registers the batch with its acquired target, if any.
PipelineRecordResult RecordRenderPipeline(RenderPipeline& pipeline, PipelineContext& context, const RenderPipelineRequest& request);
}  // namespace radray
