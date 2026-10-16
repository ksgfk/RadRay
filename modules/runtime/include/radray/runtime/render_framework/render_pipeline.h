#pragma once

#include <chrono>
#include <radray/runtime/gpu_cmd_allocator.h>
#include <radray/runtime/gpu_frame_resources.h>
#include <radray/runtime/render_framework/scene_view.h>
#include <radray/runtime/render_framework/render_output.h>
#include <radray/runtime/render_framework/renderer.h>

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
    radray::Renderer& Renderer;
    FrameBuilder& Builder;
    PipelineFrameInfo Frame;
    IGpuCmdAllocator& Commands;
    GpuFrameResources& Resources;
    Nullable<SceneManager*> Scenes{nullptr};
    Nullable<IRequiredGpuWork*> RequiredWork{nullptr};
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
    class Plan : public FramePreparation {
    public:
        virtual ~Plan() noexcept = default;
        virtual bool Admit(PipelineContext&) { return true; }
        virtual bool PreparePipeline(PipelineContext&) { return true; }
        virtual PipelineRecordResult Record(PipelineContext& context) = 0;
        bool Prepare(FrameBuilder& builder) final;
        static std::optional<FramePreparationId> Collect(unique_ptr<Plan> owner, PipelineContext& context);

    private:
        Nullable<Renderer*> _renderer{nullptr};
        Nullable<IGpuCmdAllocator*> _commands{nullptr};
        Nullable<GpuFrameResources*> _resources{nullptr};
        Nullable<SceneManager*> _scenes{nullptr};
        Nullable<IRequiredGpuWork*> _requiredWork{nullptr};
        PipelineFrameInfo _frame{};
    };
    /// Collect every pipeline/output into the same builder before its single Compile call.
    std::optional<FramePreparationId> Collect(PipelineContext& context, const RenderPipelineRequest& request);
    virtual unique_ptr<Plan> CreatePlan(const RenderPipelineRequest& request) = 0;
};
PipelineContext MakePipelineContext(Renderer& renderer, FrameBuilder& builder, AppFrameContext& frame, Nullable<SceneManager*> scenes = nullptr);
/// Complete the requested exit state, preserving every returned command on recoverable failure.
/// The caller merges results and registers the batch with its acquired target, if any.
PipelineRecordResult RecordRenderPipeline(FramePreparationId plan, PipelineContext& context, const RenderPipelineRequest& request);
}  // namespace radray
