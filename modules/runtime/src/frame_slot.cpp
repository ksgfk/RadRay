#include <radray/runtime/frame_slot.h>

#include <limits>
#include <utility>

#include <radray/logger.h>
#include <radray/profiler.h>
#include <radray/runtime/gpu_device.h>

namespace radray {

MeshPassFrameDrawList::MeshPassFrameDrawList(MeshPassSortMode mode) noexcept : DynamicContext(VisibleCommands, mode) {}

void MeshPassFrameDrawList::SetSortMode(MeshPassSortMode mode) noexcept {
    RADRAY_ASSERT(VisibleCommands.empty() || GetSortMode() == mode);
    DynamicContext.SetSortMode(mode);
}

MeshPassFrameDrawList::MeshPassFrameDrawList(MeshPassFrameDrawList&& other) noexcept
    : VisibleCommands(std::move(other.VisibleCommands)),
      DynamicContext(std::move(other.DynamicContext)),
      IndirectDraws(std::move(other.IndirectDraws)),
      _scratchCommands(std::move(other._scratchCommands)) {
    DynamicContext.BindVisibleCommands(VisibleCommands);
}

MeshPassFrameDrawList& MeshPassFrameDrawList::operator=(MeshPassFrameDrawList&& other) noexcept {
    if (this != &other) {
        VisibleCommands = std::move(other.VisibleCommands);
        DynamicContext = std::move(other.DynamicContext);
        IndirectDraws = std::move(other.IndirectDraws);
        _scratchCommands = std::move(other._scratchCommands);
        DynamicContext.BindVisibleCommands(VisibleCommands);
    }
    return *this;
}

void MeshPassFrameDrawList::SortVisibleCommands() {
    DynamicContext.FinalizeVisibleCommands();
    RADRAY_PROFILE_SCOPE_N("RadRay.DrawList.SortVisible");
    RADRAY_PROFILE_PLOT("RadRay.DrawList.SortCommands", static_cast<int64_t>(VisibleCommands.size()));
    if (VisibleCommands.size() <= 64) {
        for (size_t index = 1; index < VisibleCommands.size(); ++index) {
            const auto value = VisibleCommands[index];
            auto position = index;
            while (position != 0 && VisibleCommands[position - 1].SortKey > value.SortKey) {
                VisibleCommands[position] = VisibleCommands[position - 1];
                --position;
            }
            VisibleCommands[position] = value;
        }
        return;
    }
    uint64_t differing = 0;
    bool sorted = true;
    for (size_t i = 1; i < VisibleCommands.size(); ++i) {
        differing |= VisibleCommands[i].SortKey ^ VisibleCommands[0].SortKey;
        sorted &= VisibleCommands[i - 1].SortKey <= VisibleCommands[i].SortKey;
    }
    if (sorted) return;
    uint32_t activePasses = 0;
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        if (((differing >> shift) & 255) != 0) ++activePasses;
    }
    if ((activePasses & 1u) != 0 && _scratchCommands.capacity() < VisibleCommands.capacity()) {
        _scratchCommands.reserve(VisibleCommands.capacity());
    }
    _scratchCommands.resize(VisibleCommands.size());
    auto source = VisibleCommands.data();
    auto destination = _scratchCommands.data();
    for (uint32_t shift = 0; shift < 64; shift += 8) {
        if (((differing >> shift) & 255) == 0) continue;
        array<size_t, 256> offsets{};
        for (size_t i = 0; i < VisibleCommands.size(); ++i) ++offsets[(source[i].SortKey >> shift) & 255];
        size_t sum = 0;
        for (auto& offset : offsets) {
            const auto count = offset;
            offset = sum;
            sum += count;
        }
        for (size_t i = 0; i < VisibleCommands.size(); ++i) destination[offsets[(source[i].SortKey >> shift) & 255]++] = source[i];
        std::swap(source, destination);
    }
    if (source != VisibleCommands.data()) VisibleCommands.swap(_scratchCommands);
}

bool MeshPassFrameDrawList::Submit(gpu::GraphicsCommandEncoder& encoder, const MeshPassSubmitArgs& submitArgs) const noexcept {
    return SubmitMeshDrawCommands(encoder, VisibleCommands, submitArgs);
}

void MeshPassFrameDrawList::Clear() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.DrawList.Clear");
    VisibleCommands.clear();
    IndirectDraws.Clear();
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.DrawList.ClearDynamic");
        DynamicContext.Clear();
    }
}

