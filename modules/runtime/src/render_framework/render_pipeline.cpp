#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/logger.h>

namespace radray {
PipelineContext MakePipelineContext(RenderSystem& renderer, AppFrameContext& frame, Nullable<SceneManager*> scenes) {
    return {renderer, {frame.FlightIndex(), frame.FrameSerial(), frame.DeltaTime()}, frame.GetCmdAllocator(), frame.GetGpuFrameResources(), scenes};
}
PipelineRecordResult RecordRenderPipeline(RenderPipeline& pipeline, PipelineContext& context, const RenderPipelineRequest& request) {
    if (request.Output && !ValidateRenderOutput(*request.Output, *context.Resources.GetDevice())) return {.Status = PipelineRecordStatus::RecoverableFailure, .Commands = {}, .OutputState = {}};
    auto result = pipeline.Record(context, request);
    if (request.Output) {
        if (!result.OutputState) result.OutputState = InitialRenderOutputState(*request.Output);
        if (!IsRenderOutputFinished(*request.Output, *result.OutputState) || result.Commands.empty()) {
            auto* command = context.Commands.Allocate();
            TransitionRenderOutput(command, *request.Output, *result.OutputState, true);
            context.Commands.Return(std::span{&command, 1});
            result.Commands.push_back(command);
        }
    }
    return result;
}
}  // namespace radray
