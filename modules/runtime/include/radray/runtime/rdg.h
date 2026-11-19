#pragma once

// 渲染图、上传与槽位所有权契约见 docs/architecture/runtime.md。

#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <variant>

#include <radray/nullable.h>
#include <radray/scope_guard.h>
#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/types.h>
#include <radray/viewport.h>
#include <radray/intrusive_ptr.h>
#include <radray/gpu/types.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/upload_arena.h>

namespace radray {

using RDGTextureViewDescriptor = GpuTextureViewKey;

class RDGAttachmentSlice {
public:
    uint32_t MipLevel{0};
    uint32_t BaseArrayLayer{0};
    uint32_t ArrayLayerCount{gpu::SubresourceRange::All};
    constexpr gpu::SubresourceRange ToSubresourceRange(gpu::TextureAspects aspects = {}) const noexcept {
        return {BaseArrayLayer, ArrayLayerCount, MipLevel, 1, aspects};
    }
    friend constexpr bool operator==(const RDGAttachmentSlice&, const RDGAttachmentSlice&) noexcept = default;
};

class RDGBlackboard final {
public:
    explicit RDGBlackboard(FrameSlot& slot);
    RDGBlackboard(const RDGBlackboard&) = delete;
    RDGBlackboard& operator=(const RDGBlackboard&) = delete;

    template <class T, class... Args>
    Nullable<T*> Emplace(Args&&... args) {
        const std::type_index key{typeid(T)};
        if (!_entries.try_emplace(key, nullptr).second) return nullptr;
        auto rollback = MakeScopeGuard([&]() noexcept { _entries.erase(key); });
        auto* value = _slot.Emplace<T>(std::forward<Args>(args)...);
        _entries.at(key) = value;
        rollback.Dismiss();
        return value;
    }

    template <class T>
    Nullable<T*> Get() const noexcept {
        const auto entry = _entries.find(typeid(T));
        return entry == _entries.end() ? nullptr : static_cast<T*>(entry->second);
    }
private:
    FrameSlot& _slot;
    std::pmr::unordered_map<std::type_index, void*> _entries;
};

struct RDGDeviceConstraints {
    uint64_t BufferCopyAlignment{1};
    uint64_t TexturePlacementAlignment{1};
    uint64_t TexturePitchAlignment{1};
    uint64_t CBufferSizeAlignment{1};
    uint64_t CBufferOffsetAlignment{1};
    uint64_t StorageOffsetAlignment{1};
    uint64_t TexelOffsetAlignment{1};
    uint64_t MaxBufferSize{0};
    uint64_t MaxCBufferRange{0};
    uint64_t MaxStorageRange{0};
};

struct RDGCompileOptions {
    bool FullValidation{true};
};

struct RDGDynamicBufferDescriptor {
    uint64_t MaxSize{0};
    gpu::BufferUses Usage{gpu::BufferUse::CBuffer};
    uint64_t Alignment{1};
};

class RDGUploadPrepareContext final {
public:
    RDGUploadPrepareContext(FrameSlot& slot, std::span<const MeshPassId> passes) noexcept;
    std::span<const MeshPassId> GetAliveMeshPasses() const noexcept;
    MeshPassFrameDrawList& GetPassDrawList(MeshPassId pass) noexcept;
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;

private:
    FrameSlot& _slot;
    std::span<const MeshPassId> _passes;
};

struct RDGUploadWriteContext {
    std::span<byte> Bytes;
    uint64_t Size;
    uint64_t RowPitch{0};
    uint64_t RowBytes{0};
    uint32_t Rows{1};
    uint32_t Depth{1};  // Rows is the number of block rows per tightly staged slice.
};

class RDGUploadSource final {
public:
    RDGUploadSource() noexcept = default;
    RDGUploadSource(RDGUploadSource&&) noexcept = default;
    RDGUploadSource& operator=(RDGUploadSource&&) noexcept = default;
    RDGUploadSource(const RDGUploadSource&) = delete;
    RDGUploadSource& operator=(const RDGUploadSource&) = delete;
    // 借用到 Prepare 消费结束；SnapshotBytes 是唯一立即复制 payload 的入口。
    static RDGUploadSource BorrowedBytes(std::span<const byte> bytes) noexcept;
    static RDGUploadSource OwnedBytes(vector<byte>&& bytes) noexcept;
    static RDGUploadSource SnapshotBytes(std::span<const byte> bytes);

