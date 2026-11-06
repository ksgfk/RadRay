#include <radray/runtime/mesh_pass_processor.h>

#include <algorithm>
#include <bit>

namespace radray {

void MeshPassDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(command.ValidElementMask != 0 && elementIndex == static_cast<size_t>(std::countr_zero(command.ValidElementMask)) && elementIndex < context.Batch.Elements.size());
    const auto& element = context.Batch.Elements[elementIndex];
    command.IndexBuffer = {element.IndexBuffer.Get(), element.IndexBufferOffset, element.IndexStride};
    command.IndexCount = element.IndexCount;
    command.FirstIndex = element.FirstIndex;
    command.VertexOffset = element.VertexOffset;
}

CachedPassMeshDrawListContext::CachedPassMeshDrawListContext(MeshPassId passId, CachedPassMeshDrawList& passDrawList, MeshBatch& targetBatch) noexcept
    : _passId(passId), _passDrawList(&passDrawList), _targetBatch(&targetBatch) {}

MeshDrawCommandBindingStorage& CachedPassMeshDrawListContext::GetBindingStorage() noexcept { return _passDrawList->Bindings; }

MeshDrawCommand& CachedPassMeshDrawListContext::AddCommand(MeshDrawCommand& initializer, uint64_t baseSortKey) {
    RADRAY_ASSERT(!_pendingHandle.IsValid());
    RADRAY_ASSERT(std::none_of(_targetBatch->CachedCommands.begin(), _targetBatch->CachedCommands.end(), [&](const auto& info) { return info.Pass == _passId; }));
    _pendingSortKey = baseSortKey;
    _pendingHandle = _passDrawList->AddCommand(initializer);
    return _passDrawList->Commands.Get(_pendingHandle);
}

void CachedPassMeshDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(_pendingHandle.IsValid() && &_passDrawList->Commands.Get(_pendingHandle) == &command);
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    const auto& element = context.Batch.Elements[elementIndex];
    _targetBatch->CachedCommands.push_back({_passId, _pendingHandle, _pendingSortKey, element.FirstInstance, element.InstanceCount});
    _pendingHandle = {};
}

