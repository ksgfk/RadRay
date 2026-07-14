#pragma once

#include <limits>

#include <radray/render/common.h>
#include <radray/types.h>

namespace radray {

class HostWriteBatch;

class GpuMesh {
public:
    struct DrawData {
        render::VertexBufferView Vbv;
        render::IndexBufferView Ibv;
    };

    vector<unique_ptr<render::Buffer>> Buffers;
    vector<DrawData> Draws;
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

}  // namespace radray
