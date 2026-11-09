#pragma once

// 渲染图、上传与槽位所有权契约见 docs/architecture/runtime.md。

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
}  // 命名空间 detail

class RDGTextureHandle {
public:
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    friend constexpr bool operator==(RDGTextureHandle, RDGTextureHandle) = default;
};

class RDGBufferHandle {
public:
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
    render::BufferStates LastState{render::BufferState::Undefined};
};

class RDGTransientResourcePool final {
public:
    explicit RDGTransientResourcePool(render::Device& device) noexcept;
    ~RDGTransientResourcePool() noexcept;
    RDGTransientResourcePool(const RDGTransientResourcePool&) = delete;
    RDGTransientResourcePool& operator=(const RDGTransientResourcePool&) = delete;

    // 仅在本槽上一帧工作完成且命令存储已重置后调用。
    void BeginFrame() noexcept;
    // 释放池内资源所有者前，所有引用它们的命令存储必须先重置。
    void Clear() noexcept;
    RDGTransientTextureAllocation AcquireTexture(const render::TextureDescriptor& desc);
    RDGTransientBufferAllocation AcquireBuffer(const render::BufferDescriptor& desc);
    void CommitTextureState(render::Texture* texture, render::TextureState finalState) noexcept;
    void CommitBufferState(render::Buffer* buffer, render::BufferStates finalState) noexcept;
    Nullable<render::TextureView*> AcquireTextureView(const render::TextureViewDescriptor& desc);

private:
    class Impl;
    unique_ptr<Impl> _impl;
};

class RDGRasterPassContext final {
public:
    render::GraphicsCommandEncoder& GetEncoder() noexcept;
    FrameSlot& GetSlot() noexcept;
    // 仅可解析本 Pass 已声明的资源；借用随命令 epoch 结束而失效。
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
    RDGRasterPassBuilder& ReadBuffer(RDGBufferHandle buffer, render::BufferStates state);
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
    RDGComputePassBuilder& ReadBuffer(RDGBufferHandle buffer, render::BufferStates state = render::BufferState::ShaderRead);
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
    // 在 Record 期间、已开启录制的命令缓冲内执行一次；不负责提交。
    // 失败使本帧不可恢复；不支持重试、状态回滚或部分提交。
    bool Execute(FrameSlot& slot, RDGTransientResourcePool& transientPool, render::CommandBuffer& commandBuffer) noexcept;

private:
    friend class RDGBuilder;
    friend class ObjectArena;
    detail::RDGData& _data;
    bool _executed{false};
};

// 外部同步由调用方负责；这些标志记录所有权屏障。
class RDGExternalOwnership {
public:
    bool Acquire{false};
    bool Release{false};
    friend bool operator==(const RDGExternalOwnership&, const RDGExternalOwnership&) = default;
};

// 构图器和编译后的图借用 arena 存储，直到槽位回收。
// 设置回调立即执行；执行回调由 arena 持有，仅可捕获帧内有效的数据，
// 不得捕获设置阶段的局部引用。
class RDGBuilder final {
public:
    explicit RDGBuilder(ObjectArena& arena) noexcept;
    RDGBuilder(const RDGBuilder&) = delete;
    RDGBuilder& operator=(const RDGBuilder&) = delete;

    RDGTextureHandle ImportTexture(render::Texture* texture, render::TextureState initialState,
                                   std::optional<render::TextureState> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGBufferHandle ImportBuffer(render::Buffer* buffer, render::BufferStates initialState,
                                 std::optional<render::BufferStates> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGTextureHandle CreateTexture(const render::TextureDescriptor& desc);
    RDGBufferHandle CreateBuffer(const render::BufferDescriptor& desc);
    // 立即复制字节；上传内存的写入区间不得重叠，且写入必须先于 GPU 使用。
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

    // 执行一次 CPU 校验与裁剪；声明无效时记录日志并返回空值。
    Nullable<CompiledRDG*> Compile();

private:
    uint32_t AddRasterPassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGRasterPassContext&));
    uint32_t AddComputePassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGComputePassContext&));
    uint32_t AddCopyPassImpl(std::string_view name, void* callback, bool (*execute)(void*, RDGCopyPassContext&));
    ObjectArena& _arena;
    detail::RDGData& _data;
};

}  // namespace radray
