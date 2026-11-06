#include <radray/runtime/mesh_draw_command.h>

#include <algorithm>
#include <bit>
#include <utility>
#include <limits>

#include <radray/profiler.h>

namespace radray {
namespace {

bool ValidateVertexBuffer(const render::VertexBufferView& view) noexcept {
    if (!view.Target || !view.Target->IsValid() || view.Size == 0 || view.Size > std::numeric_limits<uint32_t>::max()) return false;
    const auto desc = view.Target->GetDesc();
    return desc.Usage.HasFlag(render::BufferUse::Vertex) && view.Offset <= desc.Size && view.Size <= desc.Size - view.Offset;
}

bool ValidateParameterSets(std::span<const MeshShaderParameterBinding> bindings) noexcept {
    for (size_t index = 0; index < bindings.size(); ++index) {
        const auto& binding = bindings[index];
        if (!binding.ParameterSet || !binding.ParameterSet->IsValid()) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (bindings[previous].GroupIndex == binding.GroupIndex) return false;
        }
        for (size_t offset = 0; offset < binding.DynamicOffsets.size(); ++offset) {
            for (size_t previous = 0; previous < offset; ++previous) {
                if (binding.DynamicOffsets[previous].BindingId == binding.DynamicOffsets[offset].BindingId &&
                    binding.DynamicOffsets[previous].ArrayIndex == binding.DynamicOffsets[offset].ArrayIndex) return false;
            }
        }
    }
    return true;
}

bool ValidateSubmitArgs(const MeshPassSubmitArgs& submitArgs) noexcept {
    return ValidateParameterSets(submitArgs.ParameterSets) &&
           (!submitArgs.InstanceStream || ValidateVertexBuffer(submitArgs.InstanceStream->View));
}

bool SameVertexBufferView(const render::VertexBufferView& a, const render::VertexBufferView& b) noexcept {
    return a.Target == b.Target && a.Offset == b.Offset && a.Size == b.Size;
}

bool SameIndexBufferView(const render::IndexBufferView& a, const render::IndexBufferView& b) noexcept {
    return a.Target == b.Target && a.Offset == b.Offset && a.Stride == b.Stride;
}

bool SameParameterBinding(const MeshShaderParameterBinding& a, const MeshShaderParameterBinding& b) noexcept {
    return a.GroupIndex == b.GroupIndex && a.ParameterSet.Get() == b.ParameterSet.Get() && a.DynamicOffsets == b.DynamicOffsets;
}

struct MeshDrawCommandStateCache {
    Nullable<render::GraphicsPipelineState*> Pipeline{nullptr};
    std::optional<render::IndexBufferView> IndexBuffer{};
    std::optional<uint8_t> StencilReference{};
    array<render::VertexBufferView, 32> BoundVertexSlots{};
    uint32_t BoundVertexMask{0};
    Nullable<const MeshDrawCommandBindingStorage*> LastVertexStorage{nullptr};
    SparseSetHandle LastVertexBuffersHandle{};
    InlineVector<MeshShaderParameterBinding, 4> CommandParameterSets;

    void OnPipelineChanged(render::GraphicsPipelineState* pipeline) noexcept {
        Pipeline = pipeline;
        BoundVertexMask = 0;
        LastVertexStorage = nullptr;
        LastVertexBuffersHandle = {};
        CommandParameterSets.clear();
    }

    bool UpdateVertexBuffer(const render::VertexBufferBinding& binding) noexcept {
        if (binding.Binding >= BoundVertexSlots.size()) return true;
        const auto bit = uint32_t{1} << binding.Binding;
        auto& slot = BoundVertexSlots[binding.Binding];
        if ((BoundVertexMask & bit) && SameVertexBufferView(slot, binding.View)) return false;
        slot = binding.View;
        BoundVertexMask |= bit;
        return true;
    }

    bool UpdateParameterSet(const MeshShaderParameterBinding& binding) {
        const auto found = std::find_if(CommandParameterSets.begin(), CommandParameterSets.end(), [&](const MeshShaderParameterBinding& cached) {
            return cached.GroupIndex == binding.GroupIndex;
        });
        if (found != CommandParameterSets.end()) {
            if (SameParameterBinding(*found, binding)) return false;
            *found = binding;
        } else {
            CommandParameterSets.push_back(binding);
        }
        return true;
    }
};

}  // namespace

