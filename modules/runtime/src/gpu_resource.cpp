#include <radray/runtime/gpu_resource.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <cstring>
#include <string_view>
#include <utility>
#include <bit>

#include <radray/file.h>
#include <radray/logger.h>
#include <radray/vertex_data.h>
#include <radray/hash.h>
#include <radray/render/shader_compiler/dxc.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/shader_asset.h>

#if defined(RADRAY_ENABLE_SPIRV_CROSS)
#include <radray/render/shader_compiler/spvc.h>
#endif

namespace radray {

HostWriteBatch::HostWriteBatch() {
    _ranges.reserve(64);
}

void HostWriteBatch::Record(render::Buffer* target, render::BufferRange range) {
    if (_sealed) {
        RADRAY_ABORT("cannot record a host write after the batch has been sealed");
    }
    if (target == nullptr) {
        RADRAY_ABORT("cannot record a host write for a null buffer");
    }
    const render::BufferDescriptor desc = target->GetDesc();
    if (!desc.Usage.HasFlag(render::BufferUse::MapWrite) ||
        !desc.Hints.HasFlag(render::ResourceHint::PersistentMap)) {
        RADRAY_ABORT("host-write batches require MapWrite + PersistentMap buffers");
    }
    if (range.Offset > desc.Size) {
        RADRAY_ABORT("host-write offset {} exceeds buffer size {}", range.Offset, desc.Size);
    }
    const uint64_t size = range.Size == render::BufferRange::All()
                              ? desc.Size - range.Offset
                              : range.Size;
    if (size > desc.Size - range.Offset) {
        RADRAY_ABORT(
            "host-write range [{}, {}) exceeds buffer size {}",
            range.Offset,
            range.Offset + size,
            desc.Size);
    }

    ++_stats.CommitCount;
    _stats.CommittedBytes += size;
    if (size == 0) {
        return;
    }
    ++_stats.RecordedRangeCount;

    const uint64_t rangeEnd = range.Offset + size;
    if (!_ranges.empty()) {
        render::MappedBufferRange& last = _ranges.back();
        if (last.Target == target) {
            const uint64_t lastEnd = last.Range.Offset + last.Range.Size;
            if (range.Offset <= lastEnd && last.Range.Offset <= rangeEnd) {
                const uint64_t begin = std::min(last.Range.Offset, range.Offset);
                const uint64_t end = std::max(lastEnd, rangeEnd);
                last.Range = render::BufferRange{.Offset = begin, .Size = end - begin};
                return;
            }
        }
    }
    _ranges.push_back(render::MappedBufferRange{
        .Target = target,
        .Range = render::BufferRange{.Offset = range.Offset, .Size = size}});
}

void HostWriteBatch::Flush(render::Device& device) noexcept {
    device.FlushMappedRanges(_ranges);
    _stats.FlushedRangeCount += _ranges.size();
    _ranges.clear();
}

void HostWriteBatch::Reset() noexcept {
    _ranges.clear();
    _sealed = false;
}

void HostWriteBatch::RecordPageAllocation(uint64_t capacity) noexcept {
    ++_stats.PageCount;
    _stats.PageCapacityBytes += capacity;
}

ScopedBufferMap::ScopedBufferMap(render::Buffer* buffer, render::BufferRange range) noexcept
    : _buffer(buffer) {
    if (_buffer == nullptr) {
        RADRAY_ABORT("ScopedBufferMap requires a non-null buffer");
    }
    const render::BufferDescriptor desc = _buffer->GetDesc();
    if (range.Offset > desc.Size) {
        RADRAY_ABORT("ScopedBufferMap range is outside the buffer");
    }
    const uint64_t size = range.Size == render::BufferRange::All()
                              ? desc.Size - range.Offset
                              : range.Size;
    if (size == 0 || size > desc.Size - range.Offset) {
        RADRAY_ABORT("ScopedBufferMap range is outside the buffer");
    }
    const bool read = desc.Usage.HasFlag(render::BufferUse::MapRead);
    _write = desc.Usage.HasFlag(render::BufferUse::MapWrite);
    if (read == _write) {
        RADRAY_ABORT("ScopedBufferMap requires exactly one of MapRead or MapWrite");
    }

    _range = render::BufferRange{.Offset = range.Offset, .Size = size};
    _data = _buffer->Map(_range.Offset, _range.Size);
    if (_data != nullptr && read) {
        _buffer->InvalidateMappedRange(_range);
    }
}

ScopedBufferMap::~ScopedBufferMap() noexcept {
    if (_buffer == nullptr || _data == nullptr) {
        return;
    }
    if (_write) {
        _buffer->FlushMappedRange(_range);
    }
    _buffer->Unmap();
}

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
    if (!std::has_single_bit(alignment)) {
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

// ═══════════════════════════════════════════════════════════════
//  StagingBufferPool
// ═══════════════════════════════════════════════════════════════

StagingBufferPool::StagingBufferPool(
    render::Device* device,
    uint32_t flightCount,
    const Descriptor& desc) noexcept
    : _device(device), _desc(desc) {
    if (_desc.PageSize == 0) {
        RADRAY_ABORT("StagingBufferPool page size must be non-zero");
    }
    _pending.resize(flightCount);
}

StagingBufferPool::StagingBufferPool(render::Device* device, uint32_t flightCount) noexcept
    : StagingBufferPool(device, flightCount, Descriptor{}) {}

StagingBufferPool::~StagingBufferPool() noexcept = default;

void StagingBufferPool::BeginFlight(HostWriteBatch& hostWrites) {
    if (_hostWrites != nullptr) {
        RADRAY_ABORT("StagingBufferPool::BeginFlight called while another flight is active");
    }
    _hostWrites = &hostWrites;
}

StagingBufferPool::Page StagingBufferPool::CreatePage(uint64_t capacity, bool cacheable) {
    if (_hostWrites == nullptr) {
        RADRAY_ABORT("StagingBufferPool cannot create a page outside BeginFlight/RetireToFlight");
    }
    render::BufferDescriptor desc{
        .Size = capacity,
        .Memory = render::MemoryType::Upload,
        .Usage = render::BufferUse::CopySource | render::BufferUse::MapWrite,
        .Hints = render::ResourceHint::PersistentMap};
    auto bufferOpt = _device->CreateBuffer(desc);
    if (!bufferOpt.HasValue()) {
        RADRAY_ABORT("StagingBufferPool failed to create upload buffer of size {}", capacity);
    }
    auto buffer = bufferOpt.Release();
    const std::string_view namePrefix = cacheable ? "staging_page" : "staging_large";
    buffer->SetDebugName(fmt::format("{}_{}", namePrefix, _nextPageId++));
    return Page{
        .Upload = make_unique<MappedUploadPage>(std::move(buffer), _hostWrites),
        .Cacheable = cacheable};
}

StagingBufferPool::Page& StagingBufferPool::AcquireStandardPage() {
    if (!_freeList.empty()) {
        Page page = std::move(_freeList.back());
        _freeList.pop_back();
        page.Upload->Reset();
        _active.emplace_back(std::move(page));
    } else {
        _active.emplace_back(CreatePage(_desc.PageSize, true));
    }
    return _active.back();
}

StagingBufferPool::Reservation StagingBufferPool::Reserve(uint64_t size, uint64_t alignment) {
    if (size == 0) {
        return {};
    }
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        RADRAY_ABORT("StagingBufferPool alignment {} must be a non-zero power of two", alignment);
    }
    if (_device == nullptr) {
        RADRAY_ABORT("StagingBufferPool cannot reserve without a render device");
    }
    if (_hostWrites == nullptr) {
        RADRAY_ABORT("StagingBufferPool::Reserve requires an active flight");
    }

    for (auto& page : _active) {
        if (!page.Cacheable) {
            continue;
        }
        Reservation reservation = page.Upload->Reserve(size, alignment, *_hostWrites);
        if (reservation.IsValid()) {
            return reservation;
        }
    }

    Page* page = nullptr;
    if (size <= _desc.PageSize) {
        page = &AcquireStandardPage();
    } else {
        if (size > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
            RADRAY_ABORT("StagingBufferPool allocation size {} overflows alignment {}", size, alignment);
        }
        const uint64_t capacity = Align(size, alignment);
        _active.emplace_back(CreatePage(capacity, false));
        page = &_active.back();
    }
    Reservation reservation = page->Upload->Reserve(size, alignment, *_hostWrites);
    if (!reservation.IsValid()) {
        RADRAY_ABORT(
            "StagingBufferPool failed to reserve {} bytes with alignment {} from a {} byte page",
            size,
            alignment,
            page->Upload->GetCapacity());
    }
    return reservation;
}

void StagingBufferPool::RetireToFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _pending.size());
    if (_hostWrites == nullptr) {
        RADRAY_ABORT("StagingBufferPool::RetireToFlight requires an active flight");
    }
    auto& pending = _pending[flightIndex];
    pending.insert(
        pending.end(),
        std::make_move_iterator(_active.begin()),
        std::make_move_iterator(_active.end()));
    _active.clear();
    _hostWrites = nullptr;
}

