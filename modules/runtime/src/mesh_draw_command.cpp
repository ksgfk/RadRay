#include <radray/runtime/mesh_draw_command.h>

#include <algorithm>
#include <limits>

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
    InlineVector<render::VertexBufferBinding, 8> VertexBuffers;
    InlineVector<MeshShaderParameterBinding, 4> CommandParameterSets;

    void OnPipelineChanged(render::GraphicsPipelineState* pipeline) noexcept {
        Pipeline = pipeline;
        VertexBuffers.clear();
        CommandParameterSets.clear();
    }

    void UpdateVertexBuffer(const render::VertexBufferBinding& binding, InlineVector<render::VertexBufferBinding, 8>& dirty) {
        const auto found = std::find_if(VertexBuffers.begin(), VertexBuffers.end(), [&](const render::VertexBufferBinding& cached) {
            return cached.Binding == binding.Binding;
        });
        if (found != VertexBuffers.end()) {
            if (SameVertexBufferView(found->View, binding.View)) return;
            *found = binding;
        } else {
            VertexBuffers.push_back(binding);
        }
        dirty.push_back(binding);
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

Nullable<const MeshDrawCommand*> ResolveIndexedMeshDrawCommand(SparseSetHandle handle, std::span<const MeshDrawCommand> commands) noexcept {
    if (handle.Generation != 0 || !handle.IsValid() || handle.Index >= commands.size()) return nullptr;
    return &commands[handle.Index];
}

}  // namespace

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
bool ValidateMeshDrawCommandImpl(const MeshDrawCommand& command, const VisibleMeshDrawCommand& visibleCommand,
                                 const MeshPassSubmitArgs& submitArgs) noexcept {
    const auto& indices = command.IndexBuffer;
    if (!command.Pipeline || !command.Pipeline->IsValid() || !indices.Target || !indices.Target->IsValid() || command.IndexCount == 0 ||
        (indices.Stride != 2 && indices.Stride != 4) || indices.Offset % indices.Stride != 0) return false;
    const auto desc = indices.Target->GetDesc();
    const uint64_t end = uint64_t{indices.Offset} + (uint64_t{command.FirstIndex} + command.IndexCount) * indices.Stride;
    if (!desc.Usage.HasFlag(render::BufferUse::Index) || end > desc.Size) return false;
    if (command.InstanceCount != 0 && uint64_t{command.FirstInstance} + command.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
    if (visibleCommand.InstanceCount != 0 && uint64_t{visibleCommand.FirstInstance} + visibleCommand.InstanceCount - 1 > std::numeric_limits<uint32_t>::max()) return false;
    bool hasDefaultInstanceStream = false;
    for (size_t index = 0; index < command.VertexBuffers.size(); ++index) {
        const auto& binding = command.VertexBuffers[index];
        const bool isInstanceStream = command.InstanceStreamBinding && *command.InstanceStreamBinding == binding.Binding;
        hasDefaultInstanceStream |= isInstanceStream;
        if (!(isInstanceStream && submitArgs.InstanceStream && submitArgs.InstanceStream->Binding == binding.Binding) && !ValidateVertexBuffer(binding.View)) return false;
        for (size_t previous = 0; previous < index; ++previous) {
            if (command.VertexBuffers[previous].Binding == binding.Binding) return false;
        }
    }
    if (command.InstanceStreamBinding && !hasDefaultInstanceStream &&
        (!submitArgs.InstanceStream || submitArgs.InstanceStream->Binding != *command.InstanceStreamBinding)) return false;
    if (submitArgs.InstanceStream && (!command.InstanceStreamBinding || submitArgs.InstanceStream->Binding != *command.InstanceStreamBinding)) return false;
    if (!ValidateParameterSets({command.ParameterSets.data(), command.ParameterSets.size()})) return false;
    for (const auto& binding : command.ParameterSets) {
        for (const auto& passBinding : submitArgs.ParameterSets) {
            if (binding.GroupIndex == passBinding.GroupIndex) return false;
        }
    }
    if (command.PushConstants) {
        const auto& push = *command.PushConstants;
        if (push.Data.empty() || push.Offset % 4 != 0 || push.Data.size() % 4 != 0 ||
            push.Data.size() > std::numeric_limits<uint32_t>::max() - push.Offset) return false;
    }
    return true;
}

}  // namespace

bool ValidateMeshDrawCommand(const MeshDrawCommand& command, const VisibleMeshDrawCommand& visibleCommand,
                             const MeshPassSubmitArgs& submitArgs) noexcept {
    return ValidateSubmitArgs(submitArgs) && ValidateMeshDrawCommandImpl(command, visibleCommand, submitArgs);
}

Nullable<const MeshDrawCommand*> ResolveVisibleMeshDrawCommand(
    const VisibleMeshDrawCommand& visibleCommand, Nullable<const CachedPassMeshDrawList*> cachedDrawList,
    std::span<const MeshDrawCommand> dynamicCommands) noexcept {
    if (visibleCommand.Source == MeshDrawCommandSource::Cached) {
        return cachedDrawList ? cachedDrawList->Commands.TryGet(visibleCommand.CommandHandle) : nullptr;
    }
    if (visibleCommand.Source == MeshDrawCommandSource::Dynamic) {
        return ResolveIndexedMeshDrawCommand(visibleCommand.CommandHandle, dynamicCommands);
    }
    return nullptr;
}