void MeshDrawCommandBindingStorage::Store(MeshDrawCommand& command, const MeshDrawCommandBindings& bindings) {
    RADRAY_ASSERT(!command.VertexBuffersHandle.IsValid() && !command.PayloadHandle.IsValid() && !command.ExtraElementsHandle.IsValid() && !command.BindingStorage);
    if (!bindings.VertexBuffers.empty() || bindings.InstanceStreamBinding) {
        MeshVertexBufferBindings vertices{bindings.VertexBuffers, bindings.InstanceStreamBinding};
        const auto less = [](const auto& a, const auto& b) { return a.Binding < b.Binding; };
        if (!std::is_sorted(vertices.Bindings.begin(), vertices.Bindings.end(), less)) std::sort(vertices.Bindings.begin(), vertices.Bindings.end(), less);
        command.VertexBuffersHandle = VertexBuffersPool.Emplace(std::move(vertices));
    }
    if (!bindings.ParameterSets.empty() || bindings.PushConstants) {
        command.PayloadHandle = Payloads.Emplace(MeshDrawCommandPayload{bindings.ParameterSets, bindings.PushConstants});
    }
    if (!bindings.ExtraElements.empty() || !bindings.ElementInstances.empty()) {
        command.ExtraElementsHandle = ExtraElementsPool.Emplace(MeshDrawCommandElements{bindings.ExtraElements, bindings.ElementInstances});
    }
    if (command.VertexBuffersHandle.IsValid() || command.PayloadHandle.IsValid() || command.ExtraElementsHandle.IsValid()) command.BindingStorage = this;
}

void MeshDrawCommandBindingStorage::Destroy(const MeshDrawCommand& command) noexcept {
    RADRAY_ASSERT(!command.BindingStorage || command.BindingStorage.Get() == this);
    if (command.VertexBuffersHandle.IsValid()) VertexBuffersPool.Destroy(command.VertexBuffersHandle);
    if (command.PayloadHandle.IsValid()) Payloads.Destroy(command.PayloadHandle);
    if (command.ExtraElementsHandle.IsValid()) ExtraElementsPool.Destroy(command.ExtraElementsHandle);
}

void MeshDrawCommandBindingStorage::Clear() noexcept {
    VertexBuffersPool.Clear();
    Payloads.Clear();
    ExtraElementsPool.Clear();
}

Nullable<const MeshVertexBufferBindings*> MeshDrawCommandBindingStorage::GetVertexBuffers(SparseSetHandle handle) const noexcept {
    return VertexBuffersPool.TryGet(handle);
}

Nullable<const MeshDrawCommandElements*> MeshDrawCommandBindingStorage::GetExtraElements(SparseSetHandle handle) const noexcept {
    return ExtraElementsPool.TryGet(handle);
}

Nullable<const MeshDrawCommandPayload*> MeshDrawCommandBindingStorage::GetPayload(SparseSetHandle handle) const noexcept {
    return Payloads.TryGet(handle);
}

CachedPassMeshDrawList::CachedPassMeshDrawList(CachedPassMeshDrawList&& other) noexcept {
    *this = std::move(other);
}

CachedPassMeshDrawList& CachedPassMeshDrawList::operator=(CachedPassMeshDrawList&& other) noexcept {
    if (this != &other) {
        Commands = std::move(other.Commands);
        Bindings = std::move(other.Bindings);
        for (auto& command : Commands.Values()) {
            if (command.BindingStorage) command.BindingStorage = &Bindings;
        }
        other.Clear();
    }
    return *this;
}

SparseSetHandle CachedPassMeshDrawList::AddCommand(MeshDrawCommand& command) {
    RADRAY_ASSERT(!command.BindingStorage || command.BindingStorage.Get() == &Bindings);
    const auto handle = Commands.Emplace(command);
    command.BindingStorage = nullptr;
    command.PayloadHandle = {};
    command.VertexBuffersHandle = {};
    command.ExtraElementsHandle = {};
    return handle;
}

void CachedPassMeshDrawList::DestroyCommand(SparseSetHandle handle) noexcept {
    Bindings.Destroy(Commands.Get(handle));
    Commands.Destroy(handle);
}

void CachedPassMeshDrawList::Clear() noexcept {
    Commands.Clear();
    Bindings.Clear();
}