DynamicMeshPassDrawListContext::DynamicMeshPassDrawListContext(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept
    : _visibleCommands(&visibleCommands) {}

void DynamicMeshPassDrawListContext::BindVisibleCommands(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept {
    _visibleCommands = &visibleCommands;
}

MeshDrawCommandBindingStorage& DynamicMeshPassDrawListContext::GetBindingStorage() noexcept {
    if (!_bindings) _bindings = make_unique<MeshDrawCommandBindingStorage>();
    return *_bindings;
}

MeshDrawCommand& DynamicMeshPassDrawListContext::AddCommand(MeshDrawCommand& initializer, uint64_t baseSortKey) {
    RADRAY_ASSERT(!_finalized);
    RADRAY_ASSERT(!initializer.BindingStorage || initializer.BindingStorage.Get() == &GetBindingStorage());
    _pendingSortKey = baseSortKey;
    auto& command = Commands.emplace_back(initializer);
    initializer.PayloadHandle = {};
    initializer.VertexBuffersHandle = {};
    initializer.ExtraElementsHandle = {};
    initializer.BindingStorage = nullptr;
    return command;
}

void DynamicMeshPassDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(!_finalized && !Commands.empty() && &Commands.back() == &command);
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    if (_visibleCommands) {
        _pendingVisibleCommands.push_back({Commands.size() - 1, _visibleCommands->size()});
        const auto& element = context.Batch.Elements[elementIndex];
        _visibleCommands->push_back({.SortKey = _pendingSortKey,
                                     .FirstInstance = element.FirstInstance,
                                     .InstanceCount = element.InstanceCount,
                                     .ElementMask = command.ValidElementMask,
                                     .UseElementFirstInstance = true,
                                     .UseElementInstanceCount = true});
    }
}

void DynamicMeshPassDrawListContext::FinalizeVisibleCommands() noexcept {
    if (_finalized) return;
    for (const auto& pending : _pendingVisibleCommands) {
        RADRAY_ASSERT(_visibleCommands && pending.VisibleIndex < _visibleCommands->size() && pending.CommandIndex < Commands.size());
        auto& visible = (*_visibleCommands)[pending.VisibleIndex];
        visible.CachedCommand = &Commands[pending.CommandIndex];
    }
    _pendingVisibleCommands.clear();
    _finalized = true;
}

void DynamicMeshPassDrawListContext::Clear() noexcept {
    Commands.clear();
    if (_bindings) _bindings->Clear();
    _pendingVisibleCommands.clear();
    _finalized = false;
}

MeshPassProcessor::MeshPassProcessor(MeshPassDrawListContext& drawListContext) noexcept : _drawListContext(&drawListContext) {}

void MeshPassProcessor::SetDrawListContext(MeshPassDrawListContext& drawListContext) noexcept { _drawListContext = &drawListContext; }

bool MeshPassProcessor::BuildMeshDrawCommands(const MeshBatchContext& context, const MeshDrawCommandSharedState& sharedState) {
    const auto& batch = context.Batch;
    if (batch.Elements.size() > 32) return false;
    const auto firstElement = std::find_if(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) { return element.IndexCount != 0 && element.InstanceCount != 0; });
    if (firstElement == batch.Elements.end()) return true;
    if (!batch.VertexInput || !ValidateVertexInput(*batch.VertexInput, sharedState.AllowDeferredInstanceStream)) return false;
    MeshDrawCommand initializer;
    MeshDrawCommandBindings bindings;
    initializer.Pipeline = sharedState.Pipeline;
    initializer.StencilReference = sharedState.StencilReference;
    bindings.ParameterSets.assign(sharedState.ParameterSets.begin(), sharedState.ParameterSets.end());
    if (sharedState.PushConstants) {
        const auto& source = *sharedState.PushConstants;
        if (source.Data.empty() || !source.Data.data()) return false;
        MeshPushConstantCommandBinding push{source.PushConstantId, source.Offset, {}};
        push.Data.assign(source.Data.begin(), source.Data.end());
        bindings.PushConstants = std::move(push);
    }
    for (const auto& stream : batch.VertexInput->Streams) {
        bindings.VertexBuffers.push_back({stream.Binding, {stream.Buffer.Get(), stream.Offset, stream.Size}});
    }
    std::sort(bindings.VertexBuffers.begin(), bindings.VertexBuffers.end(), [](const auto& a, const auto& b) { return a.Binding < b.Binding; });
    initializer.IndexBuffer = {firstElement->IndexBuffer.Get(), firstElement->IndexBufferOffset, firstElement->IndexStride};
    initializer.IndexCount = firstElement->IndexCount;
    initializer.FirstIndex = firstElement->FirstIndex;
    initializer.VertexOffset = firstElement->VertexOffset;
    initializer.ValidElementMask = 0;
    bool hasDifferentInstances = false;
    for (size_t index = 0; index < batch.Elements.size(); ++index) {
        const auto& element = batch.Elements[index];
        if (element.IndexCount == 0 || element.InstanceCount == 0) continue;
        initializer.ValidElementMask |= uint32_t{1} << index;
        if (&element != &*firstElement) bindings.ExtraElements.push_back({{element.IndexBuffer.Get(), element.IndexBufferOffset, element.IndexStride}, element.IndexCount, element.FirstIndex, element.VertexOffset, static_cast<uint32_t>(index)});
        bindings.ElementInstances.push_back({element.FirstInstance, element.InstanceCount});
        hasDifferentInstances |= element.FirstInstance != firstElement->FirstInstance || element.InstanceCount != firstElement->InstanceCount;
    }
    if (!hasDifferentInstances) bindings.ElementInstances.clear();
    const VisibleMeshDrawCommand visibleDefaults{.FirstInstance = firstElement->FirstInstance, .InstanceCount = firstElement->InstanceCount, .UseElementFirstInstance = true, .UseElementInstanceCount = true};
    if (!ValidateMeshDrawCommand(initializer, bindings, visibleDefaults)) return false;
    if (sharedState.AllowDeferredInstanceStream) {
        size_t instanceLayouts = 0;
        for (const auto& layout : batch.VertexInput->Layouts) {
            if (layout.StepMode != render::VertexStepMode::Instance) continue;
            ++instanceLayouts;
            bindings.InstanceStreamBinding = layout.Binding;
        }
        if (instanceLayouts != 1) bindings.InstanceStreamBinding.reset();
    }
    auto& storage = DrawListContext().GetBindingStorage();
    storage.Store(initializer, bindings);
    auto& command = DrawListContext().AddCommand(initializer, sharedState.BaseSortKey);
    DrawListContext().FinalizeCommand(context, static_cast<size_t>(firstElement - batch.Elements.begin()), command);
    return true;
}

}  // namespace radray
