#pragma once

#include <limits>
#include <optional>
#include <span>

#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/types.h>

namespace radray {

class MeshResource;

class GpuMesh {
public:
    struct DrawData {
        render::VertexBufferView Vbv;
        render::IndexBufferView Ibv;
    };

    vector<unique_ptr<render::Buffer>> Buffers;
    vector<DrawData> Draws;
};

struct UploadMemoryStats {
    uint64_t PageCount{0};
    uint64_t PageCapacityBytes{0};
    uint64_t CommitCount{0};
    uint64_t CommittedBytes{0};
    uint64_t RecordedRangeCount{0};
    uint64_t FlushedRangeCount{0};
};

class HostWriteBatch {
public:
    HostWriteBatch();

    void Record(render::Buffer* target, render::BufferRange range);
    void Flush(render::Device& device) noexcept;

    bool Empty() const noexcept { return _ranges.empty(); }
    bool IsSealed() const noexcept { return _sealed; }
    std::span<const render::MappedBufferRange> GetRanges() const noexcept { return _ranges; }
    const UploadMemoryStats& GetStats() const noexcept { return _stats; }

    void Seal() noexcept { _sealed = true; }
    void Reset() noexcept;
    void RecordPageAllocation(uint64_t capacity) noexcept;

private:
    vector<render::MappedBufferRange> _ranges;
    UploadMemoryStats _stats{};
    bool _sealed{false};
};

class ScopedBufferMap {
public:
    ScopedBufferMap(render::Buffer* buffer, render::BufferRange range) noexcept;
    ~ScopedBufferMap() noexcept;

    ScopedBufferMap(const ScopedBufferMap&) = delete;
    ScopedBufferMap& operator=(const ScopedBufferMap&) = delete;
    ScopedBufferMap(ScopedBufferMap&&) = delete;
    ScopedBufferMap& operator=(ScopedBufferMap&&) = delete;

    void* Data() const noexcept { return _data; }

    template <typename T>
    T* DataAs() const noexcept {
        return static_cast<T*>(_data);
    }

    explicit operator bool() const noexcept { return _data != nullptr; }

private:
    render::Buffer* _buffer{nullptr};
    void* _data{nullptr};
    render::BufferRange _range{};
    bool _write{false};
};

struct BufferUploadRequest {
    std::span<const byte> SrcData;
    render::Buffer* DstBuffer;
    uint64_t DstOffset{0};
    render::BufferStates Before{render::BufferState::Common};
    render::BufferStates After{render::BufferState::Common};
};

struct TextureUploadRequest {
    std::span<const byte> SrcData;
    render::Texture* DstTexture;
    render::SubresourceRange DstRange;
    uint64_t SrcRowPitch{0};
    render::TextureStates Before{render::TextureState::Undefined};
    render::TextureStates After{render::TextureState::ShaderRead};
};

class MappedUploadPage {
public:
    struct Allocation {
        render::Buffer* Target{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};

        static constexpr Allocation Invalid() noexcept { return {}; }
        bool IsValid() const noexcept { return Target != nullptr; }
    };

    class Reservation {
    public:
        Reservation() noexcept = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&& other) noexcept;
        Reservation& operator=(Reservation&& other) noexcept;
        ~Reservation() noexcept;

        void* Data() const noexcept { return _data; }
        render::Buffer* Target() const noexcept { return _target; }
        uint64_t Offset() const noexcept { return _offset; }
        uint64_t Capacity() const noexcept { return _capacity; }
        bool IsValid() const noexcept { return _target != nullptr; }

        Allocation Commit(uint64_t actualSize);

    private:
        friend class MappedUploadPage;

        Reservation(
            render::Buffer* target,
            void* data,
            uint64_t offset,
            uint64_t capacity,
            HostWriteBatch* hostWrites) noexcept;
        void AbandonCheck() const noexcept;

        render::Buffer* _target{nullptr};
        void* _data{nullptr};
        uint64_t _offset{0};
        uint64_t _capacity{0};
        HostWriteBatch* _hostWrites{nullptr};
        bool _committed{true};
    };

    explicit MappedUploadPage(
        unique_ptr<render::Buffer> buffer,
        Nullable<HostWriteBatch*> allocationStats = nullptr) noexcept;
    MappedUploadPage(const MappedUploadPage&) = delete;
    MappedUploadPage& operator=(const MappedUploadPage&) = delete;
    ~MappedUploadPage() noexcept;

    Reservation Reserve(uint64_t size, uint64_t alignment, HostWriteBatch& hostWrites);
    Reservation ReserveAt(uint64_t offset, uint64_t size, HostWriteBatch& hostWrites);
    void Reset() noexcept { _used = 0; }