bool ValidateVertexInput(const VertexInput& input, bool allowDeferredInstanceStream) noexcept {
    InlineVector<render::VertexAttribute, 8> attributes;
    for (const auto& attribute : input.Attributes) {
        attributes.push_back({attribute.BufferBinding, attribute.Offset, attribute.Semantic,
                              attribute.SemanticIndex, attribute.Format, attribute.Location});
    }
    if (!render::ValidateVertexInputState({input.Layouts, {attributes.data(), attributes.size()}})) return false;
    size_t instanceLayouts = 0;
    size_t deferredStreams = 0;
    for (const auto& layout : input.Layouts) {
        if (layout.StepMode != render::VertexStepMode::Vertex && layout.StepMode != render::VertexStepMode::Instance) return false;
        if (layout.StepMode == render::VertexStepMode::Instance) ++instanceLayouts;
        const auto stream = std::find_if(input.Streams.begin(), input.Streams.end(), [&](const VertexInputStream& value) {
            return value.Binding == layout.Binding;
        });
        if (stream == input.Streams.end()) {
            if (allowDeferredInstanceStream && layout.StepMode == render::VertexStepMode::Instance) {
                ++deferredStreams;
                continue;
            }
            return false;
        }
        if (stream->Size < layout.ArrayStride) return false;
    }
    if (deferredStreams != 0 && instanceLayouts > 1) return false;
    for (size_t index = 0; index < input.Streams.size(); ++index) {
        const auto& stream = input.Streams[index];
        if (!ValidateVertexBuffer({stream.Buffer.Get(), stream.Offset, stream.Size}) ||
            std::none_of(input.Layouts.begin(), input.Layouts.end(), [&](const render::VertexBufferLayout& layout) { return layout.Binding == stream.Binding; })) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (input.Streams[previous].Binding == stream.Binding) return false;
        }
    }
    return true;
}

