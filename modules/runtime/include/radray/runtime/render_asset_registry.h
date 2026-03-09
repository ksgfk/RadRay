#pragma once

#include <span>

#include <radray/basic_math.h>
#include <radray/nullable.h>
#include <radray/render/common.h>
#include <radray/types.h>
#include <radray/vertex_data.h>

#include <radray/runtime/handles.h>

namespace radray::runtime {

struct TextureInitDesc {
    render::TextureDescriptor Desc{};
    string DebugName{};
};

struct ForwardOpaqueMaterialDesc {
    TextureHandle Albedo{};
    SamplerHandle Sampler{};
    Eigen::Vector4f Tint{Eigen::Vector4f::Ones()};
    string DebugName{};
};

struct MeshSubmesh {
    render::VertexBufferView VertexBuffer{};
    render::IndexBufferView IndexBuffer{};
    uint32_t VertexCount{0};
    uint32_t IndexCount{0};
    vector<render::VertexElement> VertexElements{};
    render::VertexBufferLayout VertexLayout{};
};

struct MeshAsset {
    string DebugName{};
    vector<unique_ptr<render::Buffer>> Buffers{};
    vector<vector<byte>> PendingUploadData{};
    vector<render::BufferStates> PendingFinalStates{};
    vector<MeshSubmesh> Submeshes{};
    bool IsUploaded{false};
};

struct TextureAsset {
    string DebugName{};
    render::TextureDescriptor Desc{};
    unique_ptr<render::Texture> TextureObject{};
    unique_ptr<render::TextureView> DefaultView{};
    vector<byte> PendingUploadData{};
    bool IsUploaded{false};
};

struct SamplerAsset {
    string DebugName{};
    render::SamplerDescriptor Desc{};
    unique_ptr<render::Sampler> SamplerObject{};
};

struct MaterialAsset {
    ForwardOpaqueMaterialDesc Desc{};
};

struct PendingUploadHandle {
    enum class Kind : uint32_t {
        Mesh,
        Texture,
    };

    Kind UploadKind{Kind::Mesh};
    uint32_t Value{0};
};

class RenderAssetRegistry {
public:
    explicit RenderAssetRegistry(render::Device* device) noexcept;

    MeshHandle RegisterMesh(const MeshResource& mesh);

    TextureHandle RegisterTexture2D(const TextureInitDesc& desc, std::span<const byte> texels);

    SamplerHandle RegisterSampler(const render::SamplerDescriptor& desc);

    MaterialHandle RegisterForwardOpaqueMaterial(const ForwardOpaqueMaterialDesc& desc);

    Nullable<const MeshAsset*> ResolveMesh(MeshHandle handle) const noexcept;

    Nullable<MeshAsset*> ResolveMesh(MeshHandle handle) noexcept;

    Nullable<const TextureAsset*> ResolveTexture(TextureHandle handle) const noexcept;

    Nullable<TextureAsset*> ResolveTexture(TextureHandle handle) noexcept;

    Nullable<const SamplerAsset*> ResolveSampler(SamplerHandle handle) const noexcept;

    Nullable<const MaterialAsset*> ResolveMaterial(MaterialHandle handle) const noexcept;

    std::span<const PendingUploadHandle> GetPendingUploads() const noexcept;

    void RemovePendingUpload(PendingUploadHandle pending) noexcept;

    render::Device* GetDevice() const noexcept { return _device; }

private:
    template <class THandle, class TRecord>
    static Nullable<const TRecord*> ResolveRecord(const vector<TRecord>& records, THandle handle) noexcept;

    template <class THandle, class TRecord>
    static Nullable<TRecord*> ResolveRecord(vector<TRecord>& records, THandle handle) noexcept;

    render::Device* _device{nullptr};
    vector<MeshAsset> _meshes{};
    vector<TextureAsset> _textures{};
    vector<SamplerAsset> _samplers{};
    vector<MaterialAsset> _materials{};
    vector<PendingUploadHandle> _pendingUploads{};
};

template <class THandle, class TRecord>
inline Nullable<const TRecord*> RenderAssetRegistry::ResolveRecord(const vector<TRecord>& records, THandle handle) noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(handle.Value - 1);
    if (index >= records.size()) {
        return nullptr;
    }
    return &records[index];
}

template <class THandle, class TRecord>
inline Nullable<TRecord*> RenderAssetRegistry::ResolveRecord(vector<TRecord>& records, THandle handle) noexcept {
    if (!handle.IsValid()) {
        return nullptr;
    }
    const size_t index = static_cast<size_t>(handle.Value - 1);
    if (index >= records.size()) {
        return nullptr;
    }
    return &records[index];
}

}  // namespace radray::runtime
