#include <radray/runtime/render_framework/render_pipeline.h>
#include <radray/runtime/gpu_system.h>
#include <radray/logger.h>

namespace radray {
PipelineContext MakePipelineContext(Renderer& renderer, FrameBuilder& builder, AppFrameContext& frame, Nullable<SceneManager*> scenes) {
    return {renderer, builder, {frame.FlightIndex(), frame.FrameSerial(), frame.DeltaTime()}, frame.GetCmdAllocator(), frame.GetGpuFrameResources(), scenes, &frame};
}
std::optional<FramePreparationId> RenderPipeline::Plan::Collect(unique_ptr<Plan> owner, PipelineContext& context) {
    if (!owner || !context.Builder.IsFor(context.Renderer, context.Frame.FlightIndex, context.Frame.FrameSerial) || !owner->Admit(context)) return std::nullopt;
    owner->_renderer = &context.Renderer;
    owner->_commands = &context.Commands;
    owner->_resources = &context.Resources;
    owner->_scenes = context.Scenes;
    owner->_requiredWork = context.RequiredWork;
    owner->_frame = context.Frame;
    return context.Builder.AddPreparation(std::move(owner));
}
bool RenderPipeline::Plan::Prepare(FrameBuilder& builder) {
    if (!_renderer || !_commands || !_resources) return false;
    PipelineContext context{*_renderer, builder, _frame, *_commands, *_resources, _scenes, _requiredWork};
    return PreparePipeline(context);
}
std::optional<FramePreparationId> RenderPipeline::Collect(PipelineContext& context, const RenderPipelineRequest& request) {
    auto plan = CreatePlan(request);
    return Plan::Collect(std::move(plan), context);
}
PipelineRecordResult RecordRenderPipeline(FramePreparationId id, PipelineContext& context, const RenderPipelineRequest& request) {
    if (request.Output && !ValidateRenderOutput(*request.Output, *context.Resources.GetDevice())) return {.Status = PipelineRecordStatus::RecoverableFailure, .Commands = {}, .OutputState = {}};
    const auto prepared = context.Builder.GetPreparation(id);
    const auto plan = prepared ? dynamic_cast<RenderPipeline::Plan*>(prepared.Get()) : nullptr;
    auto result = plan ? plan->Record(context) : PipelineRecordResult{PipelineRecordStatus::RecoverableFailure};
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
