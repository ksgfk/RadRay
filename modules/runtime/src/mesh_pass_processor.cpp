#include <radray/runtime/mesh_pass_processor.h>

#include <algorithm>
#include <bit>
#include <limits>

#include <radray/logger.h>

namespace radray {

MeshPassDrawListContext::MeshPassDrawListContext(Nullable<const MeshBatch*> validatedBatch) noexcept
    : _validatedBatch(validatedBatch) {}

CachedPassMeshDrawListContext::CachedPassMeshDrawListContext(CachedPassMeshDrawList& passDrawList, uint32_t batchIndex, Nullable<const MeshBatch*> validatedBatch) noexcept
    : MeshPassDrawListContext(validatedBatch), _batchIndex(batchIndex), _passDrawList(&passDrawList) {}

MeshDrawCommandBindingStorage& CachedPassMeshDrawListContext::GetBindingStorage() noexcept { return _passDrawList->_bindings; }

MeshDrawCommand& CachedPassMeshDrawListContext::AddCommand(const MeshBatchContext& context, size_t primaryElementIndex, MeshDrawCommand& initializer, uint32_t stateSortKey) {
    RADRAY_ASSERT(!_committed);
    RADRAY_ASSERT(initializer.ValidElementMask != 0 && primaryElementIndex == static_cast<size_t>(std::countr_zero(initializer.ValidElementMask)) && primaryElementIndex < context.Batch.Elements.size());
    _committed = true;
    auto& command = _passDrawList->AddCommand(_batchIndex, initializer);
    const auto& element = context.Batch.Elements[primaryElementIndex];
    auto& slot = _passDrawList->_batchSlots[_batchIndex];
    slot.StateSortKey = stateSortKey;
    slot.FirstInstance = element.FirstInstance;
    slot.InstanceCount = element.InstanceCount;
    return command;
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

MeshDrawCommand& DynamicMeshPassDrawListContext::AddCommand(const MeshBatchContext& context, size_t primaryElementIndex, MeshDrawCommand& initializer, uint32_t stateSortKey) {
    RADRAY_ASSERT(!_finalized);
    RADRAY_ASSERT(!initializer.BindingStorage || initializer.BindingStorage.Get() == &GetBindingStorage());
    RADRAY_ASSERT(initializer.ValidElementMask != 0 && primaryElementIndex == static_cast<size_t>(std::countr_zero(initializer.ValidElementMask)) && primaryElementIndex < context.Batch.Elements.size());
    auto& command = Commands.emplace_back(initializer);
    initializer.PayloadIndex = kInvalidBindingIndex;
    initializer.VertexBuffersIndex = kInvalidBindingIndex;
    initializer.ExtraElementsIndex = kInvalidBindingIndex;
    initializer.BindingStorage = nullptr;
    if (_visibleCommands) {
        _pendingVisibleCommands.push_back({Commands.size() - 1, _visibleCommands->size()});
        const auto& element = context.Batch.Elements[primaryElementIndex];
        _visibleCommands->push_back({.SortKey = ComposeMeshDrawSortKey(_sortMode, stateSortKey, context.VisibilitySortKey),
                                     .FirstInstance = element.FirstInstance,
                                     .InstanceCount = element.InstanceCount,
                                     .ElementMask = command.ValidElementMask,
                                     .UseElementFirstInstance = true,
                                     .UseElementInstanceCount = true});
    }
    return command;
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

void MeshPassProcessor::BuildMeshDrawCommands(const MeshBatchContext& context, const MeshDrawCommandSharedState& sharedState, MeshPassDrawListContext& output) {
    const auto& batch = context.Batch;
#if RADRAY_IS_DEBUG
    if (batch.Elements.size() > 32) RADRAY_ABORT("MeshPassProcessor: batch exceeds 32 elements");
#endif
    const auto firstElement = std::find_if(batch.Elements.begin(), batch.Elements.end(), [](const MeshBatchElement& element) { return element.IndexCount != 0 && element.InstanceCount != 0; });
    if (firstElement == batch.Elements.end()) return;
#if RADRAY_IS_DEBUG
    if (!batch.VertexInput || !ValidateVertexInput(*batch.VertexInput, sharedState.AllowDeferredInstanceStream)) RADRAY_ABORT("MeshPassProcessor: invalid vertex input");
    if (sharedState.PushConstants && (sharedState.PushConstants->Data.empty() || !sharedState.PushConstants->Data.data())) RADRAY_ABORT("MeshPassProcessor: push constants require nonempty data");
#endif
    MeshDrawCommand initializer;
    MeshDrawCommandBindings bindings;
    initializer.Pipeline = sharedState.Pipeline;
    initializer.StencilReference = sharedState.StencilReference;
    bindings.ParameterSets.assign(sharedState.ParameterSets.begin(), sharedState.ParameterSets.end());
    if (sharedState.PushConstants) {
        const auto& source = *sharedState.PushConstants;
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
#if RADRAY_IS_DEBUG
    const VisibleMeshDrawCommand visibleDefaults{.FirstInstance = firstElement->FirstInstance, .InstanceCount = firstElement->InstanceCount, .UseElementFirstInstance = true, .UseElementInstanceCount = true};
    if (!MeshDrawCommandBindingStorage::ValidateConstruction(initializer, bindings, visibleDefaults, !output.HasValidatedElements(batch))) RADRAY_ABORT("MeshPassProcessor: invalid mesh draw command construction");
#endif
    if (sharedState.AllowDeferredInstanceStream) {
        size_t instanceLayouts = 0;
        for (const auto& layout : batch.VertexInput->Layouts) {
            if (layout.StepMode != render::VertexStepMode::Instance) continue;
            ++instanceLayouts;
            bindings.InstanceStreamBinding = layout.Binding;
        }
        if (instanceLayouts != 1) bindings.InstanceStreamBinding.reset();
    }
    auto& storage = output.GetBindingStorage();
    storage.Store(initializer, bindings);
    const auto stateSortKey = sharedState.CustomStateSortKey ? *sharedState.CustomStateSortKey : storage.GetStateSortKey(initializer);
    output.AddCommand(context, static_cast<size_t>(firstElement - batch.Elements.begin()), initializer, stateSortKey);
}

}  // namespace radray