    template <class T>
    static RDGUploadSource Generated(uint64_t maxSize, T context,
                                     bool (*write)(const T&, RDGUploadWriteContext&) noexcept,
                                     std::type_identity_t<Nullable<uint64_t (*)(T&, RDGUploadPrepareContext&) noexcept>> layout = nullptr) {
        class Generator {
        public:
            T Context;
            bool (*Write)(const T&, RDGUploadWriteContext&) noexcept;
            Nullable<uint64_t (*)(T&, RDGUploadPrepareContext&) noexcept> Layout;
        };
        constexpr bool kInlineGenerator = sizeof(Generator) <= 64 &&
                                          alignof(Generator) <= alignof(std::max_align_t) &&
                                          std::is_nothrow_move_constructible_v<Generator>;
        RDGUploadSource result;
        result._maxSize = maxSize;
        auto& source = result._storage.emplace<GeneratedSource>();
        if constexpr (kInlineGenerator) {
            ::new (static_cast<void*>(source.Storage)) Generator{std::move(context), write, layout};
            source.Layout = [](void* ptr, RDGUploadPrepareContext& ctx, uint64_t maximum) noexcept {
                auto& generator = *std::launder(reinterpret_cast<Generator*>(ptr));
                return generator.Layout ? generator.Layout.Get()(generator.Context, ctx) : maximum;
            };
            source.Write = [](const void* ptr, RDGUploadWriteContext& ctx) noexcept {
                const auto& generator = *std::launder(reinterpret_cast<const Generator*>(ptr));
                return generator.Write(generator.Context, ctx);
            };
            source.Move = [](void* dst, void* src) noexcept {
                auto* from = std::launder(reinterpret_cast<Generator*>(src));
                ::new (dst) Generator(std::move(*from));
                std::destroy_at(from);
            };
            source.Destroy = [](void* ptr) noexcept {
                std::destroy_at(std::launder(reinterpret_cast<Generator*>(ptr)));
            };
        } else {
            auto* heapPtr = new Generator{std::move(context), write, layout};
            ::new (static_cast<void*>(source.Storage)) Generator*(heapPtr);
            source.Layout = [](void* ptr, RDGUploadPrepareContext& ctx, uint64_t maximum) noexcept {
                auto* generator = *std::launder(reinterpret_cast<Generator**>(ptr));
                return generator->Layout ? generator->Layout.Get()(generator->Context, ctx) : maximum;
            };
            source.Write = [](const void* ptr, RDGUploadWriteContext& ctx) noexcept {
                const auto* generator = *std::launder(reinterpret_cast<Generator* const*>(ptr));
                return generator->Write(generator->Context, ctx);
            };
            source.Move = [](void* dst, void* src) noexcept {
                auto* generator = *std::launder(reinterpret_cast<Generator**>(src));
                ::new (dst) Generator*(generator);
            };
            source.Destroy = [](void* ptr) noexcept {
                auto* generator = *std::launder(reinterpret_cast<Generator**>(ptr));
                delete generator;
            };
        }
        return result;
    }
    uint64_t GetMaxSize() const noexcept;
    uint64_t PrepareLayout(RDGUploadPrepareContext& context) noexcept;
    bool Write(RDGUploadWriteContext& context, uint64_t sourceRowPitch = 0, uint32_t sourceRowsPerImage = 0) const noexcept;
    void ReleaseSource() noexcept;

private:
    class GeneratedSource final {
    public:
        GeneratedSource() noexcept = default;
        GeneratedSource(GeneratedSource&& other) noexcept;
        GeneratedSource& operator=(GeneratedSource&& other) noexcept;
        GeneratedSource(const GeneratedSource&) = delete;
        GeneratedSource& operator=(const GeneratedSource&) = delete;
        ~GeneratedSource() noexcept;
        void Reset() noexcept;

