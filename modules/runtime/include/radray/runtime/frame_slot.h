#pragma once

// 帧循环与所有权契约见 docs/architecture/runtime.md。

#include <type_traits>
#include <utility>

#include <radray/logger.h>
#include <radray/object_arena.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/mesh_pass_processor.h>

namespace radray {

class DeviceContext;
class FrameScheduler;

class MeshPassFrameDrawList {
public:
    explicit MeshPassFrameDrawList(MeshPassSortMode mode = MeshPassSortMode::StateFirst) noexcept;
    MeshPassFrameDrawList(MeshPassFrameDrawList&& other) noexcept;
    MeshPassFrameDrawList& operator=(MeshPassFrameDrawList&& other) noexcept;
    MeshPassFrameDrawList(const MeshPassFrameDrawList&) = delete;
    MeshPassFrameDrawList& operator=(const MeshPassFrameDrawList&) = delete;

    // 排序与提交都会先固定动态命令指针；调用前必须完成命令生成。
    void SortVisibleCommands();
    void SetSortMode(MeshPassSortMode mode) noexcept;
    MeshPassSortMode GetSortMode() const noexcept { return DynamicContext.GetSortMode(); }
    bool Submit(render::GraphicsCommandEncoder& encoder, const MeshPassSubmitArgs& submitArgs = {}) noexcept;
    void Clear() noexcept;

    vector<VisibleMeshDrawCommand> VisibleCommands;
    DynamicMeshPassDrawListContext DynamicContext;

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

    render::Device& GetDevice() noexcept;
    render::CommandQueue& GetQueue() noexcept;
    uint32_t GetIndex() const noexcept;

    // 调度器内的帧编号，由 BeginFrame 分配并保持到 Wait 结束。
    // 从零开始，每次 Record 完成后递增。
    uint64_t GetFrameId() const noexcept;

    MeshPassFrameDrawList& GetPassDrawList(MeshPassId pass);

    // 在 BeginFrame 之后或 Extract/Record 期间分配、登记帧内对象；对象存活到 Wait 结束。
    // 仅槽位可以 Reset/Release 此 arena；清理在命令录制与 CPU 记录清空后执行。
    // 清理回调不得重入 arena 或调度器。
    ObjectArena& GetArena() noexcept;

    // 仅可在 BeginFrame 之后至 Record 返回前，对当前槽调用；须先停止资源的一切新增使用。
    // 保留资源所有者，直到下次复用本槽或调用 WaitIdle，且所有槽的旧命令存储均已重置。
    // 存在依赖关系的资源所有者应归入同一个 RAII 对象。
    // 清理期间不得重入槽位或调度器。
    // Debug 检查调用窗口；Release 依赖调用方遵守此前置条件。
    template <typename T>
    void RetireResource(T&& resourceOwner) {
#if RADRAY_IS_DEBUG
        if (!_inFrameCallback) RADRAY_ABORT("FrameSlot: RetireResource requires the current begun frame");
#endif
        _retiredArena.Emplace<std::remove_cvref_t<T>>(std::forward<T>(resourceOwner));
    }

    // 在 Record 期间调用；每次借用本槽拥有的不同命令缓冲。
    // 本槽下次重置时，借用失效，命令缓冲可被复用。
    // 调用方负责 Begin/End；共享分配器同一时刻只允许一份命令缓冲处于录制状态。
    Nullable<render::CommandBuffer*> AcquireCommandBuffer();

private:
    friend class FrameScheduler;

    static Nullable<unique_ptr<FrameSlot>> Create(DeviceContext& device, uint32_t index);
    FrameSlot(DeviceContext& device, uint32_t index, unique_ptr<render::CommandAllocator> allocator) noexcept;

    // 要求使用已完成；共享资源所有者销毁前，所有引用它的命令分配器必须先重置。
    void ResetCommandStorage() noexcept;
    void ResetRetiredResources() noexcept;
    void ResetFrameData() noexcept;

    DeviceContext& _device;
    uint64_t _frameId{0};
    unique_ptr<render::CommandAllocator> _allocator;
    vector<unique_ptr<render::CommandBuffer>> _commands;
    ObjectArena _arena;
    ObjectArena _retiredArena{ObjectArenaOptions{.BlockSize = 1024, .MaxRetainedBytes = 4 * 1024}};
    vector<MeshPassFrameDrawList> _passDrawLists;
    size_t _nextCommandIndex{0};
    const uint32_t _index;
    bool _inFlight{false};
#if RADRAY_IS_DEBUG
    bool _inFrameCallback{false};
#endif
};

}  // namespace radray