    render::Buffer* GetBuffer() const noexcept { return _buffer.get(); }
    uint64_t GetCapacity() const noexcept { return _buffer != nullptr ? _buffer->GetDesc().Size : 0; }
    uint64_t GetUsed() const noexcept { return _used; }
private:
    unique_ptr<render::Buffer> _buffer;
    void* _mapped{nullptr};
    uint64_t _used{0};
};

/// Upload heap staging page pool. Suballocates each page linearly and recycles
/// whole standard pages after their associated flight has completed.
class StagingBufferPool {
public:
    using Allocation = MappedUploadPage::Allocation;
    using Reservation = MappedUploadPage::Reservation;

    struct Descriptor {
        uint64_t PageSize{8ull * 1024 * 1024};
        uint64_t MaxCachedBytes{64ull * 1024 * 1024};
        uint32_t MaxCachedPages{8};
    };

    StagingBufferPool(render::Device* device, uint32_t flightCount, const Descriptor& desc) noexcept;
    explicit StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept;
    ~StagingBufferPool() noexcept;
    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    void BeginFlight(HostWriteBatch& hostWrites);

    /// Reserves staging memory from an upload page. Requests larger than a page
    /// receive a one-shot buffer.
    Reservation Reserve(uint64_t size, uint64_t alignment = 1);

    /// Moves all active staging pages into a flight's pending list.
    void RetireToFlight(uint32_t flightIndex);

    /// Recycles standard pages for a completed flight and releases one-shot buffers.
    void CollectFlight(uint32_t flightIndex);

private:
    struct Page {
        unique_ptr<MappedUploadPage> Upload;
        bool Cacheable{true};
    };

    Page CreatePage(uint64_t capacity, bool cacheable);
    Page& AcquireStandardPage();
    void TrimFreeList() noexcept;

    render::Device* _device;
    Descriptor _desc;
    vector<Page> _active;
    vector<vector<Page>> _pending;
    vector<Page> _freeList;
    uint64_t _nextPageId{0};
    HostWriteBatch* _hostWrites{nullptr};
};

/// Records copy commands to an externally supplied command buffer and manages
/// the staging pages required by those commands.
class ResourceUploader {
public:
    ResourceUploader(render::Device* device, uint32_t flightCount);
    ~ResourceUploader() noexcept;
    ResourceUploader(const ResourceUploader&) = delete;
    ResourceUploader& operator=(const ResourceUploader&) = delete;

    void BeginFlight(uint32_t flightIndex, HostWriteBatch& hostWrites);
    void UploadBuffer(render::CommandBuffer* cmdBuffer, const BufferUploadRequest& request);
    void UploadTexture(render::CommandBuffer* cmdBuffer, const TextureUploadRequest& request);
    std::optional<GpuMesh> UploadMeshResource(
        render::CommandBuffer* cmdBuffer,
        const MeshResource& meshResource);
    void EndFlight(uint32_t flightIndex);
    void CollectFlight(uint32_t flightIndex);

    render::Device* GetDevice() const noexcept { return _device; }

private:
    render::Device* _device;
    StagingBufferPool _stagingPool;
    uint32_t _flightCount{0};
    uint32_t _activeFlightIndex{std::numeric_limits<uint32_t>::max()};
};

class DynamicCBufferArena {
public:
    struct Descriptor {
        uint64_t BasicSize{256 * 1024};
        uint64_t Alignment{256};
        uint64_t MaxResetSize{std::numeric_limits<uint64_t>::max()};
        string NamePrefix{};
    };

    using Allocation = MappedUploadPage::Allocation;
    using Reservation = MappedUploadPage::Reservation;

    class Block {
    public:
        explicit Block(unique_ptr<MappedUploadPage> page) noexcept;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() noexcept = default;

        unique_ptr<MappedUploadPage> Page;
    };

    DynamicCBufferArena(
        render::Device* device,
        HostWriteBatch* hostWrites,
        const Descriptor& desc) noexcept;
    DynamicCBufferArena(render::Device* device, HostWriteBatch* hostWrites) noexcept;
    DynamicCBufferArena(const DynamicCBufferArena&) = delete;
    DynamicCBufferArena& operator=(const DynamicCBufferArena&) = delete;
    DynamicCBufferArena(DynamicCBufferArena&& other) noexcept;
    DynamicCBufferArena& operator=(DynamicCBufferArena&& other) noexcept;
    ~DynamicCBufferArena() noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;
    Reservation Reserve(uint64_t size) noexcept;
    void Reset() noexcept;
    void Clear() noexcept;
    bool Contains(const render::Buffer* buffer) const noexcept;
    uint64_t GetHighWatermark() const noexcept { return _highWatermark; }

