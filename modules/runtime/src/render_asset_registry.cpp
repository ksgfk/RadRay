#include <algorithm>

#include <fmt/format.h>

#include <radray/logger.h>

#include <radray/runtime/render_asset_registry.h>

namespace radray::runtime {
namespace {

std::optional<render::VertexFormat> MapVertexFormat(VertexDataType type, uint16_t componentCount) noexcept {
    switch (type) {
        case VertexDataType::FLOAT:
            switch (componentCount) {
                case 1: return render::VertexFormat::FLOAT32;
                case 2: return render::VertexFormat::FLOAT32X2;
                case 3: return render::VertexFormat::FLOAT32X3;
                case 4: return render::VertexFormat::FLOAT32X4;
                default: return std::nullopt;
            }
        case VertexDataType::UINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::UINT32;
                case 2: return render::VertexFormat::UINT32X2;
                case 3: return render::VertexFormat::UINT32X3;
                case 4: return render::VertexFormat::UINT32X4;
                default: return std::nullopt;
            }
        case VertexDataType::SINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::SINT32;
                case 2: return render::VertexFormat::SINT32X2;
                case 3: return render::VertexFormat::SINT32X3;
                case 4: return render::VertexFormat::SINT32X4;
                default: return std::nullopt;
            }
        default: return std::nullopt;
    }
}

template <class THandle>
THandle MakeHandle(size_t size) noexcept {
    return THandle{static_cast<uint32_t>(size)};
}

}  // namespace

RenderAssetRegistry::RenderAssetRegistry(render::Device* device) noexcept
    : _device(device) {}

