#pragma once

// 渲染图、上传与槽位所有权契约见 docs/architecture/runtime.md。

#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <radray/intrusive_ptr.h>
#include <radray/render/rhi.h>
#include <radray/runtime/frame_slot.h>
#include <radray/runtime/upload_arena.h>

namespace radray {

class GpuBuffer;
class GpuTexture;

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

struct RDGBufferSlice {
    Nullable<render::Buffer*> Buffer{nullptr};
    uint64_t Offset{0};
    uint64_t Size{0};
};

struct RDGDynamicBufferDescriptor {
    uint64_t MaxSize{0};
    render::BufferUses Usage{render::BufferUse::CBuffer};
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
        RDGUploadSource result;
        result._maxSize = maxSize;
        auto owner = unique_ptr<void, void (*)(void*) noexcept>{
            new Generator{std::move(context), write, layout},
            [](void* ptr) noexcept { delete static_cast<Generator*>(ptr); }};
        auto& source = result._storage.emplace<GeneratedSource>(GeneratedSource{std::move(owner)});
        source.Layout = [](void* ptr, RDGUploadPrepareContext& ctx, uint64_t maximum) noexcept {
            auto& generator = *static_cast<Generator*>(ptr);
            return generator.Layout ? generator.Layout.Get()(generator.Context, ctx) : maximum;
        };
        source.Write = [](const void* ptr, RDGUploadWriteContext& ctx) noexcept {
            const auto& generator = *static_cast<const Generator*>(ptr);
            return generator.Write(generator.Context, ctx);
        };
        return result;
    }
    uint64_t GetMaxSize() const noexcept;
    uint64_t PrepareLayout(RDGUploadPrepareContext& context) noexcept;
    bool Write(RDGUploadWriteContext& context, uint64_t sourceRowPitch = 0) const noexcept;
    void ReleaseSource() noexcept;

private:
    struct GeneratedSource {
        unique_ptr<void, void (*)(void*) noexcept> Context;
        Nullable<uint64_t (*)(void*, RDGUploadPrepareContext&, uint64_t) noexcept> Layout{nullptr};
        Nullable<bool (*)(const void*, RDGUploadWriteContext&) noexcept> Write{nullptr};
    };
    std::variant<std::span<const byte>, vector<byte>, GeneratedSource> _storage;
    uint64_t _maxSize{0};
};

class RDGTextureUploadSource final {
public:
    static RDGTextureUploadSource BorrowedBytes(std::span<const byte> bytes, uint64_t sourceRowPitch = 0) noexcept;
    static RDGTextureUploadSource OwnedBytes(vector<byte>&& bytes, uint64_t sourceRowPitch = 0) noexcept;
    static RDGTextureUploadSource SnapshotBytes(std::span<const byte> bytes, uint64_t sourceRowPitch = 0);
    template <class T>
    static RDGTextureUploadSource Generated(uint64_t size, T data, bool (*write)(const T&, RDGUploadWriteContext&) noexcept) {
        return {RDGUploadSource::Generated(size, std::move(data), write), 0};
    }
    RDGUploadSource Source;
    uint64_t SourceRowPitch;
};

namespace detail {
struct RDGData;
struct RDGExecution;
}  // namespace detail

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

class RDGPrepareContext final {
public:
    explicit RDGPrepareContext(detail::RDGExecution& execution) noexcept;
    FrameSlot& GetSlot() noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    Nullable<render::TextureView*> ResolveView(RDGTextureHandle handle, render::TextureViewUsage usage) const noexcept;

private:
    detail::RDGExecution& _execution;
};

class RDGRasterPassContext final {
public:
    render::GraphicsCommandEncoder& GetEncoder() noexcept;
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;
    std::span<const VisibleMeshDrawCommand> GetVisibleCommands(MeshPassId pass) const noexcept;
    // 仅可解析本 Pass 已声明的资源；借用随命令 epoch 结束而失效。
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;
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
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;
    std::span<const VisibleMeshDrawCommand> GetVisibleCommands(MeshPassId pass) const noexcept;
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;
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
    uint64_t GetFrameId() const noexcept;
    uint32_t GetSlotIndex() const noexcept;
    std::span<const VisibleMeshDrawCommand> GetVisibleCommands(MeshPassId pass) const noexcept;
    render::Texture& Resolve(RDGTextureHandle handle) const noexcept;
    RDGBufferSlice ResolveBuffer(RDGBufferHandle handle) const noexcept;

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
    // Prepare、Record、CommitSubmittedStates 各一次；Record 要求命令已 Begin。
    // 失败使本帧不可恢复；不支持重试、状态回滚或部分提交。
    ~CompiledRDG() noexcept;
    bool Prepare(FrameSlot& slot, RDGTransientResourcePool& transientPool, UploadArena& uploadArena) noexcept;
    bool Record(render::CommandBuffer& commandBuffer) noexcept;
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
    explicit RDGBuilder(FrameSlot& slot, RDGDeviceConstraints constraints = {}, RDGCompileOptions options = {}) noexcept;
    RDGBuilder(const RDGBuilder&) = delete;
    RDGBuilder& operator=(const RDGBuilder&) = delete;

