#include <radray/runtime/frame_slot.h>

#include <algorithm>
#include <limits>

#include <radray/logger.h>
#include <radray/profiler.h>
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
    DynamicContext.FinalizeVisibleCommands();
    RADRAY_PROFILE_SCOPE_N("RadRay.DrawList.SortVisible");
    RADRAY_PROFILE_PLOT("RadRay.DrawList.SortCommands", static_cast<int64_t>(VisibleCommands.size()));
    std::stable_sort(VisibleCommands.begin(), VisibleCommands.end(), [](const VisibleMeshDrawCommand& left, const VisibleMeshDrawCommand& right) {
        return left.SortKey < right.SortKey;
    });
}

bool MeshPassFrameDrawList::Submit(render::GraphicsCommandEncoder& encoder, const MeshPassSubmitArgs& submitArgs) noexcept {
    DynamicContext.FinalizeVisibleCommands();
    return SubmitMeshDrawCommands(encoder, VisibleCommands, submitArgs);
}

void MeshPassFrameDrawList::Clear() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.DrawList.Clear");
    VisibleCommands.clear();
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.DrawList.ClearDynamic");
        DynamicContext.Clear();
    }
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

MeshPassFrameDrawList& FrameSlot::GetPassDrawList(MeshPassId pass) {
    RADRAY_ASSERT(pass.Value != std::numeric_limits<uint32_t>::max());
    if (pass.Value >= _passDrawLists.size()) _passDrawLists.resize(size_t{pass.Value} + 1);
    return _passDrawLists[pass.Value];
}

ObjectArena& FrameSlot::GetArena() noexcept { return _arena; }

Nullable<render::CommandBuffer*> FrameSlot::AcquireCommandBuffer() {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.AcquireCommandBuffer");
    if (_nextCommandIndex == _commands.size()) {
        RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.CreateCommandBuffer");
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
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.ResetCommandStorage");
    _allocator->Reset();
    _nextCommandIndex = 0;
}

void FrameSlot::ResetRetiredResources() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.ResetRetiredResources");
    _retiredArena.Reset();
}

void FrameSlot::ResetFrameData() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.ResetFrameData");
#if RADRAY_IS_DEBUG
    _inFrameCallback = false;
#endif
    for (auto& drawList : _passDrawLists) drawList.Clear();
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.ResetArena");
        _arena.Reset();
    }
}

}  // namespace radray
