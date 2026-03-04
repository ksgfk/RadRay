#pragma once

#include <radray/render/render_graph.h>

namespace radray::render {

class RGRegistry;

struct RGPassContext {
    CommandBuffer* Cmd{nullptr};
    RGRegistry* Registry{nullptr};
    QueueType QueueClass{QueueType::Direct};
    uint32_t SortedPassIndex{0};
    uint32_t PassIndex{0};
    std::string_view PassName{};

    Texture* GetTexture(RGResourceHandle handle) const noexcept;
    Buffer* GetBuffer(RGResourceHandle handle) const noexcept;
};

class RGRegistry {
public:
    explicit RGRegistry(shared_ptr<Device> device) noexcept;

    bool ImportPhysicalTexture(RGResourceHandle handle, Texture* texture) noexcept;
    bool ImportPhysicalBuffer(RGResourceHandle handle, Buffer* buffer) noexcept;

    Texture* GetTexture(RGResourceHandle handle) const noexcept;
    Buffer* GetBuffer(RGResourceHandle handle) const noexcept;
    RGAccessMode ResolveStateBefore(RGResourceHandle handle, RGAccessMode fallback) const noexcept;
    void CommitStateAfter(RGResourceHandle handle, RGAccessMode state) noexcept;

    bool EnsureResources(const RGGraphBuilder& graph, const CompiledGraph& compiled) noexcept;

private:
    struct TextureBinding {
        Texture* Ptr{nullptr};
        unique_ptr<Texture> Owned{};
    };
    struct BufferBinding {
        Buffer* Ptr{nullptr};
        unique_ptr<Buffer> Owned{};
    };

    bool EnsureTexture(
        uint32_t resourceIndex,
        const VirtualResource& resource,
        const RGGraphBuilder& graph,
        const CompiledGraph& compiled) noexcept;
    bool EnsureBuffer(
        uint32_t resourceIndex,
        const VirtualResource& resource,
        const RGGraphBuilder& graph,
        const CompiledGraph& compiled) noexcept;

private:
    shared_ptr<Device> _device{};
    unordered_map<uint32_t, TextureBinding> _textures{};
    unordered_map<uint32_t, BufferBinding> _buffers{};
    unordered_map<uint32_t, RGAccessMode> _resourceStates{};
};

struct RGRecordOptions {
    bool EmitBarriers{true};
    bool ValidateQueueClass{true};
    QueueType RecordQueueClass{QueueType::Direct};
};

class RGExecutor {
public:
    explicit RGExecutor(shared_ptr<Device> device) noexcept;
    virtual ~RGExecutor() noexcept = default;

    bool Record(
        CommandBuffer* cmd,
        const RGGraphBuilder& graph,
        const CompiledGraph& compiled,
        RGRegistry* registry,
        const RGRecordOptions& options = {}) noexcept;

    static unique_ptr<RGExecutor> Create(shared_ptr<Device> device) noexcept;

protected:
    virtual bool EmitPassBarriers(
        CommandBuffer* cmd,
        std::span<const RGBarrier> barriers,
        const RGGraphBuilder& graph,
        const RGRegistry& registry) noexcept = 0;

    shared_ptr<Device> _device{};
};

}  // namespace radray::render
