#pragma once

#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include <radray/object_arena.h>
#include <radray/render/rhi.h>
#include <radray/runtime/frame_slot.h>

namespace radray {

namespace detail {
struct RDGData;
struct RDGExecution;
}  // namespace detail

struct RDGTextureHandle {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    friend constexpr bool operator==(RDGTextureHandle, RDGTextureHandle) = default;
};

struct RDGBufferHandle {
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    friend constexpr bool operator==(RDGBufferHandle, RDGBufferHandle) = default;
};

struct RDGTransientTextureAllocation {
    Nullable<render::Texture*> Texture{nullptr};
    render::TextureState LastState{render::TextureState::Undefined};
};

struct RDGTransientBufferAllocation {
    Nullable<render::Buffer*> Buffer{nullptr};
    render::BufferState LastState{render::BufferState::Undefined};
};

// See docs/architecture/runtime.md for graph, upload and slot ownership contracts.
class RDGTransientResourcePool final {
public:
    explicit RDGTransientResourcePool(render::Device& device) noexcept;
    ~RDGTransientResourcePool() noexcept;
    RDGTransientResourcePool(const RDGTransientResourcePool&) = delete;
    RDGTransientResourcePool& operator=(const RDGTransientResourcePool&) = delete;

    // Call only after this slot's previous work completed and command storage reset.
    void BeginFrame() noexcept;
    // All referencing command storage must have reset before releasing pooled owners.
    void Clear() noexcept;
    RDGTransientTextureAllocation AcquireTexture(const render::TextureDescriptor& desc);
    RDGTransientBufferAllocation AcquireBuffer(const render::BufferDescriptor& desc);
    void CommitTextureState(render::Texture* texture, render::TextureState finalState) noexcept;
    void CommitBufferState(render::Buffer* buffer, render::BufferState finalState) noexcept;
    Nullable<render::TextureView*> AcquireTextureView(const render::TextureViewDescriptor& desc);

private:
    struct Impl;
    unique_ptr<Impl> _impl;
};

class RDGRasterPassContext final {
public:
    render::GraphicsCommandEncoder& GetEncoder() noexcept;
    FrameSlot& GetSlot() noexcept;
    // Resolve only resources declared by this pass. Borrowings end with the command epoch.
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    render::Buffer& Resolve(RDGBufferHandle handle) const noexcept;
    Nullable<render::TextureView*> ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept;

private:
    friend class CompiledRDG;
    RDGRasterPassContext(detail::RDGExecution& execution, render::GraphicsCommandEncoder& encoder) noexcept;
    detail::RDGExecution& _execution;
    render::GraphicsCommandEncoder& _encoder;
};

class RDGComputePassContext final {
public:
    render::ComputeCommandEncoder& GetEncoder() noexcept;
    FrameSlot& GetSlot() noexcept;
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    render::Buffer& Resolve(RDGBufferHandle handle) const noexcept;
    Nullable<render::TextureView*> ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept;

private:
    friend class CompiledRDG;
    RDGComputePassContext(detail::RDGExecution& execution, render::ComputeCommandEncoder& encoder) noexcept;
    detail::RDGExecution& _execution;
    render::ComputeCommandEncoder& _encoder;
};

class RDGCopyPassContext final {
public:
    render::CommandBuffer& GetCommandBuffer() noexcept;
    FrameSlot& GetSlot() noexcept;
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    render::Buffer& Resolve(RDGBufferHandle handle) const noexcept;

private:
    friend class CompiledRDG;
    explicit RDGCopyPassContext(detail::RDGExecution& execution) noexcept;
    detail::RDGExecution& _execution;
};

class RDGRasterPassBuilder final {
public:
    RDGRasterPassBuilder& UseMeshPass(MeshPassId pass);
    RDGRasterPassBuilder& ReadBuffer(RDGBufferHandle buffer, render::BufferState state);
    RDGRasterPassBuilder& WriteBuffer(RDGBufferHandle buffer, render::BufferState state = render::BufferState::UnorderedAccess);
    RDGRasterPassBuilder& ReadWriteBuffer(RDGBufferHandle buffer, render::BufferState state = render::BufferState::UnorderedAccess);
    RDGRasterPassBuilder& ReadTexture(RDGTextureHandle texture, render::TextureState state = render::TextureState::ShaderRead);
    RDGRasterPassBuilder& WriteTexture(RDGTextureHandle texture, render::TextureState state = render::TextureState::UnorderedAccess);
    RDGRasterPassBuilder& ReadWriteTexture(RDGTextureHandle texture, render::TextureState state = render::TextureState::UnorderedAccess);
    RDGRasterPassBuilder& WriteColor(uint32_t slot, RDGTextureHandle texture, render::LoadAction load, render::StoreAction store,
                                     render::ColorClearValue clear = {}, Nullable<render::TextureView*> explicitView = nullptr);
    RDGRasterPassBuilder& SetDepthStencil(RDGTextureHandle texture, render::TextureState state,
                                          render::LoadAction depthLoad, render::StoreAction depthStore,
                                          render::LoadAction stencilLoad = render::LoadAction::DontCare,
                                          render::StoreAction stencilStore = render::StoreAction::Discard,
                                          render::DepthStencilClearValue clear = {}, Nullable<render::TextureView*> explicitView = nullptr);
    RDGRasterPassBuilder& NeverCull() noexcept;

private:
    friend class RDGBuilder;
    RDGRasterPassBuilder(detail::RDGData& data, uint32_t pass) noexcept;
    detail::RDGData& _data;
    uint32_t _pass;
};

class RDGComputePassBuilder final {
public:
    RDGComputePassBuilder& ReadBuffer(RDGBufferHandle buffer, render::BufferState state = render::BufferState::ShaderRead);
    RDGComputePassBuilder& WriteBuffer(RDGBufferHandle buffer, render::BufferState state = render::BufferState::UnorderedAccess);
    RDGComputePassBuilder& ReadWriteBuffer(RDGBufferHandle buffer, render::BufferState state = render::BufferState::UnorderedAccess);
    RDGComputePassBuilder& ReadTexture(RDGTextureHandle texture, render::TextureState state = render::TextureState::ShaderRead);
    RDGComputePassBuilder& WriteTexture(RDGTextureHandle texture, render::TextureState state = render::TextureState::UnorderedAccess);
    RDGComputePassBuilder& ReadWriteTexture(RDGTextureHandle texture, render::TextureState state = render::TextureState::UnorderedAccess);
    RDGComputePassBuilder& NeverCull() noexcept;

private:
    friend class RDGBuilder;
    RDGComputePassBuilder(detail::RDGData& data, uint32_t pass) noexcept;
    detail::RDGData& _data;
    uint32_t _pass;
};

class RDGCopyPassBuilder final {
public:
    RDGCopyPassBuilder& ReadTexture(RDGTextureHandle texture);
    RDGCopyPassBuilder& WriteTexture(RDGTextureHandle texture);
    RDGCopyPassBuilder& ReadBuffer(RDGBufferHandle buffer);
    RDGCopyPassBuilder& WriteBuffer(RDGBufferHandle buffer);
    RDGCopyPassBuilder& NeverCull() noexcept;

private:
    friend class RDGBuilder;
    RDGCopyPassBuilder(detail::RDGData& data, uint32_t pass) noexcept;
    detail::RDGData& _data;
    uint32_t _pass;
};

class CompiledRDG final {
public:
    explicit CompiledRDG(detail::RDGData& data) noexcept;
    std::span<const MeshPassId> GetAliveMeshPasses() const noexcept;
    // Execute once during Record, inside an open command buffer. Does not submit.
    // Failure is fatal to the frame: no retry, state rollback or partial submission.
    bool Execute(FrameSlot& slot, RDGTransientResourcePool& transientPool, render::CommandBuffer& commandBuffer) noexcept;

private:
    friend class RDGBuilder;
    friend class ObjectArena;
    detail::RDGData& _data;
    bool _executed{false};
};

// External synchronization remains the caller's responsibility; these flags record ownership barriers.
struct RDGExternalOwnership {
    bool Acquire{false};
    bool Release{false};
    friend bool operator==(const RDGExternalOwnership&, const RDGExternalOwnership&) = default;
};

// Builder and compiled graph borrow arena storage until slot reclamation. Setup
// callbacks run immediately; execute callbacks are owned by the arena and must
// capture only frame-lifetime data, never setup-local references.
class RDGBuilder final {
public:
    explicit RDGBuilder(ObjectArena& arena) noexcept;
    RDGBuilder(const RDGBuilder&) = delete;
    RDGBuilder& operator=(const RDGBuilder&) = delete;

