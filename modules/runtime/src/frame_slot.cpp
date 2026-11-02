#include <radray/runtime/frame_slot.h>

#include <algorithm>
#include <limits>

#include <radray/logger.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

MeshPassFrameDrawList::MeshPassFrameDrawList() noexcept : DynamicContext(VisibleCommands) {}

MeshPassFrameDrawList::MeshPassFrameDrawList(MeshPassFrameDrawList&& other) noexcept
    : VisibleCommands(std::move(other.VisibleCommands)), DynamicContext(std::move(other.DynamicContext)) {
    DynamicContext.BindVisibleCommands(VisibleCommands);
}

MeshPassFrameDrawList& MeshPassFrameDrawList::operator=(MeshPassFrameDrawList&& other) noexcept {
    if (this != &other) {
        VisibleCommands = std::move(other.VisibleCommands);
        DynamicContext = std::move(other.DynamicContext);
        DynamicContext.BindVisibleCommands(VisibleCommands);
    }
    return *this;
}

void MeshPassFrameDrawList::SortVisibleCommands() {
    std::stable_sort(VisibleCommands.begin(), VisibleCommands.end(), [](const VisibleMeshDrawCommand& left, const VisibleMeshDrawCommand& right) {
        return left.SortKey < right.SortKey;
    });
}

bool MeshPassFrameDrawList::Submit(render::GraphicsCommandEncoder& encoder, Nullable<const CachedPassMeshDrawList*> cachedDrawList,
                                   const MeshPassSubmitArgs& submitArgs) const noexcept {
    return SubmitMeshDrawCommands(encoder, VisibleCommands, cachedDrawList, DynamicContext.Commands, submitArgs);
}

void MeshPassFrameDrawList::Clear() noexcept {
    VisibleCommands.clear();
    DynamicContext.Clear();
}

Nullable<unique_ptr<FrameSlot>> FrameSlot::Create(DeviceContext& device, uint32_t index) {
    auto allocator = device.GetDevice().CreateCommandAllocator(&device.GetQueue());
    if (!allocator) {
        RADRAY_ERR_LOG("FrameSlot: cannot create command allocator");
        return nullptr;
    }
    return unique_ptr<FrameSlot>{new FrameSlot{device, index, allocator.Release()}};
}

FrameSlot::FrameSlot(DeviceContext& device, uint32_t index, unique_ptr<render::CommandAllocator> allocator) noexcept
    : _device(device), _allocator(std::move(allocator)), _index(index) {}

FrameSlot::~FrameSlot() noexcept {
    ResetCommandStorage();
    ResetRetiredResources();
    ResetFrameData();
}

render::Device& FrameSlot::GetDevice() noexcept { return _device.GetDevice(); }

render::CommandQueue& FrameSlot::GetQueue() noexcept { return _device.GetQueue(); }

uint32_t FrameSlot::GetIndex() const noexcept { return _index; }

uint64_t FrameSlot::GetFrameId() const noexcept { return _frameId; }

Nullable<const Scene*> FrameSlot::GetScene() const noexcept { return _sourceScene; }

MeshPassFrameDrawList& FrameSlot::GetPassDrawList(MeshPassId pass) {
    RADRAY_ASSERT(pass.Value != std::numeric_limits<uint32_t>::max());
    if (pass.Value >= _passDrawLists.size()) _passDrawLists.resize(size_t{pass.Value} + 1);
    return _passDrawLists[pass.Value];
}

ObjectArena& FrameSlot::GetArena() noexcept { return _arena; }

Nullable<render::CommandBuffer*> FrameSlot::AcquireCommandBuffer() {
    if (_nextCommandIndex == _commands.size()) {
        auto command = _device.GetDevice().CreateCommandBuffer(_allocator.get());
        if (!command) {
            RADRAY_ERR_LOG("FrameSlot: cannot create command buffer");
            return nullptr;
        }
        _commands.push_back(command.Release());
    }
    return _commands[_nextCommandIndex++].get();
}

void FrameSlot::ResetCommandStorage() noexcept {
    _allocator->Reset();
    _nextCommandIndex = 0;
}

void FrameSlot::ResetRetiredResources() noexcept { _retiredArena.Reset(); }

void FrameSlot::ResetFrameData() noexcept {
    _sourceScene = nullptr;
    for (auto& drawList : _passDrawLists) drawList.Clear();
    _arena.Reset();
}

}  // namespace radray