namespace {

// Pass-wide arguments have already been validated by the caller.
bool ValidateMeshDrawCommandImpl(const MeshDrawCommand& command, std::span<const render::VertexBufferBinding> vertexBuffers,
                                 std::span<const MeshShaderParameterBinding> parameterSets,
                                 Nullable<const MeshPushConstantCommandBinding*> pushConstants, std::optional<uint32_t> instanceStreamBinding,
                                 const MeshPassSubmitArgs& submitArgs) noexcept {
    const auto& indices = command.IndexBuffer;
    if (!command.Pipeline || !command.Pipeline->IsValid() || !indices.Target || !indices.Target->IsValid() || command.IndexCount == 0 ||
        (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
    const auto desc = indices.Target->GetDesc();
    const uint64_t end = uint64_t{indices.Offset} + (uint64_t{command.FirstIndex} + command.IndexCount) * indices.Stride;
    if (!desc.Usage.HasFlag(render::BufferUse::Index) || end > desc.Size) return false;
    bool hasDefaultInstanceStream = false;
    for (size_t index = 0; index < vertexBuffers.size(); ++index) {
        const auto& binding = vertexBuffers[index];
        const bool isInstanceStream = instanceStreamBinding && *instanceStreamBinding == binding.Binding;
        hasDefaultInstanceStream |= isInstanceStream;
        if (!(isInstanceStream && submitArgs.InstanceStream && submitArgs.InstanceStream->Binding == binding.Binding) && !ValidateVertexBuffer(binding.View)) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (vertexBuffers[previous].Binding == binding.Binding) return false;
        }
    }
    if (instanceStreamBinding && !hasDefaultInstanceStream &&
        (!submitArgs.InstanceStream || submitArgs.InstanceStream->Binding != *instanceStreamBinding)) return false;
    if (submitArgs.InstanceStream && (!instanceStreamBinding || submitArgs.InstanceStream->Binding != *instanceStreamBinding)) return false;
    if (!ValidateParameterSets({parameterSets.data(), parameterSets.size()})) return false;
    for (const auto& binding : parameterSets) {
        for (const auto& passBinding : submitArgs.ParameterSets) {
            if (binding.GroupIndex == passBinding.GroupIndex) return false;
        }
    }
    if (pushConstants) {
        const auto& push = *pushConstants;
        if (push.Data.empty() || push.Offset % 4 != 0 || push.Data.size() % 4 != 0 ||
            push.Data.size() > std::numeric_limits<uint32_t>::max() - push.Offset) return false;
    }
    return true;
}

bool ValidateElements(const MeshDrawCommand& command, std::span<const MeshDrawElement> extraElements,
                      std::span<const MeshDrawElementInstances> instances, const VisibleMeshDrawCommand& visible) noexcept {
    if (command.ValidElementMask == 0 || extraElements.size() + 1 != static_cast<size_t>(std::popcount(command.ValidElementMask)) ||
        (!instances.empty() && instances.size() != extraElements.size() + 1)) return false;
    uint32_t remaining = command.ValidElementMask & (command.ValidElementMask - 1);
    for (const auto& element : extraElements) {
        if (element.ElementIndex != static_cast<uint32_t>(std::countr_zero(remaining))) return false;
        remaining &= remaining - 1;
        const auto& indices = element.IndexBuffer;
        if (!indices.Target || !indices.Target->IsValid() || element.IndexCount == 0 ||
            (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
        const auto desc = indices.Target->GetDesc();
        if (!desc.Usage.HasFlag(render::BufferUse::Index) ||
            uint64_t{indices.Offset} + (uint64_t{element.FirstIndex} + element.IndexCount) * indices.Stride > desc.Size) return false;
    }
    for (const auto& instance : instances) {
        if (instance.InstanceCount != 0 && uint64_t{instance.FirstInstance} + instance.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
    }
    uint32_t mask = visible.ElementMask & command.ValidElementMask;
    while (mask != 0) {
        const uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
        mask &= mask - 1;
        const auto ordinal = static_cast<uint32_t>(std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1)));
        const auto first = !instances.empty() && visible.UseElementFirstInstance ? instances[ordinal].FirstInstance : visible.FirstInstance;
        const auto count = !instances.empty() && visible.UseElementInstanceCount ? instances[ordinal].InstanceCount : visible.InstanceCount;
        if (count != 0 && uint64_t{first} + count - 1 > std::numeric_limits<uint32_t>::max()) return false;
    }
    return true;
}

bool ValidateStoredMeshDrawCommand(const MeshDrawCommand& command,
                                   const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    const auto storage = command.BindingStorage;
    const auto vertices = storage ? storage->GetVertexBuffers(command.VertexBuffersHandle) : nullptr;
    const auto payload = storage ? storage->GetPayload(command.PayloadHandle) : nullptr;
    const auto elements = storage ? storage->GetExtraElements(command.ExtraElementsHandle) : nullptr;
    if ((command.VertexBuffersHandle.IsValid() && !vertices) || (command.PayloadHandle.IsValid() && !payload) ||
        (command.ExtraElementsHandle.IsValid() && !elements)) return false;
    if (vertices) {
        for (size_t index = 1; index < vertices->Bindings.size(); ++index) {
            if (vertices->Bindings[index - 1].Binding >= vertices->Bindings[index].Binding) return false;
        }
    }
    return ValidateMeshDrawCommandImpl(command,
                                       vertices ? std::span<const render::VertexBufferBinding>{vertices->Bindings.data(), vertices->Bindings.size()} : std::span<const render::VertexBufferBinding>{},
                                       payload ? std::span<const MeshShaderParameterBinding>{payload->ParameterSets.data(), payload->ParameterSets.size()} : std::span<const MeshShaderParameterBinding>{},
                                       payload && payload->PushConstants ? &*payload->PushConstants : nullptr,
                                       vertices ? vertices->InstanceStreamBinding : std::nullopt, submitArgs) &&
           ValidateElements(command, elements ? std::span<const MeshDrawElement>{elements->ExtraElements.data(), elements->ExtraElements.size()} : std::span<const MeshDrawElement>{},
                            elements ? std::span<const MeshDrawElementInstances>{elements->ElementInstances.data(), elements->ElementInstances.size()} : std::span<const MeshDrawElementInstances>{}, visibleCommand);
}

}  // namespace

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindings& bindings,
                             const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    return ValidateSubmitArgs(submitArgs) && ValidateMeshDrawCommandImpl(command, {bindings.VertexBuffers.data(), bindings.VertexBuffers.size()}, {bindings.ParameterSets.data(), bindings.ParameterSets.size()}, bindings.PushConstants ? &*bindings.PushConstants : nullptr, bindings.InstanceStreamBinding, submitArgs) &&
           ValidateElements(command, {bindings.ExtraElements.data(), bindings.ExtraElements.size()}, {bindings.ElementInstances.data(), bindings.ElementInstances.size()}, visibleCommand);
}

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const MeshDrawCommandBindingStorage& storage,
                             const VisibleMeshDrawCommand& visibleCommand, const MeshPassSubmitArgs& submitArgs) noexcept {
    return (!command.BindingStorage || command.BindingStorage.Get() == &storage) &&
           ValidateSubmitArgs(submitArgs) && ValidateStoredMeshDrawCommand(command, visibleCommand, submitArgs);
}

bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            const MeshPassSubmitArgs& submitArgs) noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.MeshDraw.Submit");
    RADRAY_PROFILE_PLOT("RadRay.MeshDraw.SubmitCommands", static_cast<int64_t>(visibleCommands.size()));
    if (!encoder.IsValid()) return false;
#if RADRAY_IS_DEBUG
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.MeshDraw.Validate");
        if (!ValidateSubmitArgs(submitArgs)) return false;
        for (const auto& visible : visibleCommands) {
            const auto command = visible.CachedCommand;
            if (!command || !ValidateStoredMeshDrawCommand(*command, visible, submitArgs)) return false;
        }
    }