    friend void swap(DynamicCBufferArena& a, DynamicCBufferArena& b) noexcept;

private:
    Nullable<Block*> GetOrCreateBlock(uint64_t size) noexcept;

    render::Device* _device;
    HostWriteBatch* _hostWrites;
    vector<unique_ptr<Block>> _blocks;
    Descriptor _desc;
    size_t _activeBlockIndex{};
    uint64_t _minBlockSize{};
    uint64_t _allocatedThisFrame{};
    uint64_t _highWatermark{};
};

class MaterialConstantPool {
public:
    struct Allocation {
        render::Buffer* Target{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};
        uint64_t ReservedSize{0};
        uint32_t BlockIndex{std::numeric_limits<uint32_t>::max()};

        bool IsValid() const noexcept { return Target != nullptr; }
    };

    class Reservation {
    public:
        Reservation() noexcept = default;
        Reservation(const Reservation&) = delete;
        Reservation& operator=(const Reservation&) = delete;
        Reservation(Reservation&&) noexcept = default;
        Reservation& operator=(Reservation&&) noexcept = default;

        void* Data() const noexcept { return _reservation.Data(); }
        render::Buffer* Target() const noexcept { return _reservation.Target(); }
        uint64_t Offset() const noexcept { return _reservation.Offset(); }
        uint64_t Capacity() const noexcept { return _reservation.Capacity(); }
        bool IsValid() const noexcept { return _reservation.IsValid(); }

        Allocation Commit(uint64_t actualSize);

    private:
        friend class MaterialConstantPool;
        Reservation(
            MappedUploadPage::Reservation reservation,
            uint64_t reservedSize,
            uint32_t blockIndex) noexcept
            : _reservation(std::move(reservation)),
              _reservedSize(reservedSize),
              _blockIndex(blockIndex) {}

        MappedUploadPage::Reservation _reservation;
        uint64_t _reservedSize{0};
        uint32_t _blockIndex{std::numeric_limits<uint32_t>::max()};
    };

    MaterialConstantPool(
        render::Device* device,
        uint64_t initialSize = 256 * 1024,
        uint64_t alignment = 256) noexcept;
    MaterialConstantPool(const MaterialConstantPool&) = delete;
    MaterialConstantPool& operator=(const MaterialConstantPool&) = delete;
    ~MaterialConstantPool() noexcept;

    Reservation Reserve(uint64_t size, HostWriteBatch& hostWrites) noexcept;
    void Release(const Allocation& allocation) noexcept;
    uint64_t GetHighWatermark() const noexcept { return _highWatermark; }

private:
    struct Block {
        unique_ptr<MappedUploadPage> Page;
    };
    struct FreeSlice {
        uint32_t BlockIndex{0};
        uint64_t Offset{0};
        uint64_t Size{0};
    };

    Nullable<Block*> CreateBlock(
        uint64_t minimumSize,
        HostWriteBatch& hostWrites) noexcept;

    render::Device* _device{nullptr};
    uint64_t _initialSize{0};
    uint64_t _alignment{0};
    vector<unique_ptr<Block>> _blocks;
    vector<FreeSlice> _freeSlices;
    uint64_t _activeBytes{0};
    uint64_t _highWatermark{0};
};

struct RuntimeRenderCounters {
    uint64_t DescriptorGroupCreates{0};
    uint64_t DescriptorGroupUpdates{0};
    uint64_t DescriptorGroupBinds{0};
    uint64_t DynamicOffsetBinds{0};
    uint64_t PipelineBinds{0};
    uint64_t PipelineCacheHits{0};
    uint64_t PipelineCacheMisses{0};
    uint64_t ShaderVariantCacheHits{0};
    uint64_t ShaderVariantCacheMisses{0};
    uint64_t DrawStateCacheHits{0};
    uint64_t DrawStateCacheMisses{0};
    uint64_t DrawCommandTemplateHits{0};
    uint64_t DrawCommandTemplateMisses{0};
    uint64_t SystemGroupCacheHits{0};
    uint64_t SystemGroupCacheMisses{0};
    uint64_t MaterialGroupCacheHits{0};
    uint64_t MaterialGroupCacheMisses{0};
    uint64_t BindingPlanCacheHits{0};
    uint64_t BindingPlanCacheMisses{0};
    uint64_t BindingResolutionFailures{0};
    uint64_t ErrorFallbackDraws{0};
    uint64_t Draws{0};
    uint64_t DrawInstances{0};
    uint64_t ObjectArenaHighWatermark{0};
    uint64_t ViewArenaHighWatermark{0};
};

