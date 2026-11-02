#include <radray/runtime/mesh_pass_processor.h>

#include <algorithm>
#include <limits>

namespace radray {
namespace {

bool ValidateMeshBatchElement(const MeshBatchElement& element) noexcept {
    if (!element.IndexBuffer || !element.IndexBuffer->IsValid() ||
        (element.IndexStride != 2 && element.IndexStride != 4) || element.IndexBufferOffset % element.IndexStride != 0 ||
        uint64_t{element.FirstInstance} + element.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
    const auto desc = element.IndexBuffer->GetDesc();
    return desc.Usage.HasFlag(render::BufferUse::Index) &&
           uint64_t{element.IndexBufferOffset} + (uint64_t{element.FirstIndex} + element.IndexCount) * element.IndexStride <= desc.Size;
}

}  // namespace

void MeshPassDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    const auto& element = context.Batch.Elements[elementIndex];
    command.IndexBuffer = {element.IndexBuffer.Get(), element.IndexBufferOffset, element.IndexStride};
    command.IndexCount = element.IndexCount;
    command.FirstIndex = element.FirstIndex;
    command.VertexOffset = element.VertexOffset;
    command.FirstInstance = element.FirstInstance;
    command.InstanceCount = element.InstanceCount;
}

CachedPassMeshDrawListContext::CachedPassMeshDrawListContext(MeshPassId passId, CachedPassMeshDrawList& passDrawList, MeshBatch& targetBatch) noexcept
    : _passId(passId), _passDrawList(&passDrawList), _targetBatch(&targetBatch) {}

MeshDrawCommand& CachedPassMeshDrawListContext::AddCommand(const MeshDrawCommand& initializer) {
    RADRAY_ASSERT(!_pendingHandle.IsValid());
    _pendingHandle = _passDrawList->Commands.Emplace(initializer);
    return _passDrawList->Commands.Get(_pendingHandle);
}

void CachedPassMeshDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(_pendingHandle.IsValid() && &_passDrawList->Commands.Get(_pendingHandle) == &command);
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    _targetBatch->CachedCommands.push_back({_passId, static_cast<uint32_t>(elementIndex), _pendingHandle, command.BaseSortKey, command.FirstInstance, command.InstanceCount});
    _pendingHandle = {};
}

DynamicMeshPassDrawListContext::DynamicMeshPassDrawListContext(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept
    : _visibleCommands(&visibleCommands) {}

void DynamicMeshPassDrawListContext::BindVisibleCommands(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept {
    _visibleCommands = &visibleCommands;
}

MeshDrawCommand& DynamicMeshPassDrawListContext::AddCommand(const MeshDrawCommand& initializer) {
    return Commands.emplace_back(initializer);
}

void DynamicMeshPassDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(!Commands.empty() && &Commands.back() == &command);
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    if (_visibleCommands) {
        RADRAY_ASSERT(Commands.size() <= std::numeric_limits<uint32_t>::max());
        _visibleCommands->push_back({.Source = MeshDrawCommandSource::Dynamic,
                                     .CommandHandle = {static_cast<uint32_t>(Commands.size() - 1), 0},
                                     .SortKey = command.BaseSortKey,
                                     .FirstInstance = command.FirstInstance,
                                     .InstanceCount = command.InstanceCount});
    }
}

void DynamicMeshPassDrawListContext::Clear() noexcept { Commands.clear(); }

MeshPassProcessor::MeshPassProcessor(MeshPassDrawListContext& drawListContext) noexcept : _drawListContext(&drawListContext) {}

void MeshPassProcessor::SetDrawListContext(MeshPassDrawListContext& drawListContext) noexcept { _drawListContext = &drawListContext; }

bool MeshPassProcessor::BuildMeshDrawCommands(const MeshBatchContext& context, const MeshDrawCommandSharedState& sharedState) {
    const auto& batch = context.Batch;
    const auto firstElement = std::find_if(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) { return element.IndexCount != 0 && element.InstanceCount != 0; });
    if (firstElement == batch.Elements.end()) return true;
    if (!batch.VertexInput || !ValidateVertexInput(*batch.VertexInput, sharedState.AllowDeferredInstanceStream) ||
        batch.Elements.size() > std::numeric_limits<uint32_t>::max()) return false;
    MeshDrawCommand initializer;
    initializer.Pipeline = sharedState.Pipeline;
    initializer.StencilReference = sharedState.StencilReference;
    initializer.BaseSortKey = sharedState.BaseSortKey;
    initializer.ParameterSets.assign(sharedState.ParameterSets.begin(), sharedState.ParameterSets.end());
    if (sharedState.PushConstants) {
        const auto& source = *sharedState.PushConstants;
        if (source.Data.empty() || !source.Data.data()) return false;
        MeshPushConstantCommandBinding push{source.PushConstantId, source.Offset, {}};
        push.Data.assign(source.Data.begin(), source.Data.end());
        initializer.PushConstants = std::move(push);
    }
    for (const auto& stream : batch.VertexInput->Streams) {
        initializer.VertexBuffers.push_back({stream.Binding, {stream.Buffer.Get(), stream.Offset, stream.Size}});
    }
    initializer.IndexBuffer = {firstElement->IndexBuffer.Get(), firstElement->IndexBufferOffset, firstElement->IndexStride};
    initializer.IndexCount = firstElement->IndexCount;
    initializer.FirstIndex = firstElement->FirstIndex;
    initializer.VertexOffset = firstElement->VertexOffset;
    initializer.FirstInstance = firstElement->FirstInstance;
    initializer.InstanceCount = firstElement->InstanceCount;
    // Validate shared bindings and the first element once, then reject any invalid remaining geometry before emitting.
    if (!ValidateMeshDrawCommand(initializer)) return false;
    for (auto elementIt = firstElement + 1; elementIt != batch.Elements.end(); ++elementIt) {
        const auto& element = *elementIt;
        if (element.IndexCount == 0 || element.InstanceCount == 0) continue;
        if (!ValidateMeshBatchElement(element)) return false;
    }
    if (sharedState.AllowDeferredInstanceStream) {
        size_t instanceLayouts = 0;
        for (const auto& layout : batch.VertexInput->Layouts) {
            if (layout.StepMode != render::VertexStepMode::Instance) continue;
            ++instanceLayouts;
            initializer.InstanceStreamBinding = layout.Binding;
        }
        if (instanceLayouts != 1) initializer.InstanceStreamBinding.reset();
    }
    for (size_t index = 0; index < batch.Elements.size(); ++index) {
        if (batch.Elements[index].IndexCount == 0 || batch.Elements[index].InstanceCount == 0) continue;
        auto& command = DrawListContext().AddCommand(initializer);
        DrawListContext().FinalizeCommand(context, index, command);
    }
    return true;
}

}  // namespace radray
