#include <radray/runtime/gpu_resource.h>

#include <algorithm>
#include <utility>

#include <radray/logger.h>

namespace radray {

namespace {

render::ResourceHints GetPersistentUploadHints(render::Device* device) noexcept {
    render::ResourceHints hints{render::ResourceHint::PersistentMap};
    if (device != nullptr && device->GetBackend() == render::RenderBackend::Vulkan) {
        // VMA maps an entire backing block. A dedicated allocation keeps the
        // coherent mapped range proportional to the arena instead of the heap block.
        hints |= render::ResourceHint::Dedicated;
    }
    return hints;
}

}  // namespace

DynamicCBufferArena::Block::Block(unique_ptr<render::Buffer> buffer) noexcept
    : Buffer(std::move(buffer)) {
    Mapped = Buffer->Map(0, Buffer->GetDesc().Size);
    RADRAY_ASSERT(Mapped != nullptr);
}

DynamicCBufferArena::Block::~Block() noexcept {
    if (Buffer != nullptr && Mapped != nullptr) {
        Buffer->Unmap(0, Buffer->GetDesc().Size);
        Mapped = nullptr;
    }
}

DynamicCBufferArena::DynamicCBufferArena(render::Device* device, const Descriptor& desc) noexcept
    : _device(device), _desc(desc) {
    if (_desc.Alignment == 0 || (_desc.Alignment & (_desc.Alignment - 1)) != 0) {
        RADRAY_ABORT(
            "DynamicCBufferArena invalid Alignment: {} (must be power-of-two and non-zero)",
            _desc.Alignment);
    }
}

DynamicCBufferArena::DynamicCBufferArena(render::Device* device) noexcept
    : DynamicCBufferArena(device, Descriptor{}) {}

DynamicCBufferArena::DynamicCBufferArena(DynamicCBufferArena&& other) noexcept
    : _device(other._device),
      _blocks(std::move(other._blocks)),
      _desc(std::move(other._desc)),
      _minBlockSize(other._minBlockSize),
      _allocatedThisFrame(other._allocatedThisFrame),
      _highWatermark(other._highWatermark) {
    other._device = nullptr;
    other._minBlockSize = 0;
    other._allocatedThisFrame = 0;
    other._highWatermark = 0;
}

DynamicCBufferArena& DynamicCBufferArena::operator=(DynamicCBufferArena&& other) noexcept {
    DynamicCBufferArena tmp{std::move(other)};
    swap(*this, tmp);
    return *this;
}

DynamicCBufferArena::~DynamicCBufferArena() noexcept {
    Destroy();
}

bool DynamicCBufferArena::IsValid() const noexcept {
    return _device != nullptr;
}

void DynamicCBufferArena::Destroy() noexcept {
    _blocks.clear();
    _device = nullptr;
    _allocatedThisFrame = 0;
}

DynamicCBufferArena::Allocation DynamicCBufferArena::Allocate(uint64_t size) noexcept {
    if (size == 0) {
        return Allocation::Invalid();
    }
    auto blockOpt = GetOrCreateBlock(size);
    if (!blockOpt.HasValue()) {
        RADRAY_ABORT("allocation failed: cannot create dynamic cbuffer block");
    }
    Block* block = blockOpt.Release();
    const uint64_t offset = Align(block->Used, _desc.Alignment);
    block->Used = offset + size;
    _allocatedThisFrame += Align(size, _desc.Alignment);
    _highWatermark = std::max(_highWatermark, _allocatedThisFrame);
    return Allocation{
        .Target = block->Buffer.get(),
        .Mapped = static_cast<byte*>(block->Mapped) + offset,
        .Offset = offset,
        .Size = size};
}

Nullable<DynamicCBufferArena::Block*> DynamicCBufferArena::GetOrCreateBlock(uint64_t size) noexcept {
    if (!_blocks.empty()) {
        Block* last = _blocks.back().get();
        const auto desc = last->Buffer->GetDesc();
        const uint64_t offset = Align(last->Used, _desc.Alignment);
        if (offset <= desc.Size && size <= desc.Size - offset) {
            return last;
        }
    }

    const string name = fmt::format("{}_{}", _desc.NamePrefix, _blocks.size());
    render::BufferDescriptor desc{};
    const uint64_t previousSize = _blocks.empty() ? 0 : _blocks.back()->Buffer->GetDesc().Size;
    const uint64_t growthSize = previousSize == 0
                                    ? _desc.BasicSize
                                    : previousSize > std::numeric_limits<uint64_t>::max() / 2
                                          ? std::numeric_limits<uint64_t>::max()
                                          : previousSize * 2;
    desc.Size = Align(
        std::max({size, _minBlockSize, growthSize}),
        _desc.Alignment);
    desc.Memory = render::MemoryType::Upload;
    desc.Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite | render::BufferUse::CopySource;
    desc.Hints = GetPersistentUploadHints(_device);
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        return nullptr;
    }
    auto buffer = bufferOpt.Release();
    buffer->SetDebugName(name);
    return _blocks.emplace_back(make_unique<Block>(std::move(buffer))).get();
}