        alignas(std::max_align_t) byte Storage[64]{};
        Nullable<uint64_t (*)(void*, RDGUploadPrepareContext&, uint64_t) noexcept> Layout{nullptr};
        Nullable<bool (*)(const void*, RDGUploadWriteContext&) noexcept> Write{nullptr};
        void (*Move)(void*, void*) noexcept {nullptr};
        void (*Destroy)(void*) noexcept {nullptr};
    };
    std::variant<std::span<const byte>, vector<byte>, GeneratedSource> _storage;
    uint64_t _maxSize{0};
};

class RDGTextureUploadSource final {
public:
    static RDGTextureUploadSource BorrowedBytes(std::span<const byte> bytes, uint64_t sourceRowPitch = 0, uint32_t rowsPerImage = 0) noexcept;
    static RDGTextureUploadSource OwnedBytes(vector<byte>&& bytes, uint64_t sourceRowPitch = 0, uint32_t rowsPerImage = 0) noexcept;
    static RDGTextureUploadSource SnapshotBytes(std::span<const byte> bytes, uint64_t sourceRowPitch = 0, uint32_t rowsPerImage = 0);
    template <class T>
    static RDGTextureUploadSource Generated(uint64_t size, T data, bool (*write)(const T&, RDGUploadWriteContext&) noexcept) {
        return {RDGUploadSource::Generated(size, std::move(data), write), 0};
    }
    RDGUploadSource Source;
    uint64_t SourceRowPitch;
    uint32_t RowsPerImage{0};
};

namespace detail {
struct RDGData;
struct RDGExecution;
class ErasedPassCallback final {
public:
    ErasedPassCallback() noexcept = default;
    ErasedPassCallback(void* instance, void (*destroy)(void*) noexcept) noexcept;
    ErasedPassCallback(ErasedPassCallback&& other) noexcept;
    ErasedPassCallback& operator=(ErasedPassCallback&& other) noexcept;
    ErasedPassCallback(const ErasedPassCallback&) = delete;
    ErasedPassCallback& operator=(const ErasedPassCallback&) = delete;
    ~ErasedPassCallback() noexcept;
    void* Get() const noexcept;
    void Reset() noexcept;

private:
    void* _instance{nullptr};
    void (*_destroy)(void*) noexcept {nullptr};
};
RDGDeviceConstraints QueryRDGDeviceConstraints(const gpu::Device& device) noexcept;
}  // namespace detail

class RDGPassHandle {
public:
    uint32_t Index{std::numeric_limits<uint32_t>::max()};
    constexpr bool IsValid() const noexcept { return Index != std::numeric_limits<uint32_t>::max(); }
    friend constexpr bool operator==(RDGPassHandle, RDGPassHandle) = default;
};

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
    Nullable<gpu::Texture*> Texture{nullptr};
    gpu::TextureState LastState{gpu::TextureState::Undefined};
};

struct RDGTransientBufferAllocation {
    Nullable<gpu::Buffer*> Buffer{nullptr};
    gpu::BufferStates LastState{gpu::BufferState::Undefined};
};

class RDGTransientResourcePool final {
public:
    explicit RDGTransientResourcePool(gpu::Device& device) noexcept;
    ~RDGTransientResourcePool() noexcept;
    RDGTransientResourcePool(const RDGTransientResourcePool&) = delete;
    RDGTransientResourcePool& operator=(const RDGTransientResourcePool&) = delete;

    // 仅在本槽上一帧工作完成且命令存储已重置后调用。
    void BeginFrame() noexcept;
    // 释放池内资源所有者前，所有引用它们的命令存储必须先重置。
    void Clear() noexcept;
    RDGTransientTextureAllocation AcquireTexture(const gpu::TextureDescriptor& desc);
    RDGTransientBufferAllocation AcquireBuffer(const gpu::BufferDescriptor& desc);
    void CommitTextureState(gpu::Texture* texture, gpu::TextureState finalState) noexcept;
    void CommitBufferState(gpu::Buffer* buffer, gpu::BufferStates finalState) noexcept;
    Nullable<gpu::TextureView*> AcquireTextureView(const gpu::TextureViewDescriptor& desc);

private:
    class Impl;
    unique_ptr<Impl> _impl;
};

class RDGPrepareContext final {
public:
    explicit RDGPrepareContext(detail::RDGExecution& execution) noexcept;
    FrameSlot& GetSlot() noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;
    gpu::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    Nullable<gpu::TextureView*> ResolveView(RDGTextureHandle handle, gpu::TextureViewUsage usage) const noexcept;
    Nullable<gpu::TextureView*> ResolveView(RDGTextureHandle handle, const RDGTextureViewDescriptor& view) const noexcept;

private:
    detail::RDGExecution& _execution;
};