void StagingBufferPool::CollectFlight(uint32_t flightIndex) {
    RADRAY_ASSERT(flightIndex < _pending.size());
    auto& pending = _pending[flightIndex];
    for (auto& page : pending) {
        if (page.Cacheable) {
            page.Upload->Reset();
            _freeList.emplace_back(std::move(page));
        }
    }
    pending.clear();
    TrimFreeList();
}

void StagingBufferPool::TrimFreeList() noexcept {
    std::erase_if(_freeList, [this](const Page& page) noexcept {
        return page.Upload == nullptr ||
               !page.Cacheable ||
               page.Upload->GetCapacity() != _desc.PageSize;
    });

    uint64_t cachedBytes = 0;
    for (const auto& page : _freeList) {
        const uint64_t pageSize = page.Upload->GetCapacity();
        cachedBytes = pageSize > std::numeric_limits<uint64_t>::max() - cachedBytes
                          ? std::numeric_limits<uint64_t>::max()
                          : cachedBytes + pageSize;
    }
    while (!_freeList.empty() &&
           (_freeList.size() > _desc.MaxCachedPages || cachedBytes > _desc.MaxCachedBytes)) {
        cachedBytes -= _freeList.back().Upload->GetCapacity();
        _freeList.pop_back();
    }
}

ResourceUploader::ResourceUploader(render::Device* device, uint32_t flightCount)
    : _device(device),
      _stagingPool(device, flightCount),
      _flightCount(flightCount) {}