    RDGTextureHandle ImportTexture(render::Texture* texture, render::TextureState initialState,
                                   std::optional<render::TextureState> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGBufferHandle ImportBuffer(render::Buffer* buffer, render::BufferState initialState,
                                 std::optional<render::BufferState> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGTextureHandle CreateTexture(const render::TextureDescriptor& desc);
    RDGBufferHandle CreateBuffer(const render::BufferDescriptor& desc);
    // Copies bytes now. Upload-memory ranges must be disjoint and precede GPU use.
    void UploadBuffer(RDGBufferHandle dst, uint64_t dstOffset, std::span<const byte> data);
    void MarkOutput(RDGTextureHandle texture);
    void MarkOutput(RDGBufferHandle buffer);

    template <typename SetupFn, typename ExecuteFn>
    void AddRasterPass(std::string_view name, SetupFn&& setup, ExecuteFn&& execute) {
        auto* callback = _arena.Emplace<std::decay_t<ExecuteFn>>(std::forward<ExecuteFn>(execute));
        const uint32_t pass = AddRasterPassImpl(name, callback, [](void* value, RDGRasterPassContext& ctx) {
            return (*static_cast<std::decay_t<ExecuteFn>*>(value))(ctx);
        });
        RDGRasterPassBuilder builder{_data, pass};
        std::forward<SetupFn>(setup)(builder);
    }

    template <typename SetupFn, typename ExecuteFn>
    void AddComputePass(std::string_view name, SetupFn&& setup, ExecuteFn&& execute) {
        auto* callback = _arena.Emplace<std::decay_t<ExecuteFn>>(std::forward<ExecuteFn>(execute));
        const uint32_t pass = AddComputePassImpl(name, callback, [](void* value, RDGComputePassContext& ctx) {
            return (*static_cast<std::decay_t<ExecuteFn>*>(value))(ctx);
        });
        RDGComputePassBuilder builder{_data, pass};
        std::forward<SetupFn>(setup)(builder);
    }

    template <typename SetupFn, typename ExecuteFn>
    void AddCopyPass(std::string_view name, SetupFn&& setup, ExecuteFn&& execute) {
        auto* callback = _arena.Emplace<std::decay_t<ExecuteFn>>(std::forward<ExecuteFn>(execute));
        const uint32_t pass = AddCopyPassImpl(name, callback, [](void* value, RDGCopyPassContext& ctx) {
            return (*static_cast<std::decay_t<ExecuteFn>*>(value))(ctx);
        });
        RDGCopyPassBuilder builder{_data, pass};
        std::forward<SetupFn>(setup)(builder);
    }

    // One-shot CPU validation and culling. Invalid declarations log and return null.
    Nullable<CompiledRDG*> Compile();

private:
    uint32_t AddRasterPassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGRasterPassContext&));
    uint32_t AddComputePassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGComputePassContext&));
    uint32_t AddCopyPassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGCopyPassContext&));
    ObjectArena& _arena;
    detail::RDGData& _data;
};

}  // namespace radray
