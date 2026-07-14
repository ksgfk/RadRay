#include <radray/runtime/gpu_resource.h>

#include <algorithm>
#include <utility>

#include <radray/logger.h>
#include <radray/runtime/gpu_system.h>

namespace radray {

namespace {

render::ResourceHints GetPersistentUploadHints() noexcept {
    return render::ResourceHint::PersistentMap;
}

bool IsPowerOfTwo(uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

MappedUploadPage::Reservation::Reservation(
    render::Buffer* target,
    void* data,
    uint64_t offset,
    uint64_t capacity,
    HostWriteBatch* hostWrites) noexcept
    : _target(target),
      _data(data),
      _offset(offset),
      _capacity(capacity),
      _hostWrites(hostWrites),
      _committed(false) {}

MappedUploadPage::Reservation::Reservation(Reservation&& other) noexcept
    : _target(other._target),
      _data(other._data),
      _offset(other._offset),
      _capacity(other._capacity),
      _hostWrites(other._hostWrites),
      _committed(other._committed) {
    other._target = nullptr;
    other._data = nullptr;
    other._hostWrites = nullptr;
    other._committed = true;
}

MappedUploadPage::Reservation& MappedUploadPage::Reservation::operator=(Reservation&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    AbandonCheck();
    _target = other._target;
    _data = other._data;
    _offset = other._offset;
    _capacity = other._capacity;
    _hostWrites = other._hostWrites;
    _committed = other._committed;
    other._target = nullptr;
    other._data = nullptr;
    other._hostWrites = nullptr;
    other._committed = true;
    return *this;
}

MappedUploadPage::Reservation::~Reservation() noexcept {
    AbandonCheck();
}

void MappedUploadPage::Reservation::AbandonCheck() const noexcept {
#ifdef RADRAY_IS_DEBUG
    if (_target != nullptr && !_committed) {
        RADRAY_ABORT("mapped upload reservation was destroyed without Commit");
    }
#endif
}

MappedUploadPage::Allocation MappedUploadPage::Reservation::Commit(uint64_t actualSize) {
    if (_target == nullptr) {
        if (actualSize == 0) {
            return Allocation::Invalid();
        }
        RADRAY_ABORT("cannot commit an invalid mapped upload reservation");
    }
    if (_committed) {
        RADRAY_ABORT("mapped upload reservation was committed more than once");
    }
    if (actualSize > _capacity) {
        RADRAY_ABORT(
            "mapped upload commit size {} exceeds reservation capacity {}",
            actualSize,
            _capacity);
    }
    _committed = true;
    if (_hostWrites == nullptr) {
        RADRAY_ABORT("mapped upload reservation has no host-write batch");
    }
    _hostWrites->Record(
        _target,
        render::BufferRange{.Offset = _offset, .Size = actualSize});
    return Allocation{
        .Target = _target,
        .Offset = _offset,
        .Size = actualSize};
}

MappedUploadPage::MappedUploadPage(
    unique_ptr<render::Buffer> buffer,
    Nullable<HostWriteBatch*> allocationStats) noexcept
    : _buffer(std::move(buffer)) {
    if (_buffer == nullptr) {
        RADRAY_ABORT("MappedUploadPage requires a non-null buffer");
    }
    const render::BufferDescriptor desc = _buffer->GetDesc();
    if (!desc.Usage.HasFlag(render::BufferUse::MapWrite) ||
        !desc.Hints.HasFlag(render::ResourceHint::PersistentMap)) {
        RADRAY_ABORT("MappedUploadPage requires a MapWrite + PersistentMap buffer");
    }
    _mapped = _buffer->Map(0, desc.Size);
    if (_mapped == nullptr) {
        RADRAY_ABORT("failed to persistently map an upload page of size {}", desc.Size);
    }
    if (allocationStats.HasValue()) {
        allocationStats.Get()->RecordPageAllocation(desc.Size);
    }
}

MappedUploadPage::~MappedUploadPage() noexcept {
    if (_buffer != nullptr && _mapped != nullptr) {
        _buffer->Unmap();
        _mapped = nullptr;
    }
}

MappedUploadPage::Reservation MappedUploadPage::Reserve(
    uint64_t size,
    uint64_t alignment,
    HostWriteBatch& hostWrites) {
    if (size == 0) {
        return {};
    }
    if (!IsPowerOfTwo(alignment)) {
        RADRAY_ABORT("mapped upload alignment {} must be a non-zero power of two", alignment);
    }
    const uint64_t capacity = GetCapacity();
    if (_used > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return {};
    }
    const uint64_t offset = Align(_used, alignment);
    if (offset > capacity || size > capacity - offset) {
        return {};
    }
    _used = offset + size;
    return Reservation{
        _buffer.get(),
        static_cast<byte*>(_mapped) + offset,
        offset,
        size,
        &hostWrites};
}

MappedUploadPage::Reservation MappedUploadPage::ReserveAt(
    uint64_t offset,
    uint64_t size,
    HostWriteBatch& hostWrites) {
    const uint64_t capacity = GetCapacity();
    if (size == 0 || offset > capacity || size > capacity - offset) {
        return {};
    }
    return Reservation{
        _buffer.get(),
        static_cast<byte*>(_mapped) + offset,
        offset,
        size,
        &hostWrites};
}

DynamicCBufferArena::Block::Block(unique_ptr<MappedUploadPage> page) noexcept
    : Page(std::move(page)) {}

DynamicCBufferArena::DynamicCBufferArena(
    render::Device* device,
    HostWriteBatch* hostWrites,
    const Descriptor& desc) noexcept
    : _device(device), _hostWrites(hostWrites), _desc(desc) {
    if (!IsPowerOfTwo(_desc.Alignment)) {
        RADRAY_ABORT(
            "DynamicCBufferArena invalid Alignment: {} (must be power-of-two and non-zero)",
            _desc.Alignment);
    }
    if (_hostWrites == nullptr) {
        RADRAY_ABORT("DynamicCBufferArena requires a host-write batch");
    }
}

DynamicCBufferArena::DynamicCBufferArena(
    render::Device* device,
    HostWriteBatch* hostWrites) noexcept
    : DynamicCBufferArena(device, hostWrites, Descriptor{}) {}

DynamicCBufferArena::DynamicCBufferArena(DynamicCBufferArena&& other) noexcept
    : _device(other._device),
      _hostWrites(other._hostWrites),
      _blocks(std::move(other._blocks)),
      _desc(std::move(other._desc)),
      _activeBlockIndex(other._activeBlockIndex),
      _minBlockSize(other._minBlockSize),
      _allocatedThisFrame(other._allocatedThisFrame),
      _highWatermark(other._highWatermark) {
    other._device = nullptr;
    other._hostWrites = nullptr;
    other._activeBlockIndex = 0;
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
    return _device != nullptr && _hostWrites != nullptr;
}

void DynamicCBufferArena::Destroy() noexcept {
    _blocks.clear();
    _device = nullptr;
    _hostWrites = nullptr;
    _activeBlockIndex = 0;
    _allocatedThisFrame = 0;
}

DynamicCBufferArena::Reservation DynamicCBufferArena::Reserve(uint64_t size) noexcept {
    if (size == 0) {
        return {};
    }
    auto blockOpt = GetOrCreateBlock(size);
    if (!blockOpt.HasValue()) {
        RADRAY_ABORT("allocation failed: cannot create dynamic cbuffer block");
    }
    Block* block = blockOpt.Release();
    Reservation reservation = block->Page->Reserve(size, _desc.Alignment, *_hostWrites);
    if (!reservation.IsValid()) {
        RADRAY_ABORT("allocation failed: dynamic cbuffer page reservation failed");
    }
    _allocatedThisFrame += Align(size, _desc.Alignment);
    _highWatermark = std::max(_highWatermark, _allocatedThisFrame);
    return reservation;
}

Nullable<DynamicCBufferArena::Block*> DynamicCBufferArena::GetOrCreateBlock(uint64_t size) noexcept {
    while (_activeBlockIndex < _blocks.size()) {
        Block* block = _blocks[_activeBlockIndex].get();
        const uint64_t offset = Align(block->Page->GetUsed(), _desc.Alignment);
        if (offset <= block->Page->GetCapacity() && size <= block->Page->GetCapacity() - offset) {
            return block;
        }
        ++_activeBlockIndex;
    }

    const string name = fmt::format("{}_{}", _desc.NamePrefix, _blocks.size());
    const uint64_t previousSize = _blocks.empty() ? 0 : _blocks.back()->Page->GetCapacity();
    const uint64_t growthSize = previousSize == 0
                                    ? _desc.BasicSize
                                    : previousSize > std::numeric_limits<uint64_t>::max() / 2
                                          ? std::numeric_limits<uint64_t>::max()
                                          : previousSize * 2;
    render::BufferDescriptor desc{
        .Size = Align(std::max({size, _minBlockSize, growthSize}), _desc.Alignment),
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite | render::BufferUse::CopySource,
        .Hints = GetPersistentUploadHints()};
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        return nullptr;
    }
    auto buffer = bufferOpt.Release();
    buffer->SetDebugName(name);
    auto page = make_unique<MappedUploadPage>(std::move(buffer), _hostWrites);
    _activeBlockIndex = _blocks.size();
    return _blocks.emplace_back(make_unique<Block>(std::move(page))).get();
}

void DynamicCBufferArena::Reset() noexcept {
    _allocatedThisFrame = 0;
    _activeBlockIndex = 0;
    if (_blocks.empty()) {
        return;
    }
    if (_blocks.size() == 1) {
        _blocks.front()->Page->Reset();
        return;
    }

    uint64_t totalCapacity = 0;
    for (const auto& block : _blocks) {
        totalCapacity += block->Page->GetCapacity();
    }
    if (totalCapacity > _desc.MaxResetSize) {
        for (const auto& block : _blocks) {
            block->Page->Reset();
        }
        return;
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
    _activeBlockIndex = 0;
    _minBlockSize = 0;
    _allocatedThisFrame = 0;
}

bool DynamicCBufferArena::Contains(const render::Buffer* buffer) const noexcept {
    return std::ranges::any_of(_blocks, [buffer](const auto& block) noexcept {
        return block->Page->GetBuffer() == buffer;
    });
}

void swap(DynamicCBufferArena& a, DynamicCBufferArena& b) noexcept {
    using std::swap;
    swap(a._device, b._device);
    swap(a._hostWrites, b._hostWrites);
    swap(a._blocks, b._blocks);
    swap(a._desc, b._desc);
    swap(a._activeBlockIndex, b._activeBlockIndex);
    swap(a._minBlockSize, b._minBlockSize);
    swap(a._allocatedThisFrame, b._allocatedThisFrame);
    swap(a._highWatermark, b._highWatermark);
}

MaterialConstantPool::Allocation MaterialConstantPool::Reservation::Commit(uint64_t actualSize) {
    const MappedUploadPage::Allocation allocation = _reservation.Commit(actualSize);
    if (!allocation.IsValid()) {
        return {};
    }
    return Allocation{
        .Target = allocation.Target,
        .Offset = allocation.Offset,
        .Size = allocation.Size,
        .ReservedSize = _reservedSize,
        .BlockIndex = _blockIndex};
}

MaterialConstantPool::MaterialConstantPool(
    render::Device* device,
    uint64_t initialSize,
    uint64_t alignment) noexcept
    : _device(device),
      _initialSize(initialSize),
      _alignment(std::max<uint64_t>(alignment, 1)) {
    if (!IsPowerOfTwo(_alignment)) {
        RADRAY_ABORT("MaterialConstantPool alignment {} is not a power of two", _alignment);
    }
}

MaterialConstantPool::~MaterialConstantPool() noexcept = default;

Nullable<MaterialConstantPool::Block*> MaterialConstantPool::CreateBlock(
    uint64_t minimumSize,
    HostWriteBatch& hostWrites) noexcept {
    const uint64_t previousSize = _blocks.empty() ? 0 : _blocks.back()->Page->GetCapacity();
    const uint64_t capacity = Align(
        std::max(minimumSize, previousSize == 0 ? _initialSize : previousSize * 2),
        _alignment);
    render::BufferDescriptor desc{
        .Size = capacity,
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::CBuffer | render::BufferUse::MapWrite | render::BufferUse::CopySource,
        .Hints = GetPersistentUploadHints()};
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        return nullptr;
    }
    auto buffer = bufferOpt.Release();
    buffer->SetDebugName(fmt::format("material_constants_{}", _blocks.size()));
    auto block = make_unique<Block>();
    block->Page = make_unique<MappedUploadPage>(std::move(buffer), &hostWrites);
    return _blocks.emplace_back(std::move(block)).get();
}

MaterialConstantPool::Reservation MaterialConstantPool::Reserve(
    uint64_t size,
    HostWriteBatch& hostWrites) noexcept {
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
        auto reservation = block.Page->ReserveAt(free.Offset, size, hostWrites);
        const uint32_t blockIndex = free.BlockIndex;
        free.Offset += reserved;
        free.Size -= reserved;
        if (free.Size == 0) {
            _freeSlices.erase(_freeSlices.begin() + static_cast<ptrdiff_t>(i));
        }
        _activeBytes += reserved;
        _highWatermark = std::max(_highWatermark, _activeBytes);
        return Reservation{std::move(reservation), reserved, blockIndex};
    }