ResourceUploader::~ResourceUploader() noexcept = default;

void ResourceUploader::BeginFlight(uint32_t flightIndex, HostWriteBatch& hostWrites) {
    if (flightIndex >= _flightCount) {
        RADRAY_ABORT("ResourceUploader flight index {} is out of range", flightIndex);
    }
    if (_activeFlightIndex != std::numeric_limits<uint32_t>::max()) {
        RADRAY_ABORT("ResourceUploader already has an active flight");
    }
    _activeFlightIndex = flightIndex;
    _stagingPool.BeginFlight(hostWrites);
}

void ResourceUploader::UploadBuffer(
    render::CommandBuffer* cmdBuffer,
    const BufferUploadRequest& request) {
    if (request.SrcData.empty() || request.DstBuffer == nullptr) {
        return;
    }
    const uint64_t size = request.SrcData.size();
    const auto dstDesc = request.DstBuffer->GetDesc();
    if (request.DstOffset > dstDesc.Size || size > dstDesc.Size - request.DstOffset) {
        return;
    }

    const uint64_t copyAlignment = std::max<uint64_t>(
        1,
        _device->GetDetail().BufferCopyOffsetAlignment);
    auto reservation = _stagingPool.Reserve(size, copyAlignment);
    std::memcpy(reservation.Data(), request.SrcData.data(), size);
    const auto alloc = reservation.Commit(size);

    vector<render::ResourceBarrierDescriptor> barriersBefore;
    barriersBefore.emplace_back(render::BarrierBufferDescriptor{
        .Target = alloc.Target,
        .Before = render::BufferState::HostWrite,
        .After = render::BufferState::CopySource});
    barriersBefore.emplace_back(render::BarrierBufferDescriptor{
        .Target = request.DstBuffer,
        .Before = request.Before,
        .After = render::BufferState::CopyDestination});
    cmdBuffer->ResourceBarrier(barriersBefore);
    cmdBuffer->CopyBufferToBuffer(
        request.DstBuffer, request.DstOffset,
        alloc.Target, alloc.Offset,
        size);

    render::ResourceBarrierDescriptor barrierAfter = render::BarrierBufferDescriptor{
        .Target = request.DstBuffer,
        .Before = render::BufferState::CopyDestination,
        .After = request.After};
    cmdBuffer->ResourceBarrier(std::span{&barrierAfter, 1});
}

namespace {

std::optional<uint64_t> GetSubresourceUploadSize(
    const render::TextureDescriptor& desc,
    const render::SubresourceRange& range,
    std::span<const byte> srcData,
    uint64_t srcRowPitch,
    uint64_t dstRowPitch) noexcept {
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (bytesPerPixel == 0) {
        return std::nullopt;
    }
    const bool is3D = desc.Dim == render::TextureDimension::Dim3D;
    const uint32_t mipLevel = range.BaseMipLevel;
    const uint32_t mipWidth = std::max(desc.Width >> mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.Height >> mipLevel, 1u);
    const uint32_t mipDepth = is3D ? std::max(desc.DepthOrArraySize >> mipLevel, 1u) : 1u;
    const uint64_t tightRowPitch = static_cast<uint64_t>(mipWidth) * bytesPerPixel;
    if (srcRowPitch < tightRowPitch || dstRowPitch < tightRowPitch) {
        return std::nullopt;
    }
    const uint64_t totalRows = static_cast<uint64_t>(mipHeight) * mipDepth;
    if (totalRows == 0) {
        return std::nullopt;
    }
    const uint64_t requiredSrcSize = (totalRows - 1) * srcRowPitch + tightRowPitch;
    if (srcData.size() < requiredSrcSize) {
        return std::nullopt;
    }
    return dstRowPitch * totalRows;
}

}  // namespace