void DynamicCBufferArena::Reset() noexcept {
    _allocatedThisFrame = 0;
    if (_blocks.empty()) {
        return;
    }
    if (_blocks.size() == 1) {
        if (_blocks.front()->Buffer->GetDesc().Size > _desc.MaxResetSize) {
            _minBlockSize = _desc.MaxResetSize;
            _blocks.clear();
        } else {
            _blocks.front()->Used = 0;
        }
        return;
    }

    uint64_t totalCapacity = 0;
    for (const auto& block : _blocks) {
        totalCapacity += block->Buffer->GetDesc().Size;
    }
    _minBlockSize = std::max<uint64_t>(_desc.BasicSize, 1);
    while (_minBlockSize < totalCapacity && _minBlockSize < _desc.MaxResetSize) {
        const uint64_t doubled = _minBlockSize > std::numeric_limits<uint64_t>::max() / 2
                                     ? std::numeric_limits<uint64_t>::max()
                                     : _minBlockSize * 2;
        _minBlockSize = std::min(doubled, _desc.MaxResetSize);
    }
    _blocks.clear();
}

void DynamicCBufferArena::Clear() noexcept {
    _blocks.clear();
    _minBlockSize = 0;
    _allocatedThisFrame = 0;
}

bool DynamicCBufferArena::Contains(const render::Buffer* buffer) const noexcept {
    return std::ranges::any_of(_blocks, [buffer](const auto& block) noexcept {
        return block->Buffer.get() == buffer;
    });
}

void swap(DynamicCBufferArena& a, DynamicCBufferArena& b) noexcept {
    using std::swap;
    swap(a._device, b._device);
    swap(a._blocks, b._blocks);
    swap(a._desc, b._desc);
    swap(a._minBlockSize, b._minBlockSize);
    swap(a._allocatedThisFrame, b._allocatedThisFrame);
    swap(a._highWatermark, b._highWatermark);
}

MaterialConstantPool::Block::~Block() noexcept {
    if (Buffer != nullptr && Mapped != nullptr) {
        Buffer->Unmap(0, Buffer->GetDesc().Size);
    }
}

MaterialConstantPool::MaterialConstantPool(
    render::Device* device,
    uint64_t initialSize,
    uint64_t alignment) noexcept
    : _device(device),
      _initialSize(initialSize),
      _alignment(std::max<uint64_t>(alignment, 1)) {
    if ((_alignment & (_alignment - 1)) != 0) {
        RADRAY_ABORT("MaterialConstantPool alignment {} is not a power of two", _alignment);
    }
}

MaterialConstantPool::~MaterialConstantPool() noexcept = default;

Nullable<MaterialConstantPool::Block*> MaterialConstantPool::CreateBlock(uint64_t minimumSize) noexcept {
    const uint64_t previousSize = _blocks.empty() ? 0 : _blocks.back()->Buffer->GetDesc().Size;
    const uint64_t capacity = Align(
        std::max(minimumSize, previousSize == 0 ? _initialSize : previousSize * 2),
        _alignment);
    render::BufferDescriptor desc{
        .Size = capacity,
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite | render::BufferUse::CopySource,
        .Hints = GetPersistentUploadHints(_device)};
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        return nullptr;
    }
    auto block = make_unique<Block>();
    block->Buffer = bufferOpt.Release();
    block->Buffer->SetDebugName(fmt::format("material_constants_{}", _blocks.size()));
    block->Mapped = block->Buffer->Map(0, capacity);
    if (block->Mapped == nullptr) {
        return nullptr;
    }
    return _blocks.emplace_back(std::move(block)).get();
}