class RDGRasterPassContext final {
public:
    gpu::GraphicsCommandEncoder& GetEncoder() noexcept;
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;
    std::span<const VisibleMeshDrawCommand> GetVisibleCommands(MeshPassId pass) const noexcept;
    // 仅可解析本 Pass 已声明的资源；借用随命令 epoch 结束而失效。
    gpu::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;
    Nullable<gpu::TextureView*> ResolveView(RDGTextureHandle handle, gpu::TextureViewUsage usage) const noexcept;
    Nullable<gpu::TextureView*> ResolveView(RDGTextureHandle handle, const RDGTextureViewDescriptor& view) const noexcept;

private:
    friend class CompiledRDG;
    RDGRasterPassContext(detail::RDGExecution& execution, gpu::GraphicsCommandEncoder& encoder) noexcept;
    detail::RDGExecution& _execution;
    gpu::GraphicsCommandEncoder& _encoder;
};

class RDGComputePassContext final {
public:
    gpu::ComputeCommandEncoder& GetEncoder() noexcept;
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;
    std::span<const VisibleMeshDrawCommand> GetVisibleCommands(MeshPassId pass) const noexcept;
    gpu::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;
    Nullable<gpu::TextureView*> ResolveView(RDGTextureHandle handle, gpu::TextureViewUsage usage) const noexcept;
    Nullable<gpu::TextureView*> ResolveView(RDGTextureHandle handle, const RDGTextureViewDescriptor& view) const noexcept;

private:
    friend class CompiledRDG;
    RDGComputePassContext(detail::RDGExecution& execution, gpu::ComputeCommandEncoder& encoder) noexcept;
    detail::RDGExecution& _execution;
    gpu::ComputeCommandEncoder& _encoder;
};

class RDGCopyPassContext final {
public:
    gpu::CommandBuffer& GetCommandBuffer() noexcept;
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;
    std::span<const VisibleMeshDrawCommand> GetVisibleCommands(MeshPassId pass) const noexcept;
    gpu::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;

private:
    friend class CompiledRDG;
    explicit RDGCopyPassContext(detail::RDGExecution& execution) noexcept;
    detail::RDGExecution& _execution;
};

class RDGRasterPassBuilder final {
public:
    RDGPassHandle GetHandle() const noexcept;
    RDGRasterPassBuilder& SetRenderArea(Rect area);
    RDGRasterPassBuilder& UseMeshPass(MeshPassId pass);
    RDGRasterPassBuilder& ReadBuffer(RDGBufferHandle buffer, gpu::BufferStates state);
    RDGRasterPassBuilder& WriteBuffer(RDGBufferHandle buffer, gpu::BufferState state = gpu::BufferState::UnorderedAccess);
    RDGRasterPassBuilder& ReadWriteBuffer(RDGBufferHandle buffer, gpu::BufferState state = gpu::BufferState::UnorderedAccess);
    RDGRasterPassBuilder& ReadTexture(RDGTextureHandle texture, gpu::TextureState state = gpu::TextureState::ShaderRead);
    RDGRasterPassBuilder& WriteTexture(RDGTextureHandle texture, gpu::TextureState state = gpu::TextureState::UnorderedAccess);
    RDGRasterPassBuilder& ReadWriteTexture(RDGTextureHandle texture, gpu::TextureState state = gpu::TextureState::UnorderedAccess);
    RDGRasterPassBuilder& WriteColor(uint32_t slot, RDGTextureHandle texture, gpu::LoadAction load, gpu::StoreAction store,
                                     gpu::ColorClearValue clear = {}, Nullable<gpu::TextureView*> explicitView = nullptr);
    RDGRasterPassBuilder& WriteColor(uint32_t slot, RDGTextureHandle texture, gpu::LoadAction load, gpu::StoreAction store,
                                     gpu::ColorClearValue clear, gpu::SubresourceRange range);
    RDGRasterPassBuilder& WriteColor(uint32_t slot, RDGTextureHandle texture, gpu::LoadAction load, gpu::StoreAction store,
                                     gpu::ColorClearValue clear, RDGAttachmentSlice slice);
    RDGRasterPassBuilder& SetDepthStencil(RDGTextureHandle texture, gpu::TextureState state,
                                          gpu::LoadAction depthLoad, gpu::StoreAction depthStore,
                                          gpu::LoadAction stencilLoad = gpu::LoadAction::DontCare,
                                          gpu::StoreAction stencilStore = gpu::StoreAction::Discard,
                                          gpu::DepthStencilClearValue clear = {}, Nullable<gpu::TextureView*> explicitView = nullptr);
    RDGRasterPassBuilder& SetDepthStencil(RDGTextureHandle texture, gpu::TextureState state,
                                          gpu::LoadAction depthLoad, gpu::StoreAction depthStore,
                                          gpu::LoadAction stencilLoad, gpu::StoreAction stencilStore,
                                          gpu::DepthStencilClearValue clear, gpu::SubresourceRange range);
    RDGRasterPassBuilder& SetDepthStencil(RDGTextureHandle texture, gpu::TextureState state,
                                          gpu::LoadAction depthLoad, gpu::StoreAction depthStore,
                                          gpu::LoadAction stencilLoad, gpu::StoreAction stencilStore,
                                          gpu::DepthStencilClearValue clear, RDGAttachmentSlice slice);
    RDGRasterPassBuilder& NeverCull() noexcept;

private:
    friend class RDGBuilder;
    RDGRasterPassBuilder(detail::RDGData& data, uint32_t pass) noexcept;
    detail::RDGData& _data;
    uint32_t _pass;
};

