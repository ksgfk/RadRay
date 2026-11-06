#pragma once

#include <type_traits>
#include <utility>

#include <radray/logger.h>
#include <radray/object_arena.h>
#include <radray/runtime/mesh_draw_command.h>
#include <radray/runtime/mesh_pass_processor.h>

namespace radray {

class DeviceContext;
class FrameScheduler;

struct MeshPassFrameDrawList {
    MeshPassFrameDrawList() noexcept;
    MeshPassFrameDrawList(MeshPassFrameDrawList&& other) noexcept;
    MeshPassFrameDrawList& operator=(MeshPassFrameDrawList&& other) noexcept;
    MeshPassFrameDrawList(const MeshPassFrameDrawList&) = delete;
    MeshPassFrameDrawList& operator=(const MeshPassFrameDrawList&) = delete;

    // Both operations finalize dynamic pointers first; finish command generation before calling either.
    void SortVisibleCommands();
    bool Submit(render::GraphicsCommandEncoder& encoder, const MeshPassSubmitArgs& submitArgs = {}) noexcept;
    void Clear() noexcept;

    vector<VisibleMeshDrawCommand> VisibleCommands;
    DynamicMeshPassDrawListContext DynamicContext;
};

// See docs/architecture/runtime.md for the frame loop and ownership contract.
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

    // Scheduler-local frame number, assigned before Extract and preserved through Wait.
    // Starts at zero and advances after every completed Record.
    uint64_t GetFrameId() const noexcept;

    MeshPassFrameDrawList& GetPassDrawList(MeshPassId pass);

    // Allocate/register frame objects during Extract or Record; they survive through Wait.
    // Only the slot may Reset/Release the arena. Cleanup runs after recordings and CPU
    // records are cleared; callbacks must not re-enter the arena or scheduler.
    ObjectArena& GetArena() noexcept;

    // Call only on the current slot during Extract/Record, after stopping all new uses.
    // Retains the owner until the next slot reuse or WaitIdle, after every slot's old
    // command storage has reset. Group dependent owners in one RAII object.
    // Cleanup must not re-enter the slot or scheduler.
    // Debug checks the callback window; Release relies on this precondition.
    template <typename T>
    void RetireResource(T&& resourceOwner) {
#if RADRAY_IS_DEBUG
        if (!_inFrameCallback) RADRAY_ABORT("FrameSlot: RetireResource requires the current Extract or Record callback");
#endif
        _retiredArena.Emplace<std::remove_cvref_t<T>>(std::forward<T>(resourceOwner));
    }

    // Call during Record. Each call borrows a distinct buffer owned by this slot;
    // the next slot reset invalidates the borrowing and makes buffers reusable.
    // Caller owns Begin/End; shared allocator permits only one open recording at a time.
    Nullable<render::CommandBuffer*> AcquireCommandBuffer();

private:
    friend class FrameScheduler;

    static Nullable<unique_ptr<FrameSlot>> Create(DeviceContext& device, uint32_t index);
    FrameSlot(DeviceContext& device, uint32_t index, unique_ptr<render::CommandAllocator> allocator) noexcept;

    // Requires completed use. Every referencing allocator must reset before shared owners die.
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