MeshHandle RenderAssetRegistry::RegisterMesh(const MeshResource& mesh) {
    if (_device == nullptr) {
        RADRAY_ERR_LOG("RenderAssetRegistry::RegisterMesh called without device");
        return MeshHandle::Invalid();
    }
    if (mesh.Primitives.empty() || mesh.Bins.empty()) {
        RADRAY_ERR_LOG("mesh resource '{}' is empty", mesh.Name);
        return MeshHandle::Invalid();
    }

    vector<render::BufferUses> usages(mesh.Bins.size(), render::BufferUse::CopyDestination);
    vector<render::BufferStates> finalStates(mesh.Bins.size(), render::BufferState::Common);
    MeshAsset asset{};
    asset.DebugName = mesh.Name;
    asset.Buffers.resize(mesh.Bins.size());
    asset.PendingUploadData.resize(mesh.Bins.size());
    asset.PendingFinalStates.resize(mesh.Bins.size());

    for (const auto& primitive : mesh.Primitives) {
        for (const auto& vb : primitive.VertexBuffers) {
            if (vb.BufferIndex >= usages.size()) {
                RADRAY_ERR_LOG("vertex buffer index {} is out of range", vb.BufferIndex);
                return MeshHandle::Invalid();
            }
            usages[vb.BufferIndex] |= render::BufferUse::Vertex;
            finalStates[vb.BufferIndex] |= render::BufferState::Vertex;
        }
        if (primitive.IndexBuffer.BufferIndex >= usages.size()) {
            RADRAY_ERR_LOG("index buffer index {} is out of range", primitive.IndexBuffer.BufferIndex);
            return MeshHandle::Invalid();
        }
        if (primitive.IndexBuffer.Stride != sizeof(uint32_t)) {
            RADRAY_ERR_LOG("mesh '{}' only supports uint32 indices in runtime MVP", mesh.Name);
            return MeshHandle::Invalid();
        }
        usages[primitive.IndexBuffer.BufferIndex] |= render::BufferUse::Index;
        finalStates[primitive.IndexBuffer.BufferIndex] |= render::BufferState::Index;
    }

    for (size_t index = 0; index < mesh.Bins.size(); ++index) {
        render::BufferDescriptor desc{};
        desc.Size = mesh.Bins[index].GetSize();
        desc.Memory = render::MemoryType::Device;
        desc.Usage = usages[index];
        desc.Hints = render::ResourceHint::None;
        auto bufferOpt = _device->CreateBuffer(desc);
        if (!bufferOpt.HasValue()) {
            RADRAY_ERR_LOG("CreateBuffer failed for mesh '{}' bin {}", mesh.Name, index);
            return MeshHandle::Invalid();
        }

        auto buffer = bufferOpt.Release();
        buffer->SetDebugName(fmt::format("{}_bin_{}", mesh.Name, index));
        asset.Buffers[index] = std::move(buffer);
        auto bytes = mesh.Bins[index].GetData();
        asset.PendingUploadData[index].assign(bytes.begin(), bytes.end());
        asset.PendingFinalStates[index] = finalStates[index];
    }

    for (const auto& primitive : mesh.Primitives) {
        if (primitive.VertexBuffers.empty()) {
            RADRAY_ERR_LOG("mesh '{}' primitive missing vertex buffers", mesh.Name);
            return MeshHandle::Invalid();
        }
        const uint32_t vertexBufferIndex = primitive.VertexBuffers.front().BufferIndex;
        const bool allVertexBuffersShareBin = std::all_of(
            primitive.VertexBuffers.begin(),
            primitive.VertexBuffers.end(),
            [&](const VertexBufferEntry& entry) noexcept {
                return entry.BufferIndex == vertexBufferIndex;
            });
        if (!allVertexBuffersShareBin) {
            RADRAY_ERR_LOG("mesh '{}' primitive uses multiple vertex bins, which is unsupported in runtime MVP", mesh.Name);
            return MeshHandle::Invalid();
        }

        MeshSubmesh submesh{};
        submesh.VertexCount = primitive.VertexCount;
        submesh.IndexCount = primitive.IndexBuffer.IndexCount;
        submesh.VertexBuffer = render::VertexBufferView{
            .Target = asset.Buffers[vertexBufferIndex].get(),
            .Offset = 0,
            .Size = asset.Buffers[vertexBufferIndex]->GetDesc().Size,
        };
        submesh.IndexBuffer = render::IndexBufferView{
            .Target = asset.Buffers[primitive.IndexBuffer.BufferIndex].get(),
            .Offset = primitive.IndexBuffer.Offset,
            .Stride = primitive.IndexBuffer.Stride,
        };

        auto addElement = [&](std::string_view semantic, uint32_t location) -> bool {
            const auto it = std::find_if(
                primitive.VertexBuffers.begin(),
                primitive.VertexBuffers.end(),
                [&](const VertexBufferEntry& entry) noexcept {
                    return entry.Semantic == semantic && entry.SemanticIndex == 0;
                });
            if (it == primitive.VertexBuffers.end()) {
                return false;
            }
            const auto formatOpt = MapVertexFormat(it->Type, it->ComponentCount);
            if (!formatOpt.has_value()) {
                RADRAY_ERR_LOG("mesh '{}' semantic '{}' has unsupported vertex format", mesh.Name, semantic);
                return false;
            }
            submesh.VertexElements.push_back(render::VertexElement{
                .Offset = it->Offset,
                .Semantic = semantic,
                .SemanticIndex = it->SemanticIndex,
                .Format = formatOpt.value(),
                .Location = location,
            });
            return true;
        };

        if (!addElement(VertexSemantics::POSITION, 0) ||
            !addElement(VertexSemantics::TEXCOORD, 1)) {
            RADRAY_ERR_LOG("mesh '{}' must provide POSITION and TEXCOORD0", mesh.Name);
            return MeshHandle::Invalid();
        }

        submesh.VertexLayout = render::VertexBufferLayout{
            .ArrayStride = primitive.VertexBuffers.front().Stride,
            .StepMode = render::VertexStepMode::Vertex,
            .Elements = submesh.VertexElements,
        };
        asset.Submeshes.push_back(std::move(submesh));
    }

    _meshes.push_back(std::move(asset));
    _pendingUploads.push_back(PendingUploadHandle{
        .UploadKind = PendingUploadHandle::Kind::Mesh,
        .Value = static_cast<uint32_t>(_meshes.size()),
    });
    return MakeHandle<MeshHandle>(_meshes.size());
}