class RDGComputePassBuilder final {
public:
    RDGPassHandle GetHandle() const noexcept;
    RDGComputePassBuilder& ReadBuffer(RDGBufferHandle buffer, gpu::BufferStates state = gpu::BufferState::ShaderRead);
    RDGComputePassBuilder& WriteBuffer(RDGBufferHandle buffer, gpu::BufferState state = gpu::BufferState::UnorderedAccess);
    RDGComputePassBuilder& ReadWriteBuffer(RDGBufferHandle buffer, gpu::BufferState state = gpu::BufferState::UnorderedAccess);
    RDGComputePassBuilder& ReadTexture(RDGTextureHandle texture, gpu::TextureState state = gpu::TextureState::ShaderRead);
    RDGComputePassBuilder& WriteTexture(RDGTextureHandle texture, gpu::TextureState state = gpu::TextureState::UnorderedAccess);
    RDGComputePassBuilder& ReadWriteTexture(RDGTextureHandle texture, gpu::TextureState state = gpu::TextureState::UnorderedAccess);
    RDGComputePassBuilder& NeverCull() noexcept;

private:
    friend class RDGBuilder;
    RDGComputePassBuilder(detail::RDGData& data, uint32_t pass) noexcept;
    detail::RDGData& _data;
    uint32_t _pass;
};

class RDGCopyPassBuilder final {
public:
    RDGPassHandle GetHandle() const noexcept;
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
    // Prepare、Record、CommitSubmittedStates 各一次；Record 要求命令已 Begin。
    // 失败使本帧不可恢复；不支持重试、状态回滚或部分提交。
    ~CompiledRDG() noexcept;
    bool Prepare(FrameSlot& slot, RDGTransientResourcePool& transientPool, UploadArena& uploadArena) noexcept;
    // 计时查询 0/1 保留给整帧，之后按实际 GPU 录制顺序每个 Pass 两项。
    vector<string> GetRecordedPassNames() const;
    bool Record(gpu::CommandBuffer& commandBuffer, Nullable<gpu::QueryPool*> passTimestamps = nullptr) noexcept;
    void CommitSubmittedStates() noexcept;

private:
    friend class RDGBuilder;
    class ExecutionDeleter {
    public:
        void operator()(detail::RDGExecution* execution) const noexcept;
    };
    detail::RDGData& _data;
    unique_ptr<detail::RDGExecution, ExecutionDeleter> _execution;
    bool _prepareAttempted{false}, _prepared{false}, _recordAttempted{false}, _recorded{false}, _committed{false};
};

// 外部同步由调用方负责；这些标志记录所有权屏障。
class RDGExternalOwnership {
public:
    bool Acquire{false};
    bool Release{false};
    friend bool operator==(const RDGExternalOwnership&, const RDGExternalOwnership&) = default;
};

