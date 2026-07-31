#pragma once

#include <limits>
#include <optional>
#include <span>

#include <radray/intrusive_ptr.h>
#include <radray/nullable.h>
#include <radray/render/rhi.h>
#include <radray/runtime/asset_manager.h>
#include <radray/shader/shader_manifest.h>
#include <radray/types.h>

namespace radray {

class MeshResource;

/// 持有由网格资源创建的 GPU 缓冲区及绘制视图。
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

/// 收集持久映射缓冲区的写入范围，以便统一批量刷新。
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

/// 在作用域内映射缓冲区范围，并自动完成相应的主机访问操作。
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

/// 持有持久映射的上传缓冲区，并提供线性子分配。
class MappedUploadPage {
public:
    struct Allocation {
        render::Buffer* Target{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};

        static constexpr Allocation Invalid() noexcept { return {}; }
        bool IsValid() const noexcept { return Target != nullptr; }
    };

    /// 仅可移动的映射切片，提交时记录实际写入范围。
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

/// 上传堆暂存页池，按页线性分配，并在关联 flight 完成后回收标准页。
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

    /// 从上传页中预留暂存内存；大于标准页的请求会使用一次性缓冲区。
    Reservation Reserve(uint64_t size, uint64_t alignment = 1);

    /// 将所有活跃暂存页移入指定 flight 的待回收列表。
    void RetireToFlight(uint32_t flightIndex);

    /// 回收已完成 flight 的标准页，并释放其一次性缓冲区。
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

/// 向外部命令缓冲区记录资源复制命令，并管理所需的暂存页。
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

/// 从上传块中分配满足对齐要求的帧内常量缓冲区切片。
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

    /// 持有一个用作 Arena 后备存储的映射上传页。
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

}  // namespace radray

namespace radray {

class ShaderAsset;
class ShaderPassProgram;
struct ShaderAssetDiagnostic;

struct GraphicsPipelineStateKey {
    ShaderPassProgram* Program{nullptr};
    render::RenderPass* CompatibleRenderPass{nullptr};
    render::PrimitiveState Primitive{render::PrimitiveState::Default()};
    std::optional<render::DepthStencilState> DepthStencil{};
    render::MultiSampleState MultiSample{};
    std::span<const render::ColorTargetState> ColorTargets{};
};

class PipelineStateCache {
public:
    explicit PipelineStateCache(render::Device* device) noexcept;
    ~PipelineStateCache() noexcept;
    PipelineStateCache(const PipelineStateCache&) = delete;
    PipelineStateCache& operator=(const PipelineStateCache&) = delete;

    Nullable<render::GraphicsPipelineState*> GetOrCreateGraphics(
        const StreamingAssetRefAny& asset,
        const GraphicsPipelineStateKey& key,
        const ShaderVariantKey& variant,
        render::ShaderBlobCategory category,
        ShaderAssetDiagnostic& outDiag) noexcept;
    uint32_t RemovePipelineStatesUsing(const ShaderAsset* asset) noexcept;
    void Clear() noexcept;

    uint32_t GetGraphicsPipelineStateCount() const noexcept {
        return static_cast<uint32_t>(_graphics.size());
    }
    uint64_t GetGraphicsHitCount() const noexcept { return _graphicsHits; }
    uint64_t GetGraphicsMissCount() const noexcept { return _graphicsMisses; }

private:
    struct GraphicsEntry {
        ShaderPassProgram* Program{nullptr};
        render::RenderPass* CompatibleRenderPass{nullptr};
        vector<std::pair<render::ShaderStage, ShaderHash>> StageKeys;
        render::PrimitiveState Primitive{};
        std::optional<render::DepthStencilState> DepthStencil{};
        render::MultiSampleState MultiSample{};
        vector<render::ColorTargetState> ColorTargets;
        const ShaderAsset* Owner{nullptr};
        /// 【保住 Program 与 layout 的唯一一份引用】: Program 指向 ShaderAsset 内部的
        /// ShaderPassProgram, 而后端 PSO 又存着 PipelineLayout 裸指针 (D3D12 的
        /// GraphicsPsoD3D12 存 RootSigD3D12* 并在每次 bind 时解引用), layout 的生死由
        /// ShaderPassProgram 持有的 SharedPipelineLayout 引用计数决定。
        ///
        /// 【为何一份就够了】: 引用计数是资产生命周期的唯一权威 —— 没有任何入口能在
        /// 计数非零时销毁槽位。故本条目持有 Ref 即同时保住了资产、Program 和 layout,
        /// 从前那两个独立成员 (shared_ptr<void> Content / IntrusivePtr<SharedPipelineLayout>
        /// Layout) 都是为了防御"无视计数的强制卸载"而存在, 那条路已经不存在了。
        ///
        /// 【声明顺序有意义】: 必须在 Object 之前, 析构逆序保证 PSO 先死, 之后才放开
        /// 资产引用。
        StreamingAssetRefAny Ref;
        unique_ptr<render::GraphicsPipelineState> Object;
    };

    render::Device* _device{nullptr};
    vector<GraphicsEntry> _graphics;
    uint64_t _graphicsHits{0};
    uint64_t _graphicsMisses{0};
};

}  // namespace radray