Nullable<unique_ptr<FrameSlot>> FrameSlot::Create(DeviceContext& device, uint32_t index, Nullable<std::pmr::memory_resource*> memoryUpstream) {
    auto allocator = device.GetDevice().CreateCommandAllocator(&device.GetQueue());
    if (!allocator) {
        RADRAY_ERR_LOG("FrameSlot: cannot create command allocator");
        return nullptr;
    }
    return unique_ptr<FrameSlot>{new FrameSlot{device, index, allocator.Release(), memoryUpstream}};
}

FrameSlot::FrameSlot(DeviceContext& device, uint32_t index, unique_ptr<gpu::CommandAllocator> allocator, Nullable<std::pmr::memory_resource*> memoryUpstream)
    : _device(device), _allocator(std::move(allocator)), _parameterSets(device.GetDevice()),
      _memoryUpstream(memoryUpstream ? *memoryUpstream : _memoryHeap),
      _scratchPool(std::pmr::pool_options{.max_blocks_per_chunk = 4096, .largest_required_pool_block = 64 * 1024}, &_memoryUpstream), _index(index) {}

FrameSlot::~FrameSlot() noexcept {
    ResetCommandStorage();
    ResetFrameData();
    _memory.reset();
    if (_initialBuffer) _memoryUpstream.deallocate(_initialBuffer.Get(), _initialCapacity, alignof(std::max_align_t));
}

gpu::Device& FrameSlot::GetDevice() noexcept { return _device.GetDevice(); }

gpu::CommandQueue& FrameSlot::GetQueue() noexcept { return _device.GetQueue(); }

uint32_t FrameSlot::GetIndex() const noexcept { return _index; }

uint64_t FrameSlot::GetFrameId() const noexcept { return _frameId; }

MeshPassFrameDrawList& FrameSlot::GetPassDrawList(MeshPassId pass) {
    RADRAY_ASSERT(pass.Value != std::numeric_limits<uint32_t>::max());
    if (pass.Value >= _passDrawLists.size()) _passDrawLists.resize(size_t{pass.Value} + 1);
    return _passDrawLists[pass.Value];
}

const MeshPassFrameDrawList& FrameSlot::GetPassDrawList(MeshPassId pass) const noexcept {
    if (pass.Value >= _passDrawLists.size()) RADRAY_ABORT("FrameSlot: no prepared draw list");
    return _passDrawLists[pass.Value];
}

std::pmr::memory_resource& FrameSlot::GetMemoryResource() {
    if (!_memory) {
        auto* buffer = _memoryUpstream.allocate(_initialCapacity, alignof(std::max_align_t));
        auto cleanup = MakeScopeGuard([&]() noexcept { _memoryUpstream.deallocate(buffer, _initialCapacity, alignof(std::max_align_t)); });
        _memory.emplace(buffer, _initialCapacity, &_memoryUpstream);
        _initialBuffer = buffer;
        cleanup.Dismiss();
    }
    return *_memory;
}
std::pmr::memory_resource& FrameSlot::GetScratchMemoryResource() noexcept { return _scratchPool; }

Nullable<gpu::CommandBuffer*> FrameSlot::AcquireCommandBuffer() {
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

Nullable<gpu::ShaderParameterSet*> FrameSlot::AcquireParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout, std::span<const TransientParameterBinding> bindings) {
    return _parameterSets.AcquireParameterSet(layout, bindings);
}

Nullable<gpu::ShaderParameterSet*> FrameSlot::AcquireUninitializedParameterSet(const shared_ptr<gpu::BindingGroupLayout>& layout) {
    return _parameterSets.AcquireUninitializedParameterSet(layout);
}

void FrameSlot::ResetCommandStorage() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.ResetCommandStorage");
    _allocator->Reset();
    _parameterSets.BeginFrame();
    _nextCommandIndex = 0;
}

void FrameSlot::ResetFrameData() noexcept {
    RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.ResetFrameData");
#if RADRAY_IS_DEBUG
    RADRAY_ASSERT(_constructionDepth == 0);
    _inFrameCallback = false;
#endif
    for (auto& drawList : _passDrawLists) drawList.Clear();
    {
        RADRAY_PROFILE_SCOPE_N("RadRay.FrameSlot.DestroyObjects");
        while (_objects) {
            auto* object = _objects.Get();
            _objects = object->Previous;
            object->Destroy(object);
        }
    }
    if (_memory) _memory.emplace(_initialBuffer.Get(), _initialCapacity, &_memoryUpstream);
}

MeshPassSortMode MeshPassFrameDrawList::GetSortMode() const noexcept {
    return DynamicContext.GetSortMode();
}

}  // namespace radray