#endif
    MeshDrawCommandStateCache stateCache;
    for (const auto& visible : visibleCommands) {
        if (visible.InstanceCount == 0 && !visible.UseElementInstanceCount) continue;
        const auto commandPtr = visible.CachedCommand;
        if (!commandPtr) return false;
        const auto& command = *commandPtr;
        uint32_t mask = visible.ElementMask & command.ValidElementMask;
        if (mask == 0) continue;
        const auto storage = command.BindingStorage;
        const auto payload = command.PayloadHandle.IsValid() ? storage->GetPayload(command.PayloadHandle) : nullptr;
        const auto elements = command.ExtraElementsHandle.IsValid() ? storage->GetExtraElements(command.ExtraElementsHandle) : nullptr;
        const bool pipelineChanged = stateCache.Pipeline.Get() != command.Pipeline.Get();
        if (pipelineChanged) {
            encoder.BindGraphicsPipelineState(command.Pipeline.Get());
            stateCache.OnPipelineChanged(command.Pipeline.Get());
            for (const auto& binding : submitArgs.ParameterSets) {
                encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
            if (submitArgs.InstanceStream) {
                stateCache.UpdateVertexBuffer(*submitArgs.InstanceStream);
                encoder.BindVertexBuffers(std::span{&*submitArgs.InstanceStream, 1});
            }
        }
        if (stateCache.LastVertexStorage != storage || stateCache.LastVertexBuffersHandle != command.VertexBuffersHandle) {
            if (command.VertexBuffersHandle.IsValid()) {
                const auto& vertices = *storage->GetVertexBuffers(command.VertexBuffersHandle);
                const std::span<const render::VertexBufferBinding> bindings{vertices.Bindings.data(), vertices.Bindings.size()};
                size_t runStart = bindings.size();
                for (size_t index = 0; index < bindings.size(); ++index) {
                    const auto& binding = bindings[index];
                    const bool dirty = (!submitArgs.InstanceStream || submitArgs.InstanceStream->Binding != binding.Binding) && stateCache.UpdateVertexBuffer(binding);
                    if (runStart != bindings.size() && (!dirty || binding.Binding - bindings[index - 1].Binding != 1)) {
                        encoder.BindVertexBuffers(bindings.subspan(runStart, index - runStart));
                        runStart = bindings.size();
                    }
                    if (dirty && runStart == bindings.size()) runStart = index;
                }
                if (runStart != bindings.size()) encoder.BindVertexBuffers(bindings.subspan(runStart));
            }
            stateCache.LastVertexStorage = storage;
            stateCache.LastVertexBuffersHandle = command.VertexBuffersHandle;
        }
        if (!stateCache.StencilReference || *stateCache.StencilReference != command.StencilReference) {
            encoder.SetStencilReference(command.StencilReference);
            stateCache.StencilReference = command.StencilReference;
        }
        if (payload) {
            for (const auto& binding : payload->ParameterSets) {
                if (stateCache.UpdateParameterSet(binding)) encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
            if (payload->PushConstants) {
                const auto& push = *payload->PushConstants;
                if (!encoder.SetPushConstants(push.PushConstantId, push.Offset, {push.Data.data(), push.Data.size()})) return false;
            }
        }
        while (mask != 0) {
            const uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
            mask &= mask - 1;
            const auto ordinal = static_cast<uint32_t>(std::popcount(command.ValidElementMask & ((uint32_t{1} << bit) - 1)));
            const MeshDrawElement primary{command.IndexBuffer, command.IndexCount, command.FirstIndex, command.VertexOffset, bit};
            const auto& element = ordinal == 0 ? primary : elements->ExtraElements[ordinal - 1];
            uint32_t firstInstance = visible.FirstInstance;
            uint32_t instanceCount = visible.InstanceCount;
            if (elements && !elements->ElementInstances.empty()) {
                if (visible.UseElementFirstInstance) firstInstance = elements->ElementInstances[ordinal].FirstInstance;
                if (visible.UseElementInstanceCount) instanceCount = elements->ElementInstances[ordinal].InstanceCount;
            }
            if (instanceCount == 0) continue;
            if (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, element.IndexBuffer)) {
                encoder.BindIndexBuffer(element.IndexBuffer);
                stateCache.IndexBuffer = element.IndexBuffer;
            }
            encoder.DrawIndexed(element.IndexCount, instanceCount, element.FirstIndex, element.VertexOffset, firstInstance);
        }
    }
    return true;
}

}  // namespace radray