struct FrameResolvedBindingGroupCacheEntry {
    render::PipelineLayout* Layout{nullptr};
    uint32_t GroupIndex{0};
    vector<uint64_t> DescriptorKey;
    vector<render::Buffer*> DynamicBuffers;
    bool Persistent{false};
    unique_ptr<render::BindingGroup> Group;
};

struct FrameResolvedBindingStateCacheEntry {
    const void* Plan{nullptr};
    uint32_t GroupIndex{0};
    uint64_t MaterialKeyLo{0};
    uint64_t MaterialKeyHi{0};
    const void* Object{nullptr};
    uint64_t ViewRevision{0};
    uint64_t PassRevision{0};
    render::BindingGroup* Group{nullptr};
    vector<uint32_t> DynamicOffsets;
    uint64_t ObjectBufferPage{0};
};

struct FrameObjectBinding {
    const void* Proxy{nullptr};
    DynamicCBufferArena::Allocation Allocation{};
};

class FrameResources {
public:
    FrameResources(render::Device* device, HostWriteBatch* hostWrites) noexcept;

    void Reset() noexcept;
    HostWriteBatch& GetHostWrites() const noexcept { return *_hostWrites; }

    DynamicCBufferArena PerObjectArena;
    DynamicCBufferArena ViewArena;
    unique_ptr<render::DescriptorPool> SystemDescriptorPool;
    unique_ptr<render::DescriptorPool> TransientDescriptorPool;
    vector<FrameResolvedBindingGroupCacheEntry> ResolvedGroups;
    vector<FrameResolvedBindingStateCacheEntry> ResolvedBindingStates;
    vector<FrameObjectBinding> ObjectBindings;
    vector<shared_ptr<const void>> RetainedObjects;
    vector<unique_ptr<render::RenderBase>> RetireList;
    RuntimeRenderCounters Counters{};
    uint64_t Generation{1};

private:
    HostWriteBatch* _hostWrites;
};

class RenderPassRegistry {
public:
    explicit RenderPassRegistry(render::Device* device) noexcept;
    ~RenderPassRegistry() noexcept;
    RenderPassRegistry(const RenderPassRegistry&) = delete;
    RenderPassRegistry& operator=(const RenderPassRegistry&) = delete;

    Nullable<render::RenderPass*> GetOrCreateRenderPass(
        const render::RenderPassDescriptor& desc) noexcept;

    Nullable<render::Framebuffer*> GetOrCreateFramebuffer(
        render::RenderPass* pass,
        std::span<render::TextureView* const> colorAttachments,
        render::TextureView* depthStencilAttachment,
        uint32_t width,
        uint32_t height,
        uint32_t layers = 1) noexcept;

    void RemoveFramebuffersUsing(render::TextureView* attachment) noexcept;
    void ClearFramebuffers() noexcept;
    void Clear() noexcept;

    uint32_t GetRenderPassCount() const noexcept { return static_cast<uint32_t>(_passes.size()); }
    uint32_t GetFramebufferCount() const noexcept { return static_cast<uint32_t>(_framebuffers.size()); }
    uint64_t GetRenderPassHitCount() const noexcept { return _renderPassHits; }
    uint64_t GetRenderPassMissCount() const noexcept { return _renderPassMisses; }
    uint64_t GetFramebufferHitCount() const noexcept { return _framebufferHits; }
    uint64_t GetFramebufferMissCount() const noexcept { return _framebufferMisses; }

private:
    struct PassEntry {
        vector<render::RenderPassColorAttachmentDescriptor> ColorAttachments;
        std::optional<render::RenderPassDepthStencilAttachmentDescriptor> DepthStencilAttachment;
        unique_ptr<render::RenderPass> Object;
    };

    struct FramebufferEntry {
        render::RenderPass* Pass{nullptr};
        vector<render::TextureView*> ColorAttachments;
        render::TextureView* DepthStencilAttachment{nullptr};
        uint32_t Width{0};
        uint32_t Height{0};
        uint32_t Layers{1};
        unique_ptr<render::Framebuffer> Object;
    };

    render::Device* _device{nullptr};
    vector<PassEntry> _passes;
    vector<FramebufferEntry> _framebuffers;
    uint64_t _renderPassHits{0};
    uint64_t _renderPassMisses{0};
    uint64_t _framebufferHits{0};
    uint64_t _framebufferMisses{0};
};

}  // namespace radray
