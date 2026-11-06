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

CachedPassMeshDrawListContext::CachedPassMeshDrawListContext(CachedPassMeshDrawList& passDrawList, uint32_t batchIndex) noexcept
    : _batchIndex(batchIndex), _passDrawList(&passDrawList) {}

MeshDrawCommandBindingStorage& CachedPassMeshDrawListContext::GetBindingStorage() noexcept { return _passDrawList->_bindings; }

MeshDrawCommand& CachedPassMeshDrawListContext::AddCommand(MeshDrawCommand& initializer, uint32_t stateSortKey) {
    RADRAY_ASSERT(!_pending);
    _pending = true;
    _pendingSortKey = stateSortKey;
    return _passDrawList->AddCommand(_batchIndex, initializer);
}

void CachedPassMeshDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(_pending && _passDrawList->GetCommand(_batchIndex).Get() == &command);
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    const auto& element = context.Batch.Elements[elementIndex];
    auto& slot = _passDrawList->_batchSlots[_batchIndex];
    slot.StateSortKey = _pendingSortKey;
    slot.FirstInstance = element.FirstInstance;
    slot.InstanceCount = element.InstanceCount;
    _pending = false;
}

DynamicMeshPassDrawListContext::DynamicMeshPassDrawListContext(vector<VisibleMeshDrawCommand>& visibleCommands, MeshPassSortMode sortMode) noexcept
    : _visibleCommands(&visibleCommands), _sortMode(sortMode) {}

void DynamicMeshPassDrawListContext::SetSortMode(MeshPassSortMode mode) noexcept {
    RADRAY_ASSERT(Commands.empty() || _sortMode == mode);
    _sortMode = mode;
}

void DynamicMeshPassDrawListContext::BindVisibleCommands(vector<VisibleMeshDrawCommand>& visibleCommands) noexcept {
    _visibleCommands = &visibleCommands;
}

MeshDrawCommandBindingStorage& DynamicMeshPassDrawListContext::GetBindingStorage() noexcept {
    if (!_bindings) _bindings = make_unique<MeshDrawCommandBindingStorage>();
    return *_bindings;
}

MeshDrawCommand& DynamicMeshPassDrawListContext::AddCommand(MeshDrawCommand& initializer, uint32_t stateSortKey) {
    RADRAY_ASSERT(!_finalized);
    RADRAY_ASSERT(!initializer.BindingStorage || initializer.BindingStorage.Get() == &GetBindingStorage());
    _pendingSortKey = stateSortKey;
    auto& command = Commands.emplace_back(initializer);
    initializer.PayloadIndex = kInvalidBindingIndex;
    initializer.VertexBuffersIndex = kInvalidBindingIndex;
    initializer.ExtraElementsIndex = kInvalidBindingIndex;
    initializer.BindingStorage = nullptr;
    return command;
}

void DynamicMeshPassDrawListContext::FinalizeCommand(const MeshBatchContext& context, size_t elementIndex, MeshDrawCommand& command) {
    RADRAY_ASSERT(!_finalized && !Commands.empty() && &Commands.back() == &command);
    MeshPassDrawListContext::FinalizeCommand(context, elementIndex, command);
    if (_visibleCommands) {
        _pendingVisibleCommands.push_back({Commands.size() - 1, _visibleCommands->size()});
        const auto& element = context.Batch.Elements[elementIndex];
        _visibleCommands->push_back({.SortKey = ComposeMeshDrawSortKey(_sortMode, _pendingSortKey, context.VisibilitySortKey),
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
    _drawListContext->_buildFailed = true;
    const auto& batch = context.Batch;
    if (batch.Elements.size() > 32) return false;
    const auto firstElement = std::find_if(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) { return element.IndexCount != 0 && element.InstanceCount != 0; });
    if (firstElement == batch.Elements.end()) {
        _drawListContext->_buildFailed = false;
        return true;
    }
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
    bindings.AllElementsSharePrimaryIndexBuffer = std::all_of(bindings.ExtraElements.begin(), bindings.ExtraElements.end(), [&](const auto& element) {
        return element.IndexBuffer.Target == initializer.IndexBuffer.Target && element.IndexBuffer.Offset == initializer.IndexBuffer.Offset && element.IndexBuffer.Stride == initializer.IndexBuffer.Stride;
    });
    const uint32_t primitiveSize = batch.Topology == render::PrimitiveTopology::TriangleList ? 3 : batch.Topology == render::PrimitiveTopology::LineList ? 2
                                                                                               : batch.Topology == render::PrimitiveTopology::PointList  ? 1
                                                                                                                                                         : 0;
    if (sharedState.AllowElementMerging && primitiveSize != 0) {
        uint64_t runCount = batch.Elements.front().IndexCount;
        for (size_t index = 0; index + 1 < batch.Elements.size(); ++index) {
            const auto& a = batch.Elements[index];
            const auto& b = batch.Elements[index + 1];
            const bool merge = a.IndexCount != 0 && b.IndexCount != 0 && a.IndexCount % primitiveSize == 0 && b.IndexCount % primitiveSize == 0 &&
                               a.InstanceCount == 1 && b.InstanceCount == 1 && a.FirstInstance == b.FirstInstance &&
                               a.IndexBuffer == b.IndexBuffer && a.IndexBufferOffset == b.IndexBufferOffset && a.IndexStride == b.IndexStride &&
                               a.VertexOffset == b.VertexOffset && uint64_t{a.FirstIndex} + a.IndexCount == b.FirstIndex &&
                               runCount + b.IndexCount <= std::numeric_limits<uint32_t>::max();
            if (merge) {
                bindings.ContiguousMergeMask |= uint32_t{1} << index;
                runCount += b.IndexCount;
            } else
                runCount = b.IndexCount;
        }
    }
    const VisibleMeshDrawCommand visibleDefaults{.FirstInstance = firstElement->FirstInstance, .InstanceCount = firstElement->InstanceCount, .UseElementFirstInstance = true, .UseElementInstanceCount = true};
    if (!MeshDrawCommandBindingStorage::ValidateConstruction(initializer, bindings, visibleDefaults, !DrawListContext().HasValidatedElements(batch))) return false;
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
    const auto stateSortKey = sharedState.CustomStateSortKey ? *sharedState.CustomStateSortKey : storage.GetStateSortKey(initializer);
    auto& command = DrawListContext().AddCommand(initializer, stateSortKey);
    DrawListContext().FinalizeCommand(context, static_cast<size_t>(firstElement - batch.Elements.begin()), command);
    _drawListContext->_buildFailed = false;
    return true;
}

}  // namespace radray