void ResourceUploader::UploadTexture(
    render::CommandBuffer* cmdBuffer,
    const TextureUploadRequest& request) {
    if (request.SrcData.empty() || request.DstTexture == nullptr) {
        return;
    }
    const auto desc = request.DstTexture->GetDesc();
    const bool is3D = desc.Dim == render::TextureDimension::Dim3D;
    const uint32_t arraySize = is3D ? 1u : desc.DepthOrArraySize;
    const uint32_t bytesPerPixel = render::GetTextureFormatBytesPerPixel(desc.Format);
    if (bytesPerPixel == 0 ||
        request.DstRange.MipLevelCount != 1 ||
        request.DstRange.ArrayLayerCount != 1 ||
        request.DstRange.BaseMipLevel >= desc.MipLevels ||
        request.DstRange.BaseArrayLayer >= arraySize) {
        return;
    }

    const uint32_t baseWidth = std::max(desc.Width >> request.DstRange.BaseMipLevel, 1u);
    const uint64_t tightRowPitch = static_cast<uint64_t>(baseWidth) * bytesPerPixel;
    const uint64_t srcRowPitch = request.SrcRowPitch == 0 ? tightRowPitch : request.SrcRowPitch;
    if (srcRowPitch < tightRowPitch) {
        return;
    }
    const uint64_t dstRowPitch = Align(
        tightRowPitch,
        std::max<uint64_t>(1, _device->GetDetail().TextureDataPitchAlignment));
    const auto uploadSize = GetSubresourceUploadSize(
        desc, request.DstRange, request.SrcData, srcRowPitch, dstRowPitch);
    if (!uploadSize.has_value()) {
        return;
    }

    const uint64_t placementAlignment = std::max({uint64_t{1},
                                                  static_cast<uint64_t>(bytesPerPixel),
                                                  _device->GetDetail().TextureDataPlacementAlignment});
    auto reservation = _stagingPool.Reserve(uploadSize.value(), placementAlignment);
    auto* dst = static_cast<byte*>(reservation.Data());
    const auto* src = request.SrcData.data();
    const uint32_t mipLevel = request.DstRange.BaseMipLevel;
    const uint32_t mipWidth = std::max(desc.Width >> mipLevel, 1u);
    const uint32_t mipHeight = std::max(desc.Height >> mipLevel, 1u);
    const uint32_t mipDepth = is3D ? std::max(desc.DepthOrArraySize >> mipLevel, 1u) : 1u;
    const uint64_t rowBytes = static_cast<uint64_t>(mipWidth) * bytesPerPixel;
    uint64_t srcOffset = 0;
    uint64_t dstOffset = 0;
    for (uint32_t depth = 0; depth < mipDepth; ++depth) {
        for (uint32_t row = 0; row < mipHeight; ++row) {
            std::memcpy(dst + dstOffset, src + srcOffset, rowBytes);
            srcOffset += srcRowPitch;
            dstOffset += dstRowPitch;
        }
    }
    const auto alloc = reservation.Commit(uploadSize.value());

    vector<render::ResourceBarrierDescriptor> barriersBefore;
    barriersBefore.emplace_back(render::BarrierBufferDescriptor{
        .Target = alloc.Target,
        .Before = render::BufferState::HostWrite,
        .After = render::BufferState::CopySource});
    barriersBefore.emplace_back(render::BarrierTextureDescriptor{
        .Target = request.DstTexture,
        .Before = request.Before,
        .After = render::TextureState::CopyDestination});
    cmdBuffer->ResourceBarrier(barriersBefore);
    cmdBuffer->CopyBufferToTexture(request.DstTexture, request.DstRange, alloc.Target, alloc.Offset);

    render::ResourceBarrierDescriptor barrierAfter = render::BarrierTextureDescriptor{
        .Target = request.DstTexture,
        .Before = render::TextureState::CopyDestination,
        .After = request.After};
    cmdBuffer->ResourceBarrier(std::span{&barrierAfter, 1});
}

void ResourceUploader::EndFlight(uint32_t flightIndex) {
    if (_activeFlightIndex != flightIndex) {
        RADRAY_ABORT(
            "ResourceUploader ended flight {} while flight {} is active",
            flightIndex,
            _activeFlightIndex);
    }
    _stagingPool.RetireToFlight(flightIndex);
    _activeFlightIndex = std::numeric_limits<uint32_t>::max();
}

