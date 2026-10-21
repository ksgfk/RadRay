#pragma once

#include <radray/runtime/gpu_resource.h>

namespace radray {
class GpuSystem;

/// Borrowed frame services. Allocation cursors are reset only at safe flight reuse.
class GpuFrameResources {
public:
    explicit GpuFrameResources(render::Device* device);
    struct TextureLease {
        render::Texture* Target;
        render::TextureView* View;
        render::TextureStates InitialState;
        uint64_t Epoch;
        size_t Index;
        const GpuFrameResources* Owner;
        /// A reused physical target does not inherit logical content validity.
        bool Initialized{false};
    };
    struct TextureBudget {
        size_t MaxCount{32}, MaxIdleCount{8};
        uint64_t MaxBytes{256 * 1024 * 1024};
    };
    std::optional<TextureLease> BorrowTexture(const render::TextureDescriptor& desc, render::TextureViewUsage usage);
    bool IsCurrent(const TextureLease& lease) const noexcept;
    /// Update only for commands accepted into this domain's guaranteed work.
    void SetTextureFinalState(const TextureLease& lease, render::TextureStates state);
    void SetTextureBudget(TextureBudget budget);

    void BeginEpoch();
    void SealEpoch();
    void CompleteEpoch();
    render::Device* GetDevice() const noexcept { return _device; }
    ResourceUploader& GetUploader() noexcept { return _uploader; }
    HostWriteBatch& GetHostWrites() noexcept { return _hostWrites; }
    MappedUploadPage::Reservation AllocateConstants(uint64_t size);
    MappedUploadPage::Reservation AllocateStorage(uint64_t size, uint32_t structureByteStride = 4);

private:
    friend class GpuSystem;
    void ResetForReuse();
    struct TextureEntry {
        render::TextureDescriptor Desc;
        render::TextureViewUsage Usage;
        unique_ptr<render::Texture> Texture;
        unique_ptr<render::TextureView> View;
        render::TextureStates State{render::TextureState::Undefined};
        uint64_t Bytes{0};
        bool Used{false};
    };
    void TrimTextures(uint64_t additionalBytes = 0, size_t additionalCount = 0);
    vector<TextureEntry> _textures;
    TextureBudget _textureBudget;
    uint64_t _textureBytes{0}, _epoch{0};
    bool _epochOpen{false};
    render::Device* _device;
    HostWriteBatch _hostWrites;
    StagingBufferPool _staging;
    ResourceUploader _uploader;
    DynamicCBufferArena _constants;
    vector<unique_ptr<MappedUploadPage>> _storagePages;
    size_t _storagePage{0};
    uint32_t _storageIdle{0};
};
}  // namespace radray
