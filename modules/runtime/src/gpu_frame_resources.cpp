#include <radray/runtime/gpu_frame_resources.h>

#include <algorithm>
#include <numeric>
#include <radray/logger.h>
#include <radray/runtime/render_system.h>

namespace radray {
GpuFrameResources::GpuFrameResources(render::Device* device)
    : _device(device), _staging(device), _uploader(device, _staging),
      _constants(device, &_hostWrites, DynamicCBufferArena::Descriptor{.Alignment = std::max(uint64_t{1}, device->GetCapabilities().Limits.CBufferOffsetAlignment)}) {}

void GpuFrameResources::BeginEpoch() {
    if (_epochOpen || _epoch == UINT64_MAX) RADRAY_ABORT("Invalid frame resource epoch");
    ++_epoch;
    _epochOpen = true;
    _hostWrites.Reset();
    _staging.BeginEpoch(_hostWrites);
}
void GpuFrameResources::SealEpoch() {
    if (!_epochOpen) RADRAY_ABORT("Frame resources are not open");
    _epochOpen = false;
    _hostWrites.Seal();
    _staging.SealEpoch();
}
void GpuFrameResources::CompleteEpoch() {
    ResetForReuse();
    _staging.CompleteEpoch();
    for (auto& texture : _textures) texture.Used = false;
    TrimTextures();
}

void GpuFrameResources::SetTextureBudget(TextureBudget budget) {
    _textureBudget = budget;
    if (std::none_of(_textures.begin(), _textures.end(), [](const auto& entry) { return entry.Used; })) TrimTextures();
}
void GpuFrameResources::TrimTextures(uint64_t additionalBytes, size_t additionalCount) {
    size_t idle = std::count_if(_textures.begin(), _textures.end(), [](const auto& entry) { return !entry.Used; });
    for (size_t i = _textures.size(); i && (idle > _textureBudget.MaxIdleCount ||
                                            _textures.size() + additionalCount > _textureBudget.MaxCount ||
                                            additionalBytes > _textureBudget.MaxBytes || _textureBytes > _textureBudget.MaxBytes - additionalBytes);) {
        --i;
        if (_textures[i].Used) continue;
        _textureBytes -= _textures[i].Bytes;
        _textures[i].View.reset();
        _textures[i].Texture.reset();
        _textures.erase(_textures.begin() + i);
        --idle;
    }
}
std::optional<GpuFrameResources::TextureLease> GpuFrameResources::BorrowTexture(const render::TextureDescriptor& desc, render::TextureViewUsage usage) {
    if (!_epochOpen || _hostWrites.IsSealed() || desc.Memory != render::MemoryType::Device || desc.Hints.HasFlag(render::ResourceHint::External) ||
        !desc.Width || !desc.Height || !desc.DepthOrArraySize || !desc.MipLevels || !desc.SampleCount) return std::nullopt;
    const auto matches = [&](const TextureEntry& entry) {
        const auto& stored = entry.Desc;
        return !entry.Used && entry.Usage == usage && stored.Dim == desc.Dim && stored.Width == desc.Width && stored.Height == desc.Height &&
               stored.DepthOrArraySize == desc.DepthOrArraySize && stored.MipLevels == desc.MipLevels && stored.SampleCount == desc.SampleCount &&
               stored.Format == desc.Format && stored.Memory == desc.Memory && stored.Usage == desc.Usage && stored.Hints == desc.Hints;
    };
    auto found = std::find_if(_textures.begin(), _textures.end(), matches);
    if (found == _textures.end()) {
        uint64_t bytes = 0;
        uint64_t width = desc.Width, height = desc.Height, depth = desc.DepthOrArraySize;
        const uint64_t pixel = render::GetTextureFormatBytesPerPixel(desc.Format);
        if (!pixel) return std::nullopt;
        for (uint32_t mip = 0; mip < desc.MipLevels; ++mip) {
            uint64_t size = pixel;
            for (auto factor : {width, height, depth, uint64_t{desc.SampleCount}}) {
                if (size > UINT64_MAX / factor) return std::nullopt;
                size *= factor;
            }
            if (bytes > UINT64_MAX - size) return std::nullopt;
            bytes += size;
            width = std::max(uint64_t{1}, width / 2);
            height = std::max(uint64_t{1}, height / 2);
            if (desc.Dim == render::TextureDimension::Dim3D) depth = std::max(uint64_t{1}, depth / 2);
        }
        // Never compact the vector while a lease index is borrowed in this epoch.
        if (std::any_of(_textures.begin(), _textures.end(), [](const auto& entry) { return entry.Used; })) {
            if (_textures.size() >= _textureBudget.MaxCount || bytes > _textureBudget.MaxBytes || _textureBytes > _textureBudget.MaxBytes - bytes) return std::nullopt;
        } else
            TrimTextures(bytes, 1);
        if (_textures.size() >= _textureBudget.MaxCount || bytes > _textureBudget.MaxBytes || _textureBytes > _textureBudget.MaxBytes - bytes) return std::nullopt;
        auto texture = _device->CreateTexture(desc);
        if (!texture) return std::nullopt;
        auto view = _device->CreateTextureView({texture.Get(), desc.Dim, desc.Format, {0, desc.DepthOrArraySize, 0, 1}, usage});
        if (!view) return std::nullopt;
        _textures.push_back({desc, usage, texture.Release(), view.Release(), render::TextureState::Undefined, bytes});
        _textureBytes += bytes;
        found = _textures.end() - 1;
    }
    found->Used = true;
    const auto index = static_cast<size_t>(found - _textures.begin());
    return TextureLease{found->Texture.get(), found->View.get(), found->State, _epoch, index, this};
}
bool GpuFrameResources::IsCurrent(const TextureLease& lease) const noexcept {
    return _epochOpen && lease.Owner == this && lease.Epoch == _epoch && lease.Index < _textures.size() &&
           _textures[lease.Index].Used && _textures[lease.Index].Texture.get() == lease.Target;
}
void GpuFrameResources::SetTextureFinalState(const TextureLease& lease, render::TextureStates state) {
    if (!IsCurrent(lease)) RADRAY_ABORT("Frame texture lease escaped its epoch");
    _textures[lease.Index].State = state;
}

MappedUploadPage::Reservation GpuFrameResources::AllocateConstants(uint64_t size) { return _epochOpen ? _constants.Reserve(size) : MappedUploadPage::Reservation{}; }
MappedUploadPage::Reservation GpuFrameResources::AllocateStorage(uint64_t size, uint32_t structureByteStride) {
    if (!_epochOpen || !size || !structureByteStride) return {};
    const auto deviceAlignment = std::max(uint64_t{4}, _device->GetCapabilities().Limits.StorageBufferOffsetAlignment);
    const auto divisor = std::gcd(deviceAlignment, uint64_t{structureByteStride});
    if (deviceAlignment / divisor > UINT64_MAX / structureByteStride) return {};
    const auto alignment = deviceAlignment / divisor * structureByteStride;
    for (; _storagePage < _storagePages.size(); ++_storagePage) {
        auto& page = *_storagePages[_storagePage];
        const auto remainder = page.GetUsed() % alignment;
        const auto padding = remainder ? alignment - remainder : 0;
        const auto remaining = page.GetCapacity() - page.GetUsed();
        if (padding > remaining || size > remaining - padding) continue;
        if (padding) {
            auto gap = page.Reserve(padding, 1, _hostWrites);
            gap.Commit(0);
        }
        return page.Reserve(size, 1, _hostWrites);
    }
    if (size > UINT64_MAX - alignment) return {};
    const auto limit = _device->GetCapabilities().Limits.MaxBufferSize;
    const auto capacity = std::max(uint64_t{1024 * 1024}, (size + alignment - 1) / alignment * alignment);
    if (limit && capacity > limit) return {};
    auto buffer = _device->CreateBuffer({capacity, render::MemoryType::Upload,
                                         render::BufferUse::Resource | render::BufferUse::MapWrite, render::ResourceHint::PersistentMap});
    if (!buffer) return {};
    _storagePages.push_back(make_unique<MappedUploadPage>(buffer.Release()));
    return _storagePages.back()->Reserve(size, 1, _hostWrites);
}

Nullable<render::ShaderParameterSet*> GpuFrameResources::AllocateParameters(render::PipelineLayout* layout, uint32_t group, shared_ptr<RenderServiceGeneration> generation) {
    if (!_epochOpen || (generation && !generation->Alive)) return nullptr;
    auto& pool = _parameters[layout][group];
    if (pool.Generation && pool.Generation != generation) {
        if (pool.Used) {
            RADRAY_ERR_LOG("Cannot replace a parameter layout service generation while this flight still borrows its sets");
            return nullptr;
        }
        pool = {};
    }
    pool.Generation = std::move(generation);
    if (pool.Used < pool.Sets.size()) return pool.Sets[pool.Used++].get();
    auto set = _device->CreateShaderParameterSet({layout, group});
    if (!set) return nullptr;
    pool.Sets.push_back(set.Release());
    return pool.Sets[pool.Used++].get();
}

void GpuFrameResources::ResetForReuse() {
    for (auto it = _parameters.begin(); it != _parameters.end();) {
        auto& groups = it->second;
        for (auto item = groups.begin(); item != groups.end();) {
            auto& pool = item->second;
            if (pool.Generation && !pool.Generation->Alive) {
                item = groups.erase(item);
                continue;
            }
            if (pool.Used < pool.Sets.size() && pool.Used <= pool.Sets.size() / 2)
                ++pool.Idle;
            else
                pool.Idle = 0;
            if (pool.Idle >= 60) {
                pool.Sets.resize(pool.Used);
                pool.Idle = 0;
            }
            for (size_t index = 0; index < pool.Used; ++index) pool.Sets[index]->ResetBindingsForReuse();
            pool.Used = 0;
            if (pool.Sets.empty())
                item = groups.erase(item);
            else
                ++item;
        }
        if (groups.empty())
            it = _parameters.erase(it);
        else
            ++it;
    }
    _constants.Reset();
    size_t used = 0;
    for (size_t i = 0; i < _storagePages.size(); ++i)
        if (_storagePages[i]->GetUsed()) used = i + 1;
    if (used < _storagePages.size() && used <= _storagePages.size() / 2)
        ++_storageIdle;
    else
        _storageIdle = 0;
    if (_storageIdle >= 60) {
        _storagePages.resize(used);
        _storageIdle = 0;
    }
    for (auto& page : _storagePages) page->Reset();
    _storagePage = 0;
}
}  // namespace radray
