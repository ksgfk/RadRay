#include <radray/render/render_graph_executor.h>

#include <algorithm>

#include <radray/logger.h>

#if RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_helper.h>
#include <radray/render/backend/d3d12_impl.h>
#endif

#if RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_helper.h>
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

namespace {

constexpr bool RGIsTextureResourceType(RGResourceType type) noexcept {
    return type == RGResourceType::Texture;
}

constexpr bool RGIsBufferResourceType(RGResourceType type) noexcept {
    return type == RGResourceType::Buffer || type == RGResourceType::IndirectArgs;
}

TextureUses RGAccessModeToTextureUsage(RGAccessMode mode) noexcept {
    switch (mode) {
        case RGAccessMode::SampledRead:
        case RGAccessMode::StorageRead:
            return TextureUse::Resource;
        case RGAccessMode::StorageWrite:
            return TextureUse::UnorderedAccess;
        case RGAccessMode::ColorAttachmentWrite:
            return TextureUse::RenderTarget;
        case RGAccessMode::DepthStencilRead:
            return TextureUse::DepthStencilRead;
        case RGAccessMode::DepthStencilWrite:
            return TextureUse::DepthStencilWrite;
        case RGAccessMode::CopySource:
            return TextureUse::CopySource;
        case RGAccessMode::CopyDestination:
            return TextureUse::CopyDestination;
        default:
            return TextureUse::UNKNOWN;
    }
}

BufferUses RGAccessModeToBufferUsage(RGAccessMode mode) noexcept {
    switch (mode) {
        case RGAccessMode::SampledRead:
        case RGAccessMode::StorageRead:
            return BufferUse::Resource;
        case RGAccessMode::StorageWrite:
            return BufferUse::UnorderedAccess;
        case RGAccessMode::CopySource:
            return BufferUse::CopySource;
        case RGAccessMode::CopyDestination:
            return BufferUse::CopyDestination;
        case RGAccessMode::IndirectRead:
            return BufferUse::Indirect;
        default:
            return BufferUse::UNKNOWN;
    }
}

TextureStates RGAccessModeToTextureState(RGAccessMode mode) noexcept {
    switch (mode) {
        case RGAccessMode::Unknown:
            return TextureState::Undefined;
        case RGAccessMode::SampledRead:
        case RGAccessMode::StorageRead:
            return TextureState::ShaderRead;
        case RGAccessMode::StorageWrite:
            return TextureState::UnorderedAccess;
        case RGAccessMode::ColorAttachmentWrite:
            return TextureState::RenderTarget;
        case RGAccessMode::DepthStencilRead:
            return TextureState::DepthRead;
        case RGAccessMode::DepthStencilWrite:
            return TextureState::DepthWrite;
        case RGAccessMode::CopySource:
            return TextureState::CopySource;
        case RGAccessMode::CopyDestination:
            return TextureState::CopyDestination;
        case RGAccessMode::IndirectRead:
            return TextureState::Common;
    }
    return TextureState::Common;
}

BufferStates RGAccessModeToBufferState(RGAccessMode mode) noexcept {
    switch (mode) {
        case RGAccessMode::Unknown:
            return BufferState::Undefined;
        case RGAccessMode::SampledRead:
        case RGAccessMode::StorageRead:
            return BufferState::ShaderRead;
        case RGAccessMode::StorageWrite:
            return BufferState::UnorderedAccess;
        case RGAccessMode::CopySource:
            return BufferState::CopySource;
        case RGAccessMode::CopyDestination:
            return BufferState::CopyDestination;
        case RGAccessMode::IndirectRead:
            return BufferState::Indirect;
        default:
            return BufferState::Common;
    }
}

TextureUses RGCollectTextureUsage(uint32_t resourceIndex, const RGGraphBuilder& graph, const CompiledGraph& compiled) noexcept {
    TextureUses usage = TextureUse::UNKNOWN;
    const auto& passes = graph.GetPasses();
    for (uint32_t passIndex : compiled.SortedPasses) {
        if (passIndex >= passes.size()) {
            continue;
        }
        const auto& pass = passes[passIndex];
        for (const auto& edge : pass.Reads) {
            if (edge.Handle.Index == resourceIndex) {
                usage |= RGAccessModeToTextureUsage(edge.Mode);
            }
        }
        for (const auto& edge : pass.Writes) {
            if (edge.Handle.Index == resourceIndex) {
                usage |= RGAccessModeToTextureUsage(edge.Mode);
            }
        }
    }
    if (usage == TextureUse::UNKNOWN) {
        usage |= TextureUse::Resource;
    }
    return usage;
}

BufferUses RGCollectBufferUsage(uint32_t resourceIndex, const RGGraphBuilder& graph, const CompiledGraph& compiled) noexcept {
    BufferUses usage = BufferUse::UNKNOWN;
    const auto& passes = graph.GetPasses();
    for (uint32_t passIndex : compiled.SortedPasses) {
        if (passIndex >= passes.size()) {
            continue;
        }
        const auto& pass = passes[passIndex];
        for (const auto& edge : pass.Reads) {
            if (edge.Handle.Index == resourceIndex) {
                usage |= RGAccessModeToBufferUsage(edge.Mode);
            }
        }
        for (const auto& edge : pass.Writes) {
            if (edge.Handle.Index == resourceIndex) {
                usage |= RGAccessModeToBufferUsage(edge.Mode);
            }
        }
    }
    if (usage == BufferUse::UNKNOWN) {
        usage |= BufferUse::Common;
    }
    return usage;
}

bool RGEmitBarriersByCommonAPI(
    CommandBuffer* cmd,
    std::span<const RGBarrier> barriers,
    const RGRegistry& registry) noexcept {
    if (cmd == nullptr || barriers.empty()) {
        return true;
    }

    vector<ResourceBarrierDescriptor> rawBarriers{};
    rawBarriers.reserve(barriers.size());
    for (const auto& barrier : barriers) {
        if (Texture* texture = registry.GetTexture(barrier.Handle); texture != nullptr) {
            BarrierTextureDescriptor raw{};
            raw.Target = texture;
            raw.Before = RGAccessModeToTextureState(barrier.StateBefore);
            raw.After = RGAccessModeToTextureState(barrier.StateAfter);
            raw.IsSubresourceBarrier = std::holds_alternative<RGTextureRange>(barrier.Range);
            if (raw.IsSubresourceBarrier) {
                const auto& range = std::get<RGTextureRange>(barrier.Range);
                raw.Range = SubresourceRange{
                    .BaseArrayLayer = range.BaseArrayLayer,
                    .ArrayLayerCount = range.ArrayLayerCount,
                    .BaseMipLevel = range.BaseMipLevel,
                    .MipLevelCount = range.MipLevelCount};
            }
            rawBarriers.push_back(raw);
            continue;
        }

        if (Buffer* buffer = registry.GetBuffer(barrier.Handle); buffer != nullptr) {
            BarrierBufferDescriptor raw{};
            raw.Target = buffer;
            raw.Before = RGAccessModeToBufferState(barrier.StateBefore);
            raw.After = RGAccessModeToBufferState(barrier.StateAfter);
            rawBarriers.push_back(raw);
            continue;
        }
    }

    if (!rawBarriers.empty()) {
        cmd->ResourceBarrier(rawBarriers);
    }
    return true;
}

class RGExecutorFallback final : public RGExecutor {
public:
    using RGExecutor::RGExecutor;

protected:
    bool EmitPassBarriers(
        CommandBuffer* cmd,
        std::span<const RGBarrier> barriers,
        const RGGraphBuilder& graph,
        const RGRegistry& registry) noexcept override {
        RADRAY_UNUSED(graph);
        return RGEmitBarriersByCommonAPI(cmd, barriers, registry);
    }
};

#if RADRAY_ENABLE_D3D12
class RGExecutorD3D12 final : public RGExecutor {
public:
    using RGExecutor::RGExecutor;

protected:
    bool EmitPassBarriers(
        CommandBuffer* cmd,
        std::span<const RGBarrier> barriers,
        const RGGraphBuilder& graph,
        const RGRegistry& registry) noexcept override;
};
#endif

#if RADRAY_ENABLE_VULKAN
class RGExecutorVulkan final : public RGExecutor {
public:
    using RGExecutor::RGExecutor;

protected:
    bool EmitPassBarriers(
        CommandBuffer* cmd,
        std::span<const RGBarrier> barriers,
        const RGGraphBuilder& graph,
        const RGRegistry& registry) noexcept override;
};
#endif

#if RADRAY_ENABLE_D3D12
bool RGExecutorD3D12::EmitPassBarriers(
    CommandBuffer* cmd,
    std::span<const RGBarrier> barriers,
    const RGGraphBuilder& graph,
    const RGRegistry& registry) noexcept {
    RADRAY_UNUSED(graph);
    auto* cmdD3D12 = d3d12::CastD3D12Object(cmd);
    if (cmdD3D12 == nullptr || cmdD3D12->_cmdList.Get() == nullptr) {
        RADRAY_ERR_LOG("RenderGraphExecutor(D3D12): invalid command buffer");
        return false;
    }

    vector<D3D12_RESOURCE_BARRIER> rawBarriers{};
    rawBarriers.reserve(barriers.size());

    for (const auto& barrier : barriers) {
        if (Texture* texRaw = registry.GetTexture(barrier.Handle); texRaw != nullptr) {
            auto* texture = d3d12::CastD3D12Object(texRaw);
            if (texture == nullptr || texture->_tex.Get() == nullptr) {
                continue;
            }

            const auto beforeState = d3d12::MapType(RGAccessModeToTextureState(barrier.StateBefore));
            const auto afterState = d3d12::MapType(RGAccessModeToTextureState(barrier.StateAfter));
            if (beforeState == afterState) {
                // UAV barrier is only valid for UAV-write hazards.
                if (barrier.StateAfter == RGAccessMode::StorageWrite) {
                    auto& raw = rawBarriers.emplace_back();
                    raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    raw.UAV.pResource = texture->_tex.Get();
                }
                continue;
            }

            auto appendTransition = [&](UINT subresource) {
                auto& raw = rawBarriers.emplace_back();
                raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                raw.Transition.pResource = texture->_tex.Get();
                raw.Transition.Subresource = subresource;
                raw.Transition.StateBefore = beforeState;
                raw.Transition.StateAfter = afterState;
            };

            if (!std::holds_alternative<RGTextureRange>(barrier.Range)) {
                appendTransition(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
                continue;
            }

            const auto& range = std::get<RGTextureRange>(barrier.Range);
            const uint32_t textureMipLevels = std::max(1u, texture->_desc.MipLevels);
            const uint32_t textureArrayLayers = std::max(1u, texture->_desc.DepthOrArraySize);
            const uint32_t mipCount = range.MipLevelCount == RGTextureRange::All
                                          ? textureMipLevels - range.BaseMipLevel
                                          : range.MipLevelCount;
            const uint32_t layerCount = range.ArrayLayerCount == RGTextureRange::All
                                            ? textureArrayLayers - range.BaseArrayLayer
                                            : range.ArrayLayerCount;
            const bool isAllSubresource = range.BaseMipLevel == 0 && range.BaseArrayLayer == 0 && mipCount == textureMipLevels && layerCount == textureArrayLayers;
            if (isAllSubresource) {
                appendTransition(D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
                continue;
            }

            for (uint32_t mip = 0; mip < mipCount; ++mip) {
                for (uint32_t layer = 0; layer < layerCount; ++layer) {
                    const uint32_t subresource = D3D12CalcSubresource(
                        range.BaseMipLevel + mip,
                        range.BaseArrayLayer + layer,
                        0,
                        textureMipLevels,
                        textureArrayLayers);
                    appendTransition(subresource);
                }
            }
            continue;
        }

        if (Buffer* bufRaw = registry.GetBuffer(barrier.Handle); bufRaw != nullptr) {
            auto* buffer = d3d12::CastD3D12Object(bufRaw);
            if (buffer == nullptr || buffer->_buf.Get() == nullptr) {
                continue;
            }

            const auto beforeState = d3d12::MapType(RGAccessModeToBufferState(barrier.StateBefore));
            const auto afterState = d3d12::MapType(RGAccessModeToBufferState(barrier.StateAfter));
            if (beforeState == afterState) {
                // UAV barrier is only valid for UAV-write hazards.
                if (barrier.StateAfter == RGAccessMode::StorageWrite) {
                    auto& raw = rawBarriers.emplace_back();
                    raw.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                    raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                    raw.UAV.pResource = buffer->_buf.Get();
                }
                continue;
            }

            auto& raw = rawBarriers.emplace_back();
            raw.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            raw.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            raw.Transition.pResource = buffer->_buf.Get();
            raw.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            raw.Transition.StateBefore = beforeState;
            raw.Transition.StateAfter = afterState;
            continue;
        }
    }

    if (!rawBarriers.empty()) {
        cmdD3D12->_cmdList->ResourceBarrier(static_cast<UINT>(rawBarriers.size()), rawBarriers.data());
    }
    return true;
}
#endif

#if RADRAY_ENABLE_VULKAN
bool RGExecutorVulkan::EmitPassBarriers(
    CommandBuffer* cmd,
    std::span<const RGBarrier> barriers,
    const RGGraphBuilder& graph,
    const RGRegistry& registry) noexcept {
    RADRAY_UNUSED(graph);
    auto* cmdVk = vulkan::CastVkObject(cmd);
    if (cmdVk == nullptr || cmdVk->_cmdBuffer == VK_NULL_HANDLE) {
        RADRAY_ERR_LOG("RenderGraphExecutor(Vulkan): invalid command buffer");
        return false;
    }

    if (cmdVk->_device == nullptr || cmdVk->_device->_ftb.vkCmdPipelineBarrier2 == nullptr) {
        return RGEmitBarriersByCommonAPI(cmd, barriers, registry);
    }

    vector<VkMemoryBarrier2> memoryBarriers{};
    vector<VkBufferMemoryBarrier2> bufferBarriers{};
    vector<VkImageMemoryBarrier2> imageBarriers{};
    memoryBarriers.reserve(barriers.size());
    bufferBarriers.reserve(barriers.size());
    imageBarriers.reserve(barriers.size());

    auto fallbackSrcStage = [](VkPipelineStageFlags2 stage) {
        return stage == 0 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : stage;
    };
    auto fallbackDstStage = [](VkPipelineStageFlags2 stage) {
        return stage == 0 ? VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT : stage;
    };

    for (const auto& barrier : barriers) {
        if (Texture* texRaw = registry.GetTexture(barrier.Handle); texRaw != nullptr) {
            auto* texture = vulkan::CastVkObject(texRaw);
            if (texture == nullptr || texture->_image == VK_NULL_HANDLE) {
                continue;
            }

            const TextureStates beforeState = RGAccessModeToTextureState(barrier.StateBefore);
            const TextureStates afterState = RGAccessModeToTextureState(barrier.StateAfter);
            const auto srcStage = fallbackSrcStage(static_cast<VkPipelineStageFlags2>(
                vulkan::TextureStateToPipelineStageFlags(beforeState, true)));
            const auto dstStage = fallbackDstStage(static_cast<VkPipelineStageFlags2>(
                vulkan::TextureStateToPipelineStageFlags(afterState, false)));
            const auto srcAccess = static_cast<VkAccessFlags2>(
                vulkan::TextureStateToAccessFlags(beforeState));
            const auto dstAccess = static_cast<VkAccessFlags2>(
                vulkan::TextureStateToAccessFlags(afterState));

            if (barrier.StateBefore == barrier.StateAfter && RGIsWriteAccess(barrier.StateAfter)) {
                auto& mem = memoryBarriers.emplace_back();
                mem.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mem.pNext = nullptr;
                mem.srcStageMask = srcStage;
                mem.srcAccessMask = srcAccess;
                mem.dstStageMask = dstStage;
                mem.dstAccessMask = dstAccess;
                continue;
            }

            auto& img = imageBarriers.emplace_back();
            img.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            img.pNext = nullptr;
            img.srcStageMask = srcStage;
            img.srcAccessMask = srcAccess;
            img.dstStageMask = dstStage;
            img.dstAccessMask = dstAccess;
            img.oldLayout = vulkan::TextureStateToLayout(beforeState);
            img.newLayout = vulkan::TextureStateToLayout(afterState);
            img.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            img.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            img.image = texture->_image;
            img.subresourceRange.aspectMask = vulkan::ImageFormatToAspectFlags(texture->_rawFormat);

            if (std::holds_alternative<RGTextureRange>(barrier.Range)) {
                const auto& range = std::get<RGTextureRange>(barrier.Range);
                img.subresourceRange.baseMipLevel = range.BaseMipLevel;
                img.subresourceRange.levelCount = range.MipLevelCount == RGTextureRange::All
                                                      ? VK_REMAINING_MIP_LEVELS
                                                      : range.MipLevelCount;
                img.subresourceRange.baseArrayLayer = range.BaseArrayLayer;
                img.subresourceRange.layerCount = range.ArrayLayerCount == RGTextureRange::All
                                                      ? VK_REMAINING_ARRAY_LAYERS
                                                      : range.ArrayLayerCount;
            } else {
                img.subresourceRange.baseMipLevel = 0;
                img.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
                img.subresourceRange.baseArrayLayer = 0;
                img.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
            }
            continue;
        }

        if (Buffer* bufRaw = registry.GetBuffer(barrier.Handle); bufRaw != nullptr) {
            auto* buffer = vulkan::CastVkObject(bufRaw);
            if (buffer == nullptr || buffer->_buffer == VK_NULL_HANDLE) {
                continue;
            }

            const BufferStates beforeState = RGAccessModeToBufferState(barrier.StateBefore);
            const BufferStates afterState = RGAccessModeToBufferState(barrier.StateAfter);
            const auto srcStage = fallbackSrcStage(static_cast<VkPipelineStageFlags2>(
                vulkan::BufferStateToPipelineStageFlags(beforeState)));
            const auto dstStage = fallbackDstStage(static_cast<VkPipelineStageFlags2>(
                vulkan::BufferStateToPipelineStageFlags(afterState)));
            const auto srcAccess = static_cast<VkAccessFlags2>(
                vulkan::BufferStateToAccessFlags(beforeState));
            const auto dstAccess = static_cast<VkAccessFlags2>(
                vulkan::BufferStateToAccessFlags(afterState));

            if (barrier.StateBefore == barrier.StateAfter && RGIsWriteAccess(barrier.StateAfter)) {
                auto& mem = memoryBarriers.emplace_back();
                mem.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
                mem.pNext = nullptr;
                mem.srcStageMask = srcStage;
                mem.srcAccessMask = srcAccess;
                mem.dstStageMask = dstStage;
                mem.dstAccessMask = dstAccess;
                continue;
            }

            auto& buf = bufferBarriers.emplace_back();
            buf.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            buf.pNext = nullptr;
            buf.srcStageMask = srcStage;
            buf.srcAccessMask = srcAccess;
            buf.dstStageMask = dstStage;
            buf.dstAccessMask = dstAccess;
            buf.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            buf.buffer = buffer->_buffer;
            if (std::holds_alternative<RGBufferRange>(barrier.Range)) {
                const auto& range = std::get<RGBufferRange>(barrier.Range);
                buf.offset = range.Offset;
                buf.size = range.Size == RGBufferRange::All ? VK_WHOLE_SIZE : range.Size;
            } else {
                buf.offset = 0;
                buf.size = VK_WHOLE_SIZE;
            }
            continue;
        }
    }

    if (!memoryBarriers.empty() || !bufferBarriers.empty() || !imageBarriers.empty()) {
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.memoryBarrierCount = static_cast<uint32_t>(memoryBarriers.size());
        dep.pMemoryBarriers = memoryBarriers.empty() ? nullptr : memoryBarriers.data();
        dep.bufferMemoryBarrierCount = static_cast<uint32_t>(bufferBarriers.size());
        dep.pBufferMemoryBarriers = bufferBarriers.empty() ? nullptr : bufferBarriers.data();
        dep.imageMemoryBarrierCount = static_cast<uint32_t>(imageBarriers.size());
        dep.pImageMemoryBarriers = imageBarriers.empty() ? nullptr : imageBarriers.data();
        cmdVk->_device->_ftb.vkCmdPipelineBarrier2(cmdVk->_cmdBuffer, &dep);
    }
    return true;
}
#endif

}  // namespace

Texture* RGPassContext::GetTexture(RGResourceHandle handle) const noexcept {
    return Registry == nullptr ? nullptr : Registry->GetTexture(handle);
}

Buffer* RGPassContext::GetBuffer(RGResourceHandle handle) const noexcept {
    return Registry == nullptr ? nullptr : Registry->GetBuffer(handle);
}

RGRegistry::RGRegistry(shared_ptr<Device> device) noexcept
    : _device(std::move(device)) {}

bool RGRegistry::ImportPhysicalTexture(RGResourceHandle handle, Texture* texture) noexcept {
    if (!handle.IsValid() || texture == nullptr) {
        return false;
    }
    _textures.insert_or_assign(handle.Index, TextureBinding{
                                                 .Ptr = texture,
                                                 .Owned = {}});
    _resourceStates.insert_or_assign(handle.Index, RGAccessMode::Unknown);
    return true;
}

bool RGRegistry::ImportPhysicalBuffer(RGResourceHandle handle, Buffer* buffer) noexcept {
    if (!handle.IsValid() || buffer == nullptr) {
        return false;
    }
    _buffers.insert_or_assign(handle.Index, BufferBinding{
                                                .Ptr = buffer,
                                                .Owned = {}});
    _resourceStates.insert_or_assign(handle.Index, RGAccessMode::Unknown);
    return true;
}

Texture* RGRegistry::GetTexture(RGResourceHandle handle) const noexcept {
    const auto it = _textures.find(handle.Index);
    return it == _textures.end() ? nullptr : it->second.Ptr;
}

Buffer* RGRegistry::GetBuffer(RGResourceHandle handle) const noexcept {
    const auto it = _buffers.find(handle.Index);
    return it == _buffers.end() ? nullptr : it->second.Ptr;
}

RGAccessMode RGRegistry::ResolveStateBefore(RGResourceHandle handle, RGAccessMode fallback) const noexcept {
    if (!handle.IsValid()) {
        return fallback;
    }
    const auto it = _resourceStates.find(handle.Index);
    return it == _resourceStates.end() ? fallback : it->second;
}

void RGRegistry::CommitStateAfter(RGResourceHandle handle, RGAccessMode state) noexcept {
    if (!handle.IsValid()) {
        return;
    }
    _resourceStates.insert_or_assign(handle.Index, state);
}

bool RGRegistry::EnsureTexture(
    uint32_t resourceIndex,
    const VirtualResource& resource,
    const RGGraphBuilder& graph,
    const CompiledGraph& compiled) noexcept {
    if (_textures.contains(resourceIndex)) {
        if (!_resourceStates.contains(resourceIndex)) {
            _resourceStates.insert_or_assign(resourceIndex, RGAccessMode::Unknown);
        }
        return true;
    }
    if (resource.Flags.HasFlag(RGResourceFlag::External)) {
        RADRAY_ERR_LOG(
            "RenderGraphExecutor: external texture res[{}] '{}' not imported",
            resourceIndex,
            resource.Name);
        return false;
    }

    const auto* rgDesc = std::get_if<RGTextureDescriptor>(&resource.Descriptor);
    if (rgDesc == nullptr) {
        return false;
    }

    TextureDescriptor desc{};
    desc.Dim = rgDesc->Dim;
    desc.Width = rgDesc->Width;
    desc.Height = rgDesc->Height;
    desc.DepthOrArraySize = rgDesc->DepthOrArraySize;
    desc.MipLevels = rgDesc->MipLevels;
    desc.SampleCount = rgDesc->SampleCount;
    desc.Format = rgDesc->Format;
    desc.Memory = MemoryType::Device;
    desc.Usage = RGCollectTextureUsage(resourceIndex, graph, compiled);
    desc.Hints = ResourceHint::None;
    desc.Name = resource.Name;

    auto texture = _device->CreateTexture(desc);
    if (!texture.HasValue()) {
        return false;
    }
    auto owned = texture.Unwrap();
    Texture* raw = owned.get();
    _textures.insert_or_assign(resourceIndex, TextureBinding{
                                                  .Ptr = raw,
                                                  .Owned = std::move(owned)});
    _resourceStates.insert_or_assign(resourceIndex, RGAccessMode::Unknown);
    return true;
}

bool RGRegistry::EnsureBuffer(
    uint32_t resourceIndex,
    const VirtualResource& resource,
    const RGGraphBuilder& graph,
    const CompiledGraph& compiled) noexcept {
    if (_buffers.contains(resourceIndex)) {
        if (!_resourceStates.contains(resourceIndex)) {
            _resourceStates.insert_or_assign(resourceIndex, RGAccessMode::Unknown);
        }
        return true;
    }
    if (resource.Flags.HasFlag(RGResourceFlag::External)) {
        RADRAY_ERR_LOG(
            "RenderGraphExecutor: external buffer res[{}] '{}' not imported",
            resourceIndex,
            resource.Name);
        return false;
    }

    RGBufferDescriptor rgDesc{};
    if (const auto* buffer = std::get_if<RGBufferDescriptor>(&resource.Descriptor)) {
        rgDesc = *buffer;
    } else if (const auto* indirect = std::get_if<RGIndirectArgsDescriptor>(&resource.Descriptor)) {
        rgDesc = indirect->Buffer;
    } else {
        return false;
    }

    if (rgDesc.Size == 0 || rgDesc.Size == RGBufferRange::All) {
        return false;
    }

    BufferDescriptor desc{};
    desc.Size = rgDesc.Size;
    desc.Memory = MemoryType::Device;
    desc.Usage = RGCollectBufferUsage(resourceIndex, graph, compiled);
    if (std::holds_alternative<RGIndirectArgsDescriptor>(resource.Descriptor)) {
        desc.Usage |= BufferUse::Indirect;
    }
    desc.Hints = ResourceHint::None;
    desc.Name = resource.Name;

    auto buffer = _device->CreateBuffer(desc);
    if (!buffer.HasValue()) {
        return false;
    }
    auto owned = buffer.Unwrap();
    Buffer* raw = owned.get();
    _buffers.insert_or_assign(resourceIndex, BufferBinding{
                                                 .Ptr = raw,
                                                 .Owned = std::move(owned)});
    _resourceStates.insert_or_assign(resourceIndex, RGAccessMode::Unknown);
    return true;
}

bool RGRegistry::EnsureResources(const RGGraphBuilder& graph, const CompiledGraph& compiled) noexcept {
    if (_device == nullptr) {
        return false;
    }
    const auto& resources = graph.GetResources();
    for (uint32_t i = 0; i < static_cast<uint32_t>(resources.size()); ++i) {
        const auto& resource = resources[i];
        if (RGIsTextureResourceType(resource.GetType())) {
            if (!EnsureTexture(i, resource, graph, compiled)) {
                return false;
            }
            continue;
        }
        if (RGIsBufferResourceType(resource.GetType())) {
            if (!EnsureBuffer(i, resource, graph, compiled)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

RGExecutor::RGExecutor(shared_ptr<Device> device) noexcept
    : _device(std::move(device)) {}

bool RGExecutor::Record(
    CommandBuffer* cmd,
    const RGGraphBuilder& graph,
    const CompiledGraph& compiled,
    RGRegistry* registry,
    const RGRecordOptions& options) noexcept {
    if (_device == nullptr || cmd == nullptr || registry == nullptr || !compiled.Success) {
        return false;
    }
    if (!registry->EnsureResources(graph, compiled)) {
        return false;
    }

    const auto& passes = graph.GetPasses();
    for (uint32_t sortedIndex = 0; sortedIndex < static_cast<uint32_t>(compiled.SortedPasses.size()); ++sortedIndex) {
        const uint32_t passIndex = compiled.SortedPasses[sortedIndex];
        if (passIndex >= passes.size()) {
            return false;
        }
        const auto& pass = passes[passIndex];

        if (options.ValidateQueueClass && pass.QueueClass != options.RecordQueueClass) {
            RADRAY_ERR_LOG(
                "RenderGraphExecutor: pass '{}' queue class {} mismatches record queue {}",
                pass.Name,
                pass.QueueClass,
                options.RecordQueueClass);
            return false;
        }

        if (options.EmitBarriers && sortedIndex < compiled.PassBarriers.size()) {
            const auto& passBarriers = compiled.PassBarriers[sortedIndex];
            if (!passBarriers.empty()) {
                vector<RGBarrier> runtimeBarriers{};
                runtimeBarriers.reserve(passBarriers.size());
                for (const auto& barrier : passBarriers) {
                    RGBarrier runtimeBarrier = barrier;
                    runtimeBarrier.StateBefore = registry->ResolveStateBefore(barrier.Handle, barrier.StateBefore);
                    runtimeBarriers.push_back(runtimeBarrier);
                }
                if (!EmitPassBarriers(cmd, runtimeBarriers, graph, *registry)) {
                    return false;
                }
                for (const auto& barrier : runtimeBarriers) {
                    registry->CommitStateAfter(barrier.Handle, barrier.StateAfter);
                }
            }
        }

        if (pass.ExecuteFunc) {
            RGPassContext ctx{};
            ctx.Cmd = cmd;
            ctx.Registry = registry;
            ctx.QueueClass = pass.QueueClass;
            ctx.SortedPassIndex = sortedIndex;
            ctx.PassIndex = passIndex;
            ctx.PassName = pass.Name;
            pass.ExecuteFunc(ctx);
        }
    }

    return true;
}

unique_ptr<RGExecutor> RGExecutor::Create(shared_ptr<Device> device) noexcept {
    if (device == nullptr) {
        return nullptr;
    }

    switch (device->GetBackend()) {
#if RADRAY_ENABLE_VULKAN
        case RenderBackend::Vulkan:
            return make_unique<RGExecutorVulkan>(std::move(device));
#endif
#if RADRAY_ENABLE_D3D12
        case RenderBackend::D3D12:
            return make_unique<RGExecutorD3D12>(std::move(device));
#endif
        default:
            return make_unique<RGExecutorFallback>(std::move(device));
    }
}

}  // namespace radray::render
