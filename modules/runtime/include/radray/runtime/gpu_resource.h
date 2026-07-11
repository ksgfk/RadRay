#pragma once

#include <limits>

#include <radray/render/common.h>
#include <radray/types.h>

namespace radray {

class GpuMesh {
public:
    struct DrawData {
        render::VertexBufferView Vbv;
        render::IndexBufferView Ibv;
    };

    vector<unique_ptr<render::Buffer>> Buffers;
    vector<DrawData> Draws;
};

class DynamicCBufferArena {
public:
    struct Descriptor {
        uint64_t BasicSize{256 * 1024};
        uint64_t Alignment{256};
        uint64_t MaxResetSize{std::numeric_limits<uint64_t>::max()};
        string NamePrefix{};
    };

    struct Allocation {
        render::Buffer* Target{nullptr};
        void* Mapped{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};

        static constexpr Allocation Invalid() noexcept { return Allocation{}; }
    };

    class Block {
    public:
        explicit Block(unique_ptr<render::Buffer> buffer) noexcept;
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;
        ~Block() noexcept;

        unique_ptr<render::Buffer> Buffer;
        void* Mapped{nullptr};
        uint64_t Used{0};
    };

    DynamicCBufferArena(render::Device* device, const Descriptor& desc) noexcept;
    explicit DynamicCBufferArena(render::Device* device) noexcept;
    DynamicCBufferArena(const DynamicCBufferArena&) = delete;
    DynamicCBufferArena& operator=(const DynamicCBufferArena&) = delete;
    DynamicCBufferArena(DynamicCBufferArena&& other) noexcept;
    DynamicCBufferArena& operator=(DynamicCBufferArena&& other) noexcept;
    ~DynamicCBufferArena() noexcept;

    bool IsValid() const noexcept;
    void Destroy() noexcept;
    Allocation Allocate(uint64_t size) noexcept;
    void Reset() noexcept;
    void Clear() noexcept;
    bool Contains(const render::Buffer* buffer) const noexcept;
    uint64_t GetHighWatermark() const noexcept { return _highWatermark; }

    friend void swap(DynamicCBufferArena& a, DynamicCBufferArena& b) noexcept;

private:
    Nullable<Block*> GetOrCreateBlock(uint64_t size) noexcept;

    render::Device* _device;
    vector<unique_ptr<Block>> _blocks;
    Descriptor _desc;
    uint64_t _minBlockSize{};
    uint64_t _allocatedThisFrame{};
    uint64_t _highWatermark{};
};

class MaterialConstantPool {
public:
    struct Allocation {
        render::Buffer* Target{nullptr};
        void* Mapped{nullptr};
        uint64_t Offset{0};
        uint64_t Size{0};
        uint64_t ReservedSize{0};
        uint32_t BlockIndex{std::numeric_limits<uint32_t>::max()};

        bool IsValid() const noexcept { return Target != nullptr; }
    };

    MaterialConstantPool(
        render::Device* device,
        uint64_t initialSize = 256 * 1024,
        uint64_t alignment = 256) noexcept;
    MaterialConstantPool(const MaterialConstantPool&) = delete;
    MaterialConstantPool& operator=(const MaterialConstantPool&) = delete;
    ~MaterialConstantPool() noexcept;

    Allocation Allocate(uint64_t size) noexcept;
    void Release(const Allocation& allocation) noexcept;
    uint64_t GetHighWatermark() const noexcept { return _highWatermark; }

private:
    struct Block {
        unique_ptr<render::Buffer> Buffer;
        void* Mapped{nullptr};
        uint64_t Used{0};

        ~Block() noexcept;
    };
    struct FreeSlice {
        uint32_t BlockIndex{0};
        uint64_t Offset{0};
        uint64_t Size{0};
    };

    Nullable<Block*> CreateBlock(uint64_t minimumSize) noexcept;

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
    uint64_t Draws{0};
    uint64_t DrawInstances{0};
    uint64_t ObjectArenaHighWatermark{0};
    uint64_t ViewArenaHighWatermark{0};
};

struct FrameBindingGroupCacheEntry {
    render::PipelineLayout* Layout{nullptr};
    uint32_t GroupIndex{0};
    render::Buffer* DynamicBuffer{nullptr};
    vector<render::ResourceView*> Resources{};
    vector<render::Sampler*> Samplers{};
    unique_ptr<render::BindingGroup> Group{};
};

struct FrameObjectBinding {
    const void* Proxy{nullptr};
    DynamicCBufferArena::Allocation Allocation{};
};

class FrameResources {
public:
    explicit FrameResources(render::Device* device) noexcept;

    void Reset() noexcept;

    DynamicCBufferArena PerObjectArena;
    DynamicCBufferArena ViewArena;
    unique_ptr<render::DescriptorPool> SystemDescriptorPool;
    unique_ptr<render::DescriptorPool> TransientDescriptorPool;
    vector<FrameBindingGroupCacheEntry> SystemGroups;
    vector<FrameObjectBinding> ObjectBindings;
    vector<shared_ptr<const void>> RetainedObjects;
    vector<unique_ptr<render::RenderBase>> RetireList;
    RuntimeRenderCounters Counters{};
    uint64_t Generation{1};
};

}  // namespace radray
