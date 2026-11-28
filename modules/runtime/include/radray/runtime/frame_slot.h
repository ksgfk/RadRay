#pragma once

// 帧循环与所有权契约见 docs/architecture/runtime.md。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include <radray/gpu/types.h>
#include <radray/nullable.h>
#include <radray/types.h>
#include <radray/logger.h>
#include <radray/memory.h>
#include <radray/scope_guard.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/mesh_pass_processor.h>
#include <radray/runtime/gpu_device.h>
#include <radray/runtime/transient_parameter_pool.h>

namespace radray {

class MeshPassFrameDrawList {
public:
    explicit MeshPassFrameDrawList(MeshPassSortMode mode = MeshPassSortMode::StateFirst) noexcept;
    MeshPassFrameDrawList(MeshPassFrameDrawList&& other) noexcept;
    MeshPassFrameDrawList& operator=(MeshPassFrameDrawList&& other) noexcept;
    MeshPassFrameDrawList(const MeshPassFrameDrawList&) = delete;
    MeshPassFrameDrawList& operator=(const MeshPassFrameDrawList&) = delete;

    // 排序前固定动态命令指针；奇数趟基数排序可能与内部暂存缓冲交换，
    // 排序前持有的 VisibleCommands 底层指针/span 视图在排序后失效，须在排序后重新获取；提交只读冻结后的命令。
    void SortVisibleCommands();
    void SetSortMode(MeshPassSortMode mode) noexcept;
    MeshPassSortMode GetSortMode() const noexcept;
    bool Submit(gpu::GraphicsCommandEncoder& encoder, const MeshPassSubmitArgs& submitArgs = {}) const noexcept;
    void Clear() noexcept;

    vector<VisibleMeshDrawCommand> VisibleCommands;
    DynamicMeshPassDrawListContext DynamicContext;
    PreparedMeshPassIndirectDraws IndirectDraws;

private:
    vector<VisibleMeshDrawCommand> _scratchCommands;
};

class FrameSlot final {
public:
    ~FrameSlot() noexcept;

    FrameSlot(const FrameSlot&) = delete;
    FrameSlot& operator=(const FrameSlot&) = delete;
    FrameSlot(FrameSlot&&) = delete;
    FrameSlot& operator=(FrameSlot&&) = delete;

    gpu::Device& GetDevice() noexcept;
    gpu::CommandQueue& GetQueue() noexcept;
    uint32_t GetIndex() const noexcept;

    // 调度器内的帧编号，由 BeginFrame 分配并保持到 Wait 结束。
    // 从零开始，每次 Submit 完成后递增。
    uint64_t GetFrameId() const noexcept;

    MeshPassFrameDrawList& GetPassDrawList(MeshPassId pass);
    const MeshPassFrameDrawList& GetPassDrawList(MeshPassId pass) const noexcept;

    // 当前帧 BeginFrame 后可登记；提交完成或 RunFrame 因设备丢失返回时关闭登记窗口。
    // 成功对象按构造完成的逆序，在等待、命令重置及 CPU 记录清空后统一析构。
    // 构造失败不登记该对象；已成功构造的嵌套对象仍由槽清理。
    // 资源 owner 应先于依赖它的 view/绑定登记，或放在同一个 RAII 对象中。
    // 移入已停止新增使用的旧 owner，可保活到本槽下次回收或 WaitIdle。
    // 不得手动销毁对象，构造/清理期间不得重入槽回收或调度器。
    template <class T, class... Args>
    T* Emplace(Args&&... args) {
        static_assert(std::is_object_v<T> && !std::is_array_v<T>);
        static_assert(std::is_same_v<T, std::remove_cv_t<T>>);
        static_assert(std::is_nothrow_destructible_v<T>);
#if RADRAY_IS_DEBUG
        RADRAY_ASSERT(_inFrameCallback);
        ++_constructionDepth;
        auto construction = MakeScopeGuard([this]() noexcept { --_constructionDepth; });
#endif
        std::pmr::polymorphic_allocator<> allocator{&GetMemoryResource()};
        if constexpr (std::is_trivially_destructible_v<T>) {
            return allocator.new_object<T>(std::forward<Args>(args)...);
        } else {
            auto* object = allocator.new_object<ObjectNode<T>>(std::forward<Args>(args)...);
            object->Previous = _objects;
            _objects = object;
            return &object->Value;
        }
    }

