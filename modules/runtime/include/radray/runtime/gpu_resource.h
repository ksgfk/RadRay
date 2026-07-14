#pragma once

#include <limits>
#include <optional>
#include <span>

#include <radray/nullable.h>
#include <radray/guid.h>
#include <radray/hash.h>
#include <radray/render/common.h>
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

/// 根据创建参数缓存渲染通道和帧缓冲区。
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

/// PSO 缓存的 POD key. 所有字段为标量 (无指针/optional/span),
/// 构造时以 `Key{}` 清零, 再逐字段赋值, 保证 padding 恒为 0, 从而可安全用于 PodHasher (byte-wise xxHash) 与 PodEqual (memcmp).
struct GraphicsPsoKey {
    Guid LayoutId;
    Guid VSId;
    Guid PSId;

    // PrimitiveState 展平
    int32_t Topology;
    int32_t FaceClockwise;
    int32_t Cull;
    int32_t Poly;
    uint32_t HasStripIndexFormat;
    int32_t StripIndexFormat;
    uint32_t UnclippedDepth;
    uint32_t Conservative;

    // DepthStencil 展平
    uint32_t HasDepthStencil;
    int32_t DSFormat;
    int32_t DepthCompare;
    uint32_t DepthWriteEnable;
    int32_t DepthBiasConstant;
    float DepthBiasSlopScale;
    float DepthBiasClamp;
    uint32_t HasStencil;
    int32_t StencilFrontCompare;
    int32_t StencilFrontFailOp;
    int32_t StencilFrontDepthFailOp;
    int32_t StencilFrontPassOp;
    int32_t StencilBackCompare;
    int32_t StencilBackFailOp;
    int32_t StencilBackDepthFailOp;
    int32_t StencilBackPassOp;
    uint32_t StencilReadMask;
    uint32_t StencilWriteMask;

    // MultiSample 展平
    uint32_t MsCount;
    uint64_t MsMask;
    uint32_t MsAlphaToCoverage;

    // ColorTargets 展平
    uint32_t ColorTargetCount;
    struct ColorTargetEntry {
        int32_t Format;
        uint32_t HasBlend;
        int32_t ColorSrc;
        int32_t ColorDst;
        int32_t ColorOp;
        int32_t AlphaSrc;
        int32_t AlphaDst;
        int32_t AlphaOp;
        uint32_t WriteMask;
    } ColorTargets[render::kMaxColorTargets];

    // VertexLayouts 展平
    uint32_t VertexLayoutCount;
    struct VertexLayoutEntry {
        uint64_t ArrayStride;
        int32_t StepMode;
        uint32_t ElemCount;
        struct ElemEntry {
            uint64_t Offset;
            char Semantic[render::kMaxSemanticLength];
            uint32_t SemanticIndex;
            int32_t Format;
            uint32_t Location;
        } Elems[render::kMaxVertexElementsPerLayout];
    } VertexLayouts[render::kMaxVertexBufferLayouts];
};

static_assert(std::is_trivially_copyable_v<GraphicsPsoKey>, "GraphicsPsoKey must be trivially copyable");

/// Sampler 缓存的纯 POD key (仿 GraphicsPsoKey)。
///
/// 所有字段为标量, 无指针 / optional / span。构造时以 `SamplerKey{}` 清零, 再逐字段赋值,
/// 保证 padding 恒为 0, 从而可安全用于 PodHasher (byte-wise xxHash) 与 PodEqual (memcmp)。
/// SamplerDescriptor::Compare 是 std::optional, 在此展平为 HasCompare + Compare 两个标量。
struct SamplerKey {
    int32_t AddressS;
    int32_t AddressT;
    int32_t AddressR;
    int32_t MinFilter;
    int32_t MagFilter;
    int32_t MipmapFilter;
    float LodMin;
    float LodMax;
    uint32_t HasCompare;
    int32_t Compare;
    uint32_t AnisotropyClamp;
};

static_assert(std::is_trivially_copyable_v<SamplerKey>, "SamplerKey must be trivially copyable");

/// 从 SamplerDescriptor 构造清零的 POD key。
SamplerKey BuildSamplerKey(const render::SamplerDescriptor& desc) noexcept;

/// 采样器缓存 (对应 UE5 的 GTextureSamplerStateCache / 各 RHI 后端 sampler cache)。
///
/// 设计要点:
/// - 按 SamplerDescriptor 去重: 相同状态的 sampler 只创建一次。
/// - unique_ptr 永生持有: 一经创建即缓存到 app 生命周期结束, 从不单独释放。
///   因此 GetOrCreate 返回的裸指针在缓存存活期内【永不悬垂】, 材质快照可安全跨帧/跨线程持有。
/// - sampler 是纯状态对象 (无数据), 组合数有限, 永生缓存无实际内存压力。
class SamplerCache {
public:
    explicit SamplerCache(render::Device* device) noexcept;
    SamplerCache(const SamplerCache&) = delete;
    SamplerCache(SamplerCache&&) = delete;
    SamplerCache& operator=(const SamplerCache&) = delete;
    SamplerCache& operator=(SamplerCache&&) = delete;
    ~SamplerCache() noexcept = default;

    /// 按 descriptor 去重取 sampler。命中返回缓存指针; 未命中创建并永生缓存。
    /// device 为空 / 创建失败返回 nullptr。返回的指针在本缓存存活期内稳定不悬垂。
    Nullable<render::Sampler*> GetOrCreate(const render::SamplerDescriptor& desc) noexcept;

private:
    render::Device* _device{nullptr};
    unordered_map<SamplerKey, unique_ptr<render::Sampler>, PodHasher<SamplerKey>, PodEqual<SamplerKey>> _cache;
};

}  // namespace radray