Nullable<const MeshDrawCommand*> ResolveVisibleMeshDrawCommand(
    const VisibleMeshDrawCommand& visibleCommand, std::span<const MeshDrawCommand> cachedCommands,
    std::span<const MeshDrawCommand> dynamicCommands) noexcept {
    if (visibleCommand.Source == MeshDrawCommandSource::Cached) {
        return ResolveIndexedMeshDrawCommand(visibleCommand.CommandHandle, cachedCommands);
    }
    if (visibleCommand.Source == MeshDrawCommandSource::Dynamic) {
        return ResolveIndexedMeshDrawCommand(visibleCommand.CommandHandle, dynamicCommands);
    }
    return nullptr;
}

namespace {

template <typename TCachedCommands>
bool SubmitMeshDrawCommandsImpl(render::GraphicsCommandEncoder& encoder,
                                std::span<const VisibleMeshDrawCommand> visibleCommands,
                                TCachedCommands cachedCommands,
                                std::span<const MeshDrawCommand> dynamicCommands,
                                const MeshPassSubmitArgs& submitArgs) noexcept {
    if (!encoder.IsValid()) return false;
#if RADRAY_IS_DEBUG
    if (!ValidateSubmitArgs(submitArgs)) return false;
    for (const auto& visible : visibleCommands) {
        const auto command = ResolveVisibleMeshDrawCommand(visible, cachedCommands, dynamicCommands);
        if (!command || !ValidateMeshDrawCommandImpl(*command, visible, submitArgs)) return false;
    }
#endif
    MeshDrawCommandStateCache stateCache;
    for (const auto& visible : visibleCommands) {
        if (visible.InstanceCount == 0) continue;
        const auto commandPtr = ResolveVisibleMeshDrawCommand(visible, cachedCommands, dynamicCommands);
        if (!commandPtr) return false;
        const auto& command = *commandPtr;
        if (stateCache.Pipeline.Get() != command.Pipeline.Get()) {
            encoder.BindGraphicsPipelineState(command.Pipeline.Get());
            stateCache.OnPipelineChanged(command.Pipeline.Get());
            for (const auto& binding : submitArgs.ParameterSets) {
                encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
        }
        InlineVector<render::VertexBufferBinding, 8> dirtyVertexBuffers;
        for (const auto& binding : command.VertexBuffers) {
            if (!submitArgs.InstanceStream || submitArgs.InstanceStream->Binding != binding.Binding) {
                stateCache.UpdateVertexBuffer(binding, dirtyVertexBuffers);
            }
        }
        if (submitArgs.InstanceStream) stateCache.UpdateVertexBuffer(*submitArgs.InstanceStream, dirtyVertexBuffers);
        std::sort(dirtyVertexBuffers.begin(), dirtyVertexBuffers.end(), [](const render::VertexBufferBinding& a, const render::VertexBufferBinding& b) {
            return a.Binding < b.Binding;
        });
        for (size_t first = 0; first < dirtyVertexBuffers.size();) {
            size_t end = first + 1;
            // RHI backends overwrite every slot between a call's lowest and highest binding.
            while (end < dirtyVertexBuffers.size() && dirtyVertexBuffers[end].Binding - dirtyVertexBuffers[end - 1].Binding == 1) ++end;
            encoder.BindVertexBuffers({dirtyVertexBuffers.data() + first, end - first});
            first = end;
        }
        if (!stateCache.IndexBuffer || !SameIndexBufferView(*stateCache.IndexBuffer, command.IndexBuffer)) {
            encoder.BindIndexBuffer(command.IndexBuffer);
            stateCache.IndexBuffer = command.IndexBuffer;
        }
        if (!stateCache.StencilReference || *stateCache.StencilReference != command.StencilReference) {
            encoder.SetStencilReference(command.StencilReference);
            stateCache.StencilReference = command.StencilReference;
        }
        for (const auto& binding : command.ParameterSets) {
            if (stateCache.UpdateParameterSet(binding)) {
                encoder.BindShaderParameterSet(binding.GroupIndex, binding.ParameterSet.Get(), {binding.DynamicOffsets.data(), binding.DynamicOffsets.size()});
            }
        }
        if (command.PushConstants) {
            const auto& push = *command.PushConstants;
            if (!encoder.SetPushConstants(push.PushConstantId, push.Offset, {push.Data.data(), push.Data.size()})) return false;
        }
        encoder.DrawIndexed(command.IndexCount, visible.InstanceCount, command.FirstIndex, command.VertexOffset, visible.FirstInstance);
    }
    return true;
}

}  // namespace

bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            Nullable<const CachedPassMeshDrawList*> cachedDrawList,
                            std::span<const MeshDrawCommand> dynamicCommands,
                            const MeshPassSubmitArgs& submitArgs) noexcept {
    return SubmitMeshDrawCommandsImpl(encoder, visibleCommands, cachedDrawList, dynamicCommands, submitArgs);
}

bool SubmitMeshDrawCommands(render::GraphicsCommandEncoder& encoder,
                            std::span<const VisibleMeshDrawCommand> visibleCommands,
                            std::span<const MeshDrawCommand> cachedCommands,
                            std::span<const MeshDrawCommand> dynamicCommands,
                            const MeshPassSubmitArgs& submitArgs) noexcept {
    return SubmitMeshDrawCommandsImpl(encoder, visibleCommands, cachedCommands, dynamicCommands, submitArgs);
}

}  // namespace radray