    Block* block = _blocks.empty() ? nullptr : _blocks.back().get();
    if (block == nullptr) {
        auto blockOpt = CreateBlock(reserved, hostWrites);
        if (!blockOpt.HasValue()) {
            return {};
        }
        block = blockOpt.Get();
    }
    auto reservation = block->Page->Reserve(size, _alignment, hostWrites);
    if (!reservation.IsValid()) {
        auto blockOpt = CreateBlock(reserved, hostWrites);
        if (!blockOpt.HasValue()) {
            return {};
        }
        block = blockOpt.Get();
        reservation = block->Page->Reserve(size, _alignment, hostWrites);
    }
    const uint32_t blockIndex = static_cast<uint32_t>(_blocks.size() - 1);
    _activeBytes += reserved;
    _highWatermark = std::max(_highWatermark, _activeBytes);
    return Reservation{std::move(reservation), reserved, blockIndex};
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

FrameResources::FrameResources(
    render::Device* device,
    HostWriteBatch* hostWrites) noexcept
    : PerObjectArena(
          device,
          hostWrites,
          DynamicCBufferArena::Descriptor{
              .BasicSize = 256 * 1024,
              .Alignment = std::max<uint64_t>(256, device->GetDetail().CBufferAlignment),
              .MaxResetSize = 4 * 1024 * 1024,
              .NamePrefix = "per_object"}),
      ViewArena(
          device,
          hostWrites,
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
                                                            .Lifetime = render::DescriptorPoolLifetime::PerFlight})
                               .Unwrap()),
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
                                                               .Lifetime = render::DescriptorPoolLifetime::PerFlight})
                                  .Unwrap()),
      _hostWrites(hostWrites) {
    if (_hostWrites == nullptr) {
        RADRAY_ABORT("FrameResources requires a host-write batch");
    }
    SystemDescriptorPool->SetDebugName("frame_system_descriptors");
    TransientDescriptorPool->SetDebugName("frame_transient_descriptors");
}

void FrameResources::Reset() noexcept {
    if (!_hostWrites->Empty()) {
        RADRAY_ABORT("FrameResources cannot reset before its host writes are submitted");
    }
    RetireList.clear();
    RetainedObjects.clear();
    PerObjectArena.Reset();
    ViewArena.Reset();
    ObjectBindings.clear();
    ResolvedBindingStates.clear();
    std::erase_if(ResolvedGroups, [this](const FrameResolvedBindingGroupCacheEntry& entry) noexcept {
        if (!entry.Persistent) {
            return true;
        }
        return std::ranges::any_of(entry.DynamicBuffers, [this](render::Buffer* buffer) noexcept {
            return !PerObjectArena.Contains(buffer) && !ViewArena.Contains(buffer);
        });
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