// 构图器和编译后的图借用槽内存储，直到槽位回收。
// PassData 随图清理；Prepare 可修改，Record 只读；借用输入保持到消费完成。
class RDGBuilder final {
public:
    // 要求当前槽已 BeginFrame；图声明、编译和 Prepare 使用同一槽。
    explicit RDGBuilder(FrameSlot& slot, RDGCompileOptions options = {}) noexcept;
    RDGBuilder(FrameSlot& slot, RDGDeviceConstraints constraints, RDGCompileOptions options = {}) noexcept;
    RDGBuilder(const RDGBuilder&) = delete;
    RDGBuilder& operator=(const RDGBuilder&) = delete;

    RDGTextureHandle ImportTexture(gpu::Texture* texture, gpu::TextureState initialState,
                                   std::optional<gpu::TextureState> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGBufferHandle ImportBuffer(gpu::Buffer* buffer, gpu::BufferStates initialState,
                                 std::optional<gpu::BufferStates> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGTextureHandle CreateTexture(const gpu::TextureDescriptor& desc);
    RDGBlackboard& GetBlackboard() noexcept;
    std::optional<gpu::TextureDescriptor> GetTextureDescriptor(RDGTextureHandle texture) const noexcept;
    RDGPassHandle AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value);
    RDGPassHandle AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value, gpu::SubresourceRange range, std::optional<Rect> renderArea = std::nullopt);
    RDGPassHandle AddClearColor(RDGTextureHandle texture, gpu::ColorClearValue value, RDGAttachmentSlice slice, std::optional<Rect> renderArea = std::nullopt);
    RDGPassHandle AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value, bool clearDepth = true, bool clearStencil = true);
    RDGPassHandle AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value, gpu::SubresourceRange range,
                                       bool clearDepth = true, bool clearStencil = true, std::optional<Rect> renderArea = std::nullopt);
    RDGPassHandle AddClearDepthStencil(RDGTextureHandle texture, gpu::DepthStencilClearValue value, RDGAttachmentSlice slice,
                                       bool clearDepth = true, bool clearStencil = true, std::optional<Rect> renderArea = std::nullopt);
    RDGBufferHandle CreateBuffer(const gpu::BufferDescriptor& desc);
    RDGBufferHandle CreateBuffer(const RDGDynamicBufferDescriptor& desc, RDGUploadSource source);
    RDGBufferHandle ImportPersistentBorrowed(GpuBuffer& buffer);
    RDGTextureHandle ImportPersistentBorrowed(GpuTexture& texture);
    RDGBufferHandle ImportPersistentOwned(const IntrusivePtr<GpuBuffer>& buffer);
    RDGTextureHandle ImportPersistentOwned(const IntrusivePtr<GpuTexture>& texture);
    RDGPassHandle UploadBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGUploadSource source);
    RDGPassHandle UploadTexture(RDGTextureHandle dst, const gpu::TextureRegion2D& region, RDGTextureUploadSource source);
    void AddOrderDependency(RDGPassHandle before, RDGPassHandle after);
    void AddOrderBarrier(RDGPassHandle barrier);
    void MarkOutput(RDGTextureHandle texture);
    void MarkOutput(RDGBufferHandle buffer);

    template <class T>
    RDGRasterPassBuilder AddRasterPass(std::string_view name, T data,
                                       bool (*record)(const T&, RDGRasterPassContext&) noexcept,
                                       std::type_identity_t<Nullable<bool (*)(T&, RDGPrepareContext&) noexcept>> prepare = nullptr) {
        class Callback {
        public:
            T Data;
            bool (*Record)(const T&, RDGRasterPassContext&) noexcept;
            Nullable<bool (*)(T&, RDGPrepareContext&) noexcept> Prepare;
        };
        std::pmr::polymorphic_allocator<> allocator{&_slot.GetMemoryResource()};
        void* storage = allocator.allocate_bytes(sizeof(Callback), alignof(Callback));
        auto* instance = ::new (storage) Callback{std::move(data), record, prepare};
        constexpr void (*destroy)(void*) noexcept = std::is_trivially_destructible_v<Callback>
                                                        ? nullptr
                                                        : +[](void* ptr) noexcept { std::destroy_at(static_cast<Callback*>(ptr)); };
        detail::ErasedPassCallback callback{instance, destroy};
        const auto index = AddRasterPassImpl(name, std::move(callback), [](const void* value, RDGRasterPassContext& context) noexcept {
                const auto& cb = *static_cast<const Callback*>(value);
                return cb.Record(cb.Data, context); }, [](void* value, RDGPrepareContext& context) noexcept {
                auto& cb = *static_cast<Callback*>(value);
                return !cb.Prepare || cb.Prepare.Get()(cb.Data, context); });
        return {_data, index};
    }

    template <class T>
    RDGComputePassBuilder AddComputePass(std::string_view name, T data,
                                         bool (*record)(const T&, RDGComputePassContext&) noexcept,
                                         std::type_identity_t<Nullable<bool (*)(T&, RDGPrepareContext&) noexcept>> prepare = nullptr) {
        class Callback {
        public:
            T Data;
            bool (*Record)(const T&, RDGComputePassContext&) noexcept;
            Nullable<bool (*)(T&, RDGPrepareContext&) noexcept> Prepare;
        };
        std::pmr::polymorphic_allocator<> allocator{&_slot.GetMemoryResource()};
        void* storage = allocator.allocate_bytes(sizeof(Callback), alignof(Callback));
        auto* instance = ::new (storage) Callback{std::move(data), record, prepare};
        constexpr void (*destroy)(void*) noexcept = std::is_trivially_destructible_v<Callback>
                                                        ? nullptr
                                                        : +[](void* ptr) noexcept { std::destroy_at(static_cast<Callback*>(ptr)); };
        detail::ErasedPassCallback callback{instance, destroy};
        const auto index = AddComputePassImpl(name, std::move(callback), [](const void* value, RDGComputePassContext& context) noexcept {
                const auto& cb = *static_cast<const Callback*>(value);
                return cb.Record(cb.Data, context); }, [](void* value, RDGPrepareContext& context) noexcept {
                auto& cb = *static_cast<Callback*>(value);
                return !cb.Prepare || cb.Prepare.Get()(cb.Data, context); });
        return {_data, index};
    }

    template <class T>
    RDGCopyPassBuilder AddCopyPass(std::string_view name, T data,
                                   bool (*record)(const T&, RDGCopyPassContext&) noexcept,
                                   std::type_identity_t<Nullable<bool (*)(T&, RDGPrepareContext&) noexcept>> prepare = nullptr) {
        class Callback {
        public:
            T Data;
            bool (*Record)(const T&, RDGCopyPassContext&) noexcept;
            Nullable<bool (*)(T&, RDGPrepareContext&) noexcept> Prepare;
        };
        std::pmr::polymorphic_allocator<> allocator{&_slot.GetMemoryResource()};
        void* storage = allocator.allocate_bytes(sizeof(Callback), alignof(Callback));
        auto* instance = ::new (storage) Callback{std::move(data), record, prepare};
        constexpr void (*destroy)(void*) noexcept = std::is_trivially_destructible_v<Callback>
                                                        ? nullptr
                                                        : +[](void* ptr) noexcept { std::destroy_at(static_cast<Callback*>(ptr)); };
        detail::ErasedPassCallback callback{instance, destroy};
        const auto index = AddCopyPassImpl(name, std::move(callback), [](const void* value, RDGCopyPassContext& context) noexcept {
                const auto& cb = *static_cast<const Callback*>(value);
                return cb.Record(cb.Data, context); }, [](void* value, RDGPrepareContext& context) noexcept {
                auto& cb = *static_cast<Callback*>(value);
                return !cb.Prepare || cb.Prepare.Get()(cb.Data, context); });
        return {_data, index};
    }

    // 执行一次 CPU 校验与裁剪；声明无效时记录日志并返回空值。
    Nullable<CompiledRDG*> Compile();

private:
    uint32_t AddRasterPassImpl(std::string_view name, detail::ErasedPassCallback callback, bool (*execute)(const void*, RDGRasterPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept);
    uint32_t AddComputePassImpl(std::string_view name, detail::ErasedPassCallback callback, bool (*execute)(const void*, RDGComputePassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept);
    uint32_t AddCopyPassImpl(std::string_view name, detail::ErasedPassCallback callback, bool (*execute)(const void*, RDGCopyPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept);
    FrameSlot& _slot;
    detail::RDGData& _data;
    RDGBlackboard& _blackboard;
};

}  // namespace radray