void ResourceUploader::CollectFlight(uint32_t flightIndex) {
    _stagingPool.CollectFlight(flightIndex);
}

std::optional<GpuMesh> ResourceUploader::UploadMeshResource(
    render::CommandBuffer* cmdBuffer,
    const MeshResource& meshResource) {
    if (meshResource.Primitives.empty()) {
        return std::nullopt;
    }

    GpuMesh result;
    vector<Nullable<render::Buffer*>> bufferByBin(meshResource.Bins.size());
    for (size_t binIdx = 0; binIdx < meshResource.Bins.size(); ++binIdx) {
        const MeshBuffer& bin = meshResource.Bins[binIdx];
        auto data = bin.GetData();
        if (data.empty()) {
            continue;
        }

        render::BufferDescriptor bufDesc{
            .Size = data.size(),
            .Memory = render::MemoryType::Device,
            .Usage = render::BufferUse::Vertex | render::BufferUse::Index | render::BufferUse::CopyDestination,
            .Hints = render::ResourceHint::None};
        auto bufOpt = _device->CreateBuffer(bufDesc);
        if (!bufOpt.HasValue()) {
            return std::nullopt;
        }
        auto buf = bufOpt.Release();
        buf->SetDebugName(fmt::format("{}_{}", meshResource.Name, binIdx));
        UploadBuffer(cmdBuffer, BufferUploadRequest{
                                    .SrcData = data,
                                    .DstBuffer = buf.get(),
                                    .DstOffset = 0,
                                    .Before = render::BufferState::Common,
                                    .After = render::BufferState::Vertex | render::BufferState::Index});

        bufferByBin[binIdx] = buf.get();
        result.Buffers.emplace_back(std::move(buf));
    }

    for (size_t primIdx = 0; primIdx < meshResource.Primitives.size(); ++primIdx) {
        const MeshPrimitive& prim = meshResource.Primitives[primIdx];
        GpuMesh::DrawData drawData{};
        if (!prim.VertexBuffers.empty()) {
            const VertexBufferEntry& vbEntry = prim.VertexBuffers[0];
            if (vbEntry.BufferIndex < bufferByBin.size() && bufferByBin[vbEntry.BufferIndex].HasValue()) {
                const uint64_t vbSize = static_cast<uint64_t>(prim.VertexCount) * vbEntry.Stride;
                drawData.Vbv = render::VertexBufferView{
                    .Target = bufferByBin[vbEntry.BufferIndex].Get(),
                    .Offset = vbEntry.Offset,
                    .Size = vbSize};
            }
        }
        if (prim.IndexBuffer.BufferIndex < bufferByBin.size() && bufferByBin[prim.IndexBuffer.BufferIndex].HasValue()) {
            drawData.Ibv = render::IndexBufferView{
                .Target = bufferByBin[prim.IndexBuffer.BufferIndex].Get(),
                .Offset = prim.IndexBuffer.Offset,
                .Stride = prim.IndexBuffer.Stride};
        }
        result.Draws.emplace_back(drawData);
    }

    return result;
}

DynamicCBufferArena::Block::Block(unique_ptr<MappedUploadPage> page) noexcept
    : Page(std::move(page)) {}