TextureHandle RenderAssetRegistry::RegisterTexture2D(const TextureInitDesc& desc, std::span<const byte> texels) {
    if (_device == nullptr) {
        RADRAY_ERR_LOG("RenderAssetRegistry::RegisterTexture2D called without device");
        return TextureHandle::Invalid();
    }
    if (desc.Desc.Dim != render::TextureDimension::Dim2D ||
        desc.Desc.DepthOrArraySize != 1 ||
        desc.Desc.MipLevels != 1 ||
        desc.Desc.SampleCount != 1) {
        RADRAY_ERR_LOG("RegisterTexture2D only supports 2D single-mip textures");
        return TextureHandle::Invalid();
    }

    render::TextureDescriptor textureDesc = desc.Desc;
    textureDesc.Usage |= render::TextureUse::CopyDestination;
    textureDesc.Usage |= render::TextureUse::Resource;
    auto textureOpt = _device->CreateTexture(textureDesc);
    if (!textureOpt.HasValue()) {
        RADRAY_ERR_LOG("CreateTexture failed for '{}'", desc.DebugName);
        return TextureHandle::Invalid();
    }

    TextureAsset asset{};
    asset.DebugName = desc.DebugName;
    asset.Desc = textureDesc;
    asset.TextureObject = textureOpt.Release();
    asset.TextureObject->SetDebugName(desc.DebugName);
    asset.PendingUploadData.assign(texels.begin(), texels.end());

    render::TextureViewDescriptor viewDesc{};
    viewDesc.Target = asset.TextureObject.get();
    viewDesc.Dim = render::TextureDimension::Dim2D;
    viewDesc.Format = textureDesc.Format;
    viewDesc.Range = render::SubresourceRange{0, 1, 0, 1};
    viewDesc.Usage = render::TextureViewUsage::Resource;
    auto viewOpt = _device->CreateTextureView(viewDesc);
    if (!viewOpt.HasValue()) {
        RADRAY_ERR_LOG("CreateTextureView failed for '{}'", desc.DebugName);
        return TextureHandle::Invalid();
    }
    asset.DefaultView = viewOpt.Release();
    asset.DefaultView->SetDebugName(fmt::format("{}_srv", desc.DebugName));

    _textures.push_back(std::move(asset));
    _pendingUploads.push_back(PendingUploadHandle{
        .UploadKind = PendingUploadHandle::Kind::Texture,
        .Value = static_cast<uint32_t>(_textures.size()),
    });
    return MakeHandle<TextureHandle>(_textures.size());
}

SamplerHandle RenderAssetRegistry::RegisterSampler(const render::SamplerDescriptor& desc) {
    if (_device == nullptr) {
        RADRAY_ERR_LOG("RenderAssetRegistry::RegisterSampler called without device");
        return SamplerHandle::Invalid();
    }

    auto samplerOpt = _device->CreateSampler(desc);
    if (!samplerOpt.HasValue()) {
        RADRAY_ERR_LOG("CreateSampler failed");
        return SamplerHandle::Invalid();
    }

    SamplerAsset asset{};
    asset.Desc = desc;
    asset.SamplerObject = samplerOpt.Release();
    asset.DebugName = fmt::format("sampler_{}", _samplers.size());
    asset.SamplerObject->SetDebugName(asset.DebugName);
    _samplers.push_back(std::move(asset));
    return MakeHandle<SamplerHandle>(_samplers.size());
}

MaterialHandle RenderAssetRegistry::RegisterForwardOpaqueMaterial(const ForwardOpaqueMaterialDesc& desc) {
    if (!desc.Albedo.IsValid() || !desc.Sampler.IsValid()) {
        RADRAY_ERR_LOG("forward opaque material requires valid texture and sampler handles");
        return MaterialHandle::Invalid();
    }
    if (!this->ResolveTexture(desc.Albedo).HasValue() ||
        !this->ResolveSampler(desc.Sampler).HasValue()) {
        RADRAY_ERR_LOG("forward opaque material references unknown texture or sampler handle");
        return MaterialHandle::Invalid();
    }

    MaterialAsset asset{};
    asset.Desc = desc;
    _materials.push_back(std::move(asset));
    return MakeHandle<MaterialHandle>(_materials.size());
}

Nullable<const MeshAsset*> RenderAssetRegistry::ResolveMesh(MeshHandle handle) const noexcept {
    return ResolveRecord(_meshes, handle);
}

Nullable<MeshAsset*> RenderAssetRegistry::ResolveMesh(MeshHandle handle) noexcept {
    return ResolveRecord(_meshes, handle);
}

Nullable<const TextureAsset*> RenderAssetRegistry::ResolveTexture(TextureHandle handle) const noexcept {
    return ResolveRecord(_textures, handle);
}

Nullable<TextureAsset*> RenderAssetRegistry::ResolveTexture(TextureHandle handle) noexcept {
    return ResolveRecord(_textures, handle);
}

Nullable<const SamplerAsset*> RenderAssetRegistry::ResolveSampler(SamplerHandle handle) const noexcept {
    return ResolveRecord(_samplers, handle);
}

Nullable<const MaterialAsset*> RenderAssetRegistry::ResolveMaterial(MaterialHandle handle) const noexcept {
    return ResolveRecord(_materials, handle);
}

std::span<const PendingUploadHandle> RenderAssetRegistry::GetPendingUploads() const noexcept {
    return _pendingUploads;
}

void RenderAssetRegistry::RemovePendingUpload(PendingUploadHandle pending) noexcept {
    auto it = std::find_if(
        _pendingUploads.begin(),
        _pendingUploads.end(),
        [&](const PendingUploadHandle& value) noexcept {
            return value.UploadKind == pending.UploadKind && value.Value == pending.Value;
        });
    if (it != _pendingUploads.end()) {
        _pendingUploads.erase(it);
    }
}

}  // namespace radray::runtime