    RDGTextureHandle ImportTexture(render::Texture* texture, render::TextureState initialState,
                                   std::optional<render::TextureState> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGBufferHandle ImportBuffer(render::Buffer* buffer, render::BufferStates initialState,
                                 std::optional<render::BufferStates> finalState = std::nullopt, RDGExternalOwnership ownership = {});
    RDGTextureHandle CreateTexture(const render::TextureDescriptor& desc);
    RDGBufferHandle CreateBuffer(const render::BufferDescriptor& desc);
    RDGBufferHandle CreateBuffer(const RDGDynamicBufferDescriptor& desc, RDGUploadSource source);
    RDGBufferHandle ImportPersistentBorrowed(GpuBuffer& buffer);
    RDGTextureHandle ImportPersistentBorrowed(GpuTexture& texture);
    RDGBufferHandle ImportPersistentOwned(const IntrusivePtr<GpuBuffer>& buffer);
    RDGTextureHandle ImportPersistentOwned(const IntrusivePtr<GpuTexture>& texture);
    void UploadBuffer(RDGBufferHandle dst, uint64_t dstOffset, RDGUploadSource source);
    void UploadTexture(RDGTextureHandle dst, const render::TextureRegion2D& region, RDGTextureUploadSource source);
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
        auto callback = make_shared<Callback>(Callback{std::move(data), record, prepare});
        const auto index = AddRasterPassImpl(name, std::move(callback), [](const void* value, RDGRasterPassContext& context) noexcept {
                const auto& callback = *static_cast<const Callback*>(value);
                return callback.Record(callback.Data, context); }, [](void* value, RDGPrepareContext& context) noexcept {
                auto& callback = *static_cast<Callback*>(value);
                return !callback.Prepare || callback.Prepare.Get()(callback.Data, context); });
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
        auto callback = make_shared<Callback>(Callback{std::move(data), record, prepare});
        const auto index = AddComputePassImpl(name, std::move(callback), [](const void* value, RDGComputePassContext& context) noexcept {
                const auto& callback = *static_cast<const Callback*>(value);
                return callback.Record(callback.Data, context); }, [](void* value, RDGPrepareContext& context) noexcept {
                auto& callback = *static_cast<Callback*>(value);
                return !callback.Prepare || callback.Prepare.Get()(callback.Data, context); });
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
        auto callback = make_shared<Callback>(Callback{std::move(data), record, prepare});
        const auto index = AddCopyPassImpl(name, std::move(callback), [](const void* value, RDGCopyPassContext& context) noexcept {
                const auto& callback = *static_cast<const Callback*>(value);
                return callback.Record(callback.Data, context); }, [](void* value, RDGPrepareContext& context) noexcept {
                auto& callback = *static_cast<Callback*>(value);
                return !callback.Prepare || callback.Prepare.Get()(callback.Data, context); });
        return {_data, index};
    }

    // 执行一次 CPU 校验与裁剪；声明无效时记录日志并返回空值。
    Nullable<CompiledRDG*> Compile();

private:
    uint32_t AddRasterPassImpl(std::string_view name, shared_ptr<void> callback, bool (*execute)(const void*, RDGRasterPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept);
    uint32_t AddComputePassImpl(std::string_view name, shared_ptr<void> callback, bool (*execute)(const void*, RDGComputePassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept);
    uint32_t AddCopyPassImpl(std::string_view name, shared_ptr<void> callback, bool (*execute)(const void*, RDGCopyPassContext&) noexcept, bool (*prepare)(void*, RDGPrepareContext&) noexcept);
    FrameSlot& _slot;
    detail::RDGData& _data;
};

}  // namespace radray