DynamicCBufferArena::DynamicCBufferArena(
    render::Device* device,
    HostWriteBatch* hostWrites,
    const Descriptor& desc) noexcept
    : _device(device), _hostWrites(hostWrites), _desc(desc) {
    if (!std::has_single_bit(_desc.Alignment)) {
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
        .Hints = render::ResourceHint::PersistentMap};
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

RenderPassRegistry::RenderPassRegistry(render::Device* device) noexcept
    : _device(device) {}

RenderPassRegistry::~RenderPassRegistry() noexcept {
    Clear();
}

Nullable<render::RenderPass*> RenderPassRegistry::GetOrCreateRenderPass(
    const render::RenderPassDescriptor& desc) noexcept {
    for (PassEntry& entry : _passes) {
        if (entry.ColorAttachments.size() == desc.ColorAttachments.size() &&
            std::equal(entry.ColorAttachments.begin(), entry.ColorAttachments.end(), desc.ColorAttachments.begin()) &&
            entry.DepthStencilAttachment == desc.DepthStencilAttachment) {
            ++_renderPassHits;
            return entry.Object.get();
        }
    }

    ++_renderPassMisses;
    if (_device == nullptr) {
        return nullptr;
    }
    auto passOpt = _device->CreateRenderPass(desc);
    if (!passOpt.HasValue()) {
        return nullptr;
    }
    auto pass = passOpt.Release();
    render::RenderPass* result = pass.get();
    _passes.push_back(PassEntry{
        .ColorAttachments = vector<render::RenderPassColorAttachmentDescriptor>{
            desc.ColorAttachments.begin(), desc.ColorAttachments.end()},
        .DepthStencilAttachment = desc.DepthStencilAttachment,
        .Object = std::move(pass)});
    return result;
}

Nullable<render::Framebuffer*> RenderPassRegistry::GetOrCreateFramebuffer(
    render::RenderPass* pass,
    std::span<render::TextureView* const> colorAttachments,
    render::TextureView* depthStencilAttachment,
    uint32_t width,
    uint32_t height,
    uint32_t layers) noexcept {
    for (FramebufferEntry& entry : _framebuffers) {
        if (entry.Pass == pass && entry.DepthStencilAttachment == depthStencilAttachment &&
            entry.Width == width && entry.Height == height && entry.Layers == layers &&
            entry.ColorAttachments.size() == colorAttachments.size() &&
            std::equal(entry.ColorAttachments.begin(), entry.ColorAttachments.end(), colorAttachments.begin())) {
            ++_framebufferHits;
            return entry.Object.get();
        }
    }

    ++_framebufferMisses;
    if (_device == nullptr) {
        return nullptr;
    }
    render::FramebufferDescriptor desc{
        .Pass = pass,
        .ColorAttachments = colorAttachments,
        .DepthStencilAttachment = depthStencilAttachment,
        .Width = width,
        .Height = height,
        .Layers = layers};
    auto framebufferOpt = _device->CreateFramebuffer(desc);
    if (!framebufferOpt.HasValue()) {
        return nullptr;
    }
    auto framebuffer = framebufferOpt.Release();
    render::Framebuffer* result = framebuffer.get();
    _framebuffers.push_back(FramebufferEntry{
        .Pass = pass,
        .ColorAttachments = vector<render::TextureView*>{colorAttachments.begin(), colorAttachments.end()},
        .DepthStencilAttachment = depthStencilAttachment,
        .Width = width,
        .Height = height,
        .Layers = layers,
        .Object = std::move(framebuffer)});
    return result;
}

void RenderPassRegistry::RemoveFramebuffersUsing(render::TextureView* attachment) noexcept {
    if (attachment == nullptr) {
        return;
    }
    std::erase_if(_framebuffers, [attachment](FramebufferEntry& entry) noexcept {
        const bool match = entry.DepthStencilAttachment == attachment ||
                           std::ranges::find(entry.ColorAttachments, attachment) != entry.ColorAttachments.end();
        if (match && entry.Object != nullptr) {
            entry.Object->Destroy();
        }
        return match;
    });
}

void RenderPassRegistry::ClearFramebuffers() noexcept {
    for (FramebufferEntry& entry : _framebuffers) {
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _framebuffers.clear();
}

void RenderPassRegistry::Clear() noexcept {
    ClearFramebuffers();
    for (PassEntry& entry : _passes) {
        if (entry.Object != nullptr) {
            entry.Object->Destroy();
        }
    }
    _passes.clear();
}

SamplerCache::SamplerCache(render::Device* device) noexcept
    : _device(device) {}

Nullable<render::Sampler*> SamplerCache::GetOrCreate(const render::SamplerDescriptor& desc) noexcept {
    if (_device == nullptr) {
        return nullptr;
    }
    if (auto it = _cache.find(desc); it != _cache.end()) {
        return it->second.get();
    }
    auto samplerOpt = _device->CreateSampler(desc);
    if (!samplerOpt.HasValue()) {
        RADRAY_ERR_LOG("SamplerCache::GetOrCreate: failed to create sampler");
        return nullptr;
    }
    render::Sampler* raw = samplerOpt.Get();
    _cache.emplace(desc, samplerOpt.Release());
    return raw;
}

namespace {

void NormalizeShaderModuleKey(ShaderModuleKey& key) {
    std::erase_if(key.Defines, [](const string& define) noexcept { return define.empty(); });
    std::ranges::sort(key.Defines);
    key.Defines.erase(std::unique(key.Defines.begin(), key.Defines.end()), key.Defines.end());
}

std::optional<std::string_view> FindShaderEntryPoint(
    const ShaderPassDesc& pass,
    render::ShaderStage stage) noexcept {
    if (const auto* graphics = std::get_if<ShaderGraphicsPassDesc>(&pass.Program)) {
        if (stage == render::ShaderStage::Vertex) {
            return graphics->VertexEntry;
        }
        if (stage == render::ShaderStage::Pixel && graphics->PixelEntry.has_value()) {
            return *graphics->PixelEntry;
        }
        return std::nullopt;
    }

    const auto* compute = std::get_if<ShaderComputePassDesc>(&pass.Program);
    if (compute != nullptr && stage == render::ShaderStage::Compute) {
        return compute->EntryPoint;
    }
    return std::nullopt;
}

bool AreShaderDefinesValid(
    const ShaderPassDesc& pass,
    render::ShaderStage stage,
    const vector<string>& defines) noexcept {
    for (const string& define : defines) {
        const bool declaredForStage = std::ranges::any_of(
            pass.KeywordGroups,
            [stage, &define](const ShaderKeywordGroupDesc& group) noexcept {
                const bool affectsStage = group.Stages == render::ShaderStage::UNKNOWN ||
                                          group.Stages.HasFlag(stage);
                return affectsStage && std::ranges::find(group.Alternatives, define) != group.Alternatives.end();
            });
        if (!declaredForStage) {
            return false;
        }
    }

    for (const ShaderKeywordGroupDesc& group : pass.KeywordGroups) {
        if (group.Stages != render::ShaderStage::UNKNOWN && !group.Stages.HasFlag(stage)) {
            continue;
        }
        const size_t selectedCount = std::ranges::count_if(
            group.Alternatives,
            [&defines](const string& alternative) noexcept {
                return !alternative.empty() &&
                       std::ranges::find(defines, alternative) != defines.end();
            });
        if (selectedCount > 1) {
            return false;
        }
    }
    return true;
}

}  // namespace

ShaderModuleCache::ShaderModuleCache(
    render::Device* device,
    render::Dxc* dxc,
    AssetManager* assetManager,
    string shaderSourceRoot) noexcept
    : _device(device),
      _dxc(dxc),
      _assetManager(assetManager),
      _shaderSourceRoot(std::move(shaderSourceRoot)) {}

ShaderModuleCache::~ShaderModuleCache() noexcept {
    Clear();
}

Nullable<render::Shader*> ShaderModuleCache::GetOrCreate(const ShaderModuleKey& sourceKey) noexcept {
    ShaderModuleKey key = sourceKey;
    NormalizeShaderModuleKey(key);

    if (const auto cached = _cache.find(key); cached != _cache.end()) {
        return cached->second.get();
    }

#if !defined(RADRAY_ENABLE_DXC)
    RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: DXC is not enabled");
    return nullptr;
#else
    if (_device == nullptr || _dxc == nullptr || _assetManager == nullptr) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: device, DXC, or AssetManager is null");
        return nullptr;
    }
    if (key.Shader.IsEmpty()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: shader asset id is empty");
        return nullptr;
    }

    StreamingAssetRef<ShaderAsset> shaderRef = _assetManager->Get<ShaderAsset>(key.Shader);
    ShaderAsset* shaderAsset = shaderRef.Get();
    if (shaderAsset == nullptr) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: ShaderAsset {} is not ready", key.Shader);
        return nullptr;
    }
    if (!shaderAsset->IsValid()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: ShaderAsset {} is invalid", key.Shader);
        return nullptr;
    }

    const vector<ShaderPassDesc>& passes = shaderAsset->GetPasses();
    if (key.PassIndex >= passes.size()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: pass {} is out of range for ShaderAsset {}",
            key.PassIndex,
            key.Shader);
        return nullptr;
    }
    const ShaderPassDesc& pass = passes[key.PassIndex];
    const std::optional<std::string_view> entryPoint = FindShaderEntryPoint(pass, key.Stage);
    if (!entryPoint.has_value()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: pass {} of ShaderAsset {} has no {} stage",
            key.PassIndex,
            key.Shader,
            key.Stage);
        return nullptr;
    }
    if (!AreShaderDefinesValid(pass, key.Stage, key.Defines)) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: defines are invalid for ShaderAsset {} pass {} stage {}",
            key.Shader,
            key.PassIndex,
            key.Stage);
        return nullptr;
    }
    const std::filesystem::path relativeSourcePath{pass.SourcePath};
    std::filesystem::path sourcePath;
    std::error_code fileError;

    const std::filesystem::path executableDirectory = GetExecutableDirectory();
    if (!executableDirectory.empty()) {
        const std::filesystem::path executableRelativePath = executableDirectory / relativeSourcePath;
        if (std::filesystem::is_regular_file(executableRelativePath, fileError)) {
            sourcePath = executableRelativePath;
        }
    }

    if (sourcePath.empty() && !_shaderSourceRoot.empty()) {
        const std::filesystem::path rootRelativePath =
            std::filesystem::path{_shaderSourceRoot} / relativeSourcePath;
        fileError.clear();
        if (std::filesystem::is_regular_file(rootRelativePath, fileError)) {
            sourcePath = rootRelativePath;
        }
    }

    if (sourcePath.empty()) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: shader source '{}' not found", pass.SourcePath);
        return nullptr;
    }

    vector<std::string_view> defines;
    defines.reserve(key.Defines.size());
    for (const string& define : key.Defines) {
        defines.emplace_back(define);
    }
    const std::string_view includeRoot = _shaderSourceRoot;
    const std::array includeDirs{includeRoot};

    const render::RenderBackend backend = _device->GetBackend();
    if (backend != render::RenderBackend::D3D12 && backend != render::RenderBackend::Vulkan) {
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: unsupported render backend {}", backend);
        return nullptr;
    }

    render::DxcCompileOptions compileOptions{};
    compileOptions.EntryPoint = *entryPoint;
    compileOptions.Stage = key.Stage;
    compileOptions.SM = pass.SM;
    compileOptions.Defines = defines;
    compileOptions.Includes = includeDirs;
    compileOptions.IsOptimize = pass.IsOptimize;
    compileOptions.IsSpirv = backend == render::RenderBackend::Vulkan;
    compileOptions.EnableUnbounded = pass.EnableUnbounded;

    std::optional<render::DxcOutput> outputOpt = _dxc->CompileFile(sourcePath, compileOptions);
    if (!outputOpt.has_value()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: DXC failed for '{}' entry '{}' stage {}",
            sourcePath.string(),
            *entryPoint,
            key.Stage);
        return nullptr;
    }
    render::DxcOutput output = std::move(*outputOpt);

    render::ShaderReflectionDesc reflection{};
    if (backend == render::RenderBackend::D3D12) {
        std::optional<render::HlslShaderDesc> reflectionOpt = _dxc->GetShaderDescFromOutput(output.Refl);
        if (!reflectionOpt.has_value()) {
            RADRAY_ERR_LOG(
                "ShaderModuleCache::GetOrCreate: DXIL reflection failed for '{}' entry '{}'",
                sourcePath.string(),
                *entryPoint);
            return nullptr;
        }
        reflection = std::move(*reflectionOpt);
    } else {
#if defined(RADRAY_ENABLE_SPIRV_CROSS)
        std::optional<render::SpirvShaderDesc> reflectionOpt = render::ReflectSpirv(render::SpirvBytecodeView{
            .Data = output.Data,
            .EntryPointName = *entryPoint,
            .Stage = key.Stage});
        if (!reflectionOpt.has_value()) {
            RADRAY_ERR_LOG(
                "ShaderModuleCache::GetOrCreate: SPIR-V reflection failed for '{}' entry '{}'",
                sourcePath.string(),
                *entryPoint);
            return nullptr;
        }
        reflection = std::move(*reflectionOpt);
#else
        RADRAY_ERR_LOG("ShaderModuleCache::GetOrCreate: SPIR-V Cross is not enabled");
        return nullptr;
#endif
    }

    render::ShaderDescriptor shaderDesc{
        .Source = output.Data,
        .Category = output.Category,
        .Stages = key.Stage,
        .Reflection = std::move(reflection)};
    auto shaderOpt = _device->CreateShader(shaderDesc);
    if (!shaderOpt.HasValue()) {
        RADRAY_ERR_LOG(
            "ShaderModuleCache::GetOrCreate: Device::CreateShader failed for ShaderAsset {} pass {} stage {}",
            key.Shader,
            key.PassIndex,
            key.Stage);
        return nullptr;
    }

    unique_ptr<render::Shader> shader = shaderOpt.Release();
    render::Shader* result = shader.get();
    _cache.emplace(std::move(key), std::move(shader));
    return result;
#endif
}

void ShaderModuleCache::Clear() noexcept {
    for (auto& [key, shader] : _cache) {
        (void)key;
        if (shader != nullptr) {
            shader->Destroy();
        }
    }
    _cache.clear();
}

bool operator==(const ShaderModuleKey& lhs, const ShaderModuleKey& rhs) noexcept {
    return lhs.Shader == rhs.Shader && lhs.Defines == rhs.Defines && lhs.PassIndex == rhs.PassIndex && lhs.Stage == rhs.Stage;
}

}  // namespace radray

std::size_t std::hash<radray::ShaderModuleKey>::operator()(const radray::ShaderModuleKey& key) const noexcept {
    radray::HashCode hash;
    hash.Add(key.Shader);
    hash.Add(key.PassIndex);
    hash.Add(static_cast<radray::uint32_t>(key.Stage));
    hash.Add(key.Defines.size());
    for (const radray::string& value : key.Defines) {
        hash.Add(value);
    }
    return hash.ToHashCode();
}
