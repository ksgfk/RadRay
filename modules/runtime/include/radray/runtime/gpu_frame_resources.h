#pragma once

#include <radray/runtime/gpu_resource.h>

namespace radray {
class GpuSystem;
struct RenderServiceGeneration;

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
    /// Each call produces a distinct parameter version for this frame; caller fills the entire group.
    Nullable<render::ShaderParameterSet*> AllocateParameters(render::PipelineLayout* layout, uint32_t group, shared_ptr<RenderServiceGeneration> generation = {});

private:
    friend class GpuSystem;
    void ResetForReuse();
    struct ParameterPool {
        vector<unique_ptr<render::ShaderParameterSet>> Sets;
        size_t Used{0};
        uint32_t Idle{0};
        shared_ptr<RenderServiceGeneration> Generation;
    };
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
    unordered_map<render::PipelineLayout*, unordered_map<uint32_t, ParameterPool>> _parameters;
};
}  // namespace radray
