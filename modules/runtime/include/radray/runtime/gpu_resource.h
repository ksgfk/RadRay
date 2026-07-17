#pragma once

#include <limits>
#include <optional>
#include <span>

#include <radray/nullable.h>
#include <radray/guid.h>
#include <radray/render/common.h>
#include <radray/types.h>

namespace radray::render {
class Dxc;
}  // namespace radray::render

namespace radray {

class AssetManager;
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
    unordered_map<render::SamplerDescriptor, unique_ptr<render::Sampler>> _cache;
};

/// 单 stage 编译模块的 key。
///
/// Defines 是根据 ShaderKeywordGroupDesc::Stages 投影到当前 Stage 后的集合。这样
/// 只影响 pixel 的 keyword 不会导致 vertex module 被重复编译。Stage 必须是单个
/// stage bit，而不是 Graphics 等组合值。
struct ShaderModuleKey {
    Guid Shader{};
    vector<string> Defines{};
    uint32_t PassIndex{0};
    render::ShaderStage Stage{render::ShaderStage::UNKNOWN};

    friend bool operator==(const ShaderModuleKey&, const ShaderModuleKey&) noexcept;
};

}  // namespace radray

namespace std {

template <>
struct hash<radray::ShaderModuleKey> {
    size_t operator()(const radray::ShaderModuleKey& key) const noexcept;
};

}  // namespace std

namespace radray {

/// 按 ShaderAsset/pass/stage/defines 编译并缓存单 stage 的 GPU shader module。
///
/// Device、Dxc 和 AssetManager 均为非拥有依赖，必须比缓存活得更久。缓存只在 miss
/// 时访问 AssetManager；ShaderAsset 必须已经 Ready。返回的裸指针在 Clear 或缓存
/// 析构前保持稳定。该类型与 AssetManager 一样只允许在其所属线程调用。
class ShaderModuleCache {
public:
    ShaderModuleCache(
        render::Device* device,
        render::Dxc* dxc,
        AssetManager* assetManager,
        string shaderSourceRoot) noexcept;
    ShaderModuleCache(const ShaderModuleCache&) = delete;
    ShaderModuleCache(ShaderModuleCache&&) = delete;
    ShaderModuleCache& operator=(const ShaderModuleCache&) = delete;
    ShaderModuleCache& operator=(ShaderModuleCache&&) = delete;
    ~ShaderModuleCache() noexcept;

    /// 命中时返回已有 module；miss 时同步编译并创建。失败不写入缓存。
    Nullable<render::Shader*> GetOrCreate(const ShaderModuleKey& key) noexcept;

    void Clear() noexcept;
    size_t GetCount() const noexcept { return _cache.size(); }

private:
    render::Device* _device{nullptr};
    render::Dxc* _dxc{nullptr};
    AssetManager* _assetManager{nullptr};
    string _shaderSourceRoot;
    unordered_map<ShaderModuleKey, unique_ptr<render::Shader>> _cache;
};

}  // namespace radray