MaterialConstantPool::Allocation MaterialConstantPool::Allocate(uint64_t size) noexcept {
    if (_device == nullptr || size == 0) {
        return {};
    }
    const uint64_t reserved = Align(size, _alignment);
    for (size_t i = 0; i < _freeSlices.size(); ++i) {
        FreeSlice& free = _freeSlices[i];
        if (free.Size < reserved || free.BlockIndex >= _blocks.size()) {
            continue;
        }
        Block& block = *_blocks[free.BlockIndex];
        const Allocation result{
            .Target = block.Buffer.get(),
            .Mapped = static_cast<byte*>(block.Mapped) + free.Offset,
            .Offset = free.Offset,
            .Size = size,
            .ReservedSize = reserved,
            .BlockIndex = free.BlockIndex};
        free.Offset += reserved;
        free.Size -= reserved;
        if (free.Size == 0) {
            _freeSlices.erase(_freeSlices.begin() + static_cast<ptrdiff_t>(i));
        }
        _activeBytes += reserved;
        _highWatermark = std::max(_highWatermark, _activeBytes);
        return result;
    }

    Block* block = _blocks.empty() ? nullptr : _blocks.back().get();
    if (block == nullptr || block->Used > block->Buffer->GetDesc().Size ||
        reserved > block->Buffer->GetDesc().Size - block->Used) {
        auto blockOpt = CreateBlock(reserved);
        if (!blockOpt.HasValue()) {
            return {};
        }
        block = blockOpt.Get();
    }
    const uint32_t blockIndex = static_cast<uint32_t>(_blocks.size() - 1);
    const uint64_t offset = block->Used;
    block->Used += reserved;
    _activeBytes += reserved;
    _highWatermark = std::max(_highWatermark, _activeBytes);
    return Allocation{
        .Target = block->Buffer.get(),
        .Mapped = static_cast<byte*>(block->Mapped) + offset,
        .Offset = offset,
        .Size = size,
        .ReservedSize = reserved,
        .BlockIndex = blockIndex};
}

void MaterialConstantPool::Release(const Allocation& allocation) noexcept {
    if (!allocation.IsValid() || allocation.BlockIndex >= _blocks.size() || allocation.ReservedSize == 0) {
        return;
    }
    _freeSlices.push_back(FreeSlice{
        .BlockIndex = allocation.BlockIndex,
        .Offset = allocation.Offset,
        .Size = allocation.ReservedSize});
    _activeBytes = allocation.ReservedSize <= _activeBytes
                       ? _activeBytes - allocation.ReservedSize
                       : 0;
}

FrameResources::FrameResources(render::Device* device) noexcept
    : PerObjectArena(
          device,
          DynamicCBufferArena::Descriptor{
              .BasicSize = 256 * 1024,
              .Alignment = std::max<uint64_t>(256, device->GetDetail().CBufferAlignment),
              .MaxResetSize = 4 * 1024 * 1024,
              .NamePrefix = "per_object"}),
      ViewArena(
          device,
          DynamicCBufferArena::Descriptor{
              .BasicSize = 256 * 1024,
              .Alignment = std::max<uint64_t>(256, device->GetDetail().CBufferAlignment),
              .MaxResetSize = 4 * 1024 * 1024,
              .NamePrefix = "per_view"}),
      SystemDescriptorPool(device->CreateDescriptorPool(render::DescriptorPoolDescriptor{
          .MaxBindingGroups = 64,
          .MaxSampledTextures = 128,
          .MaxStorageTextures = 32,
          .MaxUniformBuffers = 32,
          .MaxDynamicUniformBuffers = 64,
          .MaxStorageBuffers = 64,
          .MaxReadOnlyTexelBuffers = 16,
          .MaxReadWriteTexelBuffers = 16,
          .MaxSamplers = 64,
          .MaxAccelerationStructures = 16,
          .Lifetime = render::DescriptorPoolLifetime::PerFlight}).Unwrap()),
      TransientDescriptorPool(device->CreateDescriptorPool(render::DescriptorPoolDescriptor{
          .MaxBindingGroups = 128,
          .MaxSampledTextures = 256,
          .MaxStorageTextures = 64,
          .MaxUniformBuffers = 64,
          .MaxDynamicUniformBuffers = 128,
          .MaxStorageBuffers = 128,
          .MaxReadOnlyTexelBuffers = 32,
          .MaxReadWriteTexelBuffers = 32,
          .MaxSamplers = 128,
          .MaxAccelerationStructures = 32,
          .Lifetime = render::DescriptorPoolLifetime::PerFlight}).Unwrap()) {
    SystemDescriptorPool->SetDebugName("frame_system_descriptors");
    TransientDescriptorPool->SetDebugName("frame_transient_descriptors");
}

void FrameResources::Reset() noexcept {
    RetireList.clear();
    RetainedObjects.clear();
    PerObjectArena.Reset();
    ViewArena.Reset();
    ObjectBindings.clear();
    std::erase_if(SystemGroups, [this](const FrameBindingGroupCacheEntry& entry) noexcept {
        return entry.DynamicBuffer != nullptr &&
               !PerObjectArena.Contains(entry.DynamicBuffer) &&
               !ViewArena.Contains(entry.DynamicBuffer);
    });
    if (TransientDescriptorPool != nullptr) {
        TransientDescriptorPool->Reset();
    }
    Counters = {};
    Counters.ObjectArenaHighWatermark = PerObjectArena.GetHighWatermark();
    Counters.ViewArenaHighWatermark = ViewArena.GetHighWatermark();
    ++Generation;
}

}  // namespace radray