    struct AllocationCounts {
        uint64_t Calls{};
        uint64_t Bytes{};
    };
    struct MemoryUsage {
        uint64_t InitialBytes{};
        AllocationCounts LinearOverflow;
        AllocationCounts ScratchGrowth;
    };
    // 本次槽使用期间的上游申请量。首次分配初始线性缓冲的字节单列，槽回收时清零。
    MemoryUsage GetMemoryUsage() const noexcept;

    // 帧内线性存储，首次访问按需申请初始缓冲；外部容器须在帧对象清理前销毁。
    // 不得 release 此资源；槽回收先析构对象再重置存储，保留初始缓冲；下一帧重新获取资源引用。
    std::pmr::memory_resource& GetMemoryResource();

    // 仅本槽串行 CPU 阶段的局部工作容器使用，须在作用域结束时销毁。
    // 不得 release 此资源或让录制结果引用临时容器；pool 缓存跨帧保留。
    std::pmr::memory_resource& GetScratchMemoryResource() noexcept;

    // 在 Prepare 期间调用；每次借用本槽拥有的不同命令缓冲。
    // 本槽下次重置时，借用失效，命令缓冲可被复用。
    // 调用方负责 Begin/End；共享分配器同一时刻只允许一份命令缓冲处于录制状态。
    Nullable<gpu::CommandBuffer*> AcquireCommandBuffer();
    Nullable<gpu::ShaderParameterSet*> AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout, std::span<const TransientParameterBinding> bindings);
    Nullable<gpu::ShaderParameterSet*> AcquireUninitializedParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout);

private:
    friend class FrameScheduler;

    static Nullable<unique_ptr<FrameSlot>> Create(DeviceContext& device, uint32_t index, Nullable<std::pmr::memory_resource*> memoryUpstream);
    FrameSlot(DeviceContext& device, uint32_t index, unique_ptr<gpu::CommandAllocator> allocator, Nullable<std::pmr::memory_resource*> memoryUpstream);

    // 要求使用已完成；共享资源所有者销毁前，所有引用它的命令分配器必须先重置。
    void ResetCommandStorage() noexcept;
    void ResetFrameData() noexcept;

    struct Object {
        Nullable<Object*> Previous;
        void (*Destroy)(Object*) noexcept;
    };

    template <class T>
    class ObjectNode final : public Object {
    public:
        template <class... Args>
        explicit ObjectNode(Args&&... args)
            : Object{nullptr, +[](Object* object) noexcept { std::destroy_at(static_cast<ObjectNode*>(object)); }},
              Value(std::forward<Args>(args)...) {}
        T Value;
    };

    class CountingResource final : public std::pmr::memory_resource {
    public:
        explicit CountingResource(std::pmr::memory_resource& upstream) noexcept;
        AllocationCounts Counts{};

    private:
        void* do_allocate(size_t bytes, size_t alignment) override;
        void do_deallocate(void* data, size_t bytes, size_t alignment) override;
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;
        std::pmr::memory_resource& _upstream;
    };

    DeviceContext& _device;
    uint64_t _frameId{0};
    unique_ptr<gpu::CommandAllocator> _allocator;
    TransientParameterSetPool _parameterSets;
    vector<unique_ptr<gpu::CommandBuffer>> _commands;
    HeapMemoryResource _memoryHeap;
    std::pmr::memory_resource& _memoryUpstream;
    CountingResource _linearUpstream{_memoryUpstream};
    CountingResource _scratchUpstream{_memoryUpstream};
    uint64_t _initialBytes{};
    std::pmr::unsynchronized_pool_resource _scratchPool;
    const size_t _initialCapacity{128 * 1024};
    Nullable<void*> _initialBuffer{nullptr};
    std::optional<std::pmr::monotonic_buffer_resource> _memory;
    Nullable<Object*> _objects{nullptr};
    vector<MeshPassFrameDrawList> _passDrawLists;
    size_t _nextCommandIndex{0};
    const uint32_t _index;
    bool _inFlight{false};
#if RADRAY_IS_DEBUG
    bool _inFrameCallback{false};
    size_t _constructionDepth{0};
#endif
};

}  // namespace radray
