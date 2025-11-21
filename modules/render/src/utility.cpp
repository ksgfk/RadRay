#include <radray/render/utility.h>

#include <radray/errors.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

RootSignatureSetElementContainer::RootSignatureSetElementContainer(const RootSignatureSetElement& elem) noexcept
    : _elem(elem),
      _staticSamplers(elem.StaticSamplers.begin(), elem.StaticSamplers.end()) {
    Refresh();
}

RootSignatureSetElementContainer::RootSignatureSetElementContainer(const RootSignatureSetElementContainer& other) noexcept
    : _elem(other._elem),
      _staticSamplers(other._staticSamplers) {
    Refresh();
}

RootSignatureSetElementContainer::RootSignatureSetElementContainer(RootSignatureSetElementContainer&& other) noexcept
    : _elem(other._elem),
      _staticSamplers(std::move(other._staticSamplers)) {
    Refresh();
}

RootSignatureSetElementContainer& RootSignatureSetElementContainer::operator=(const RootSignatureSetElementContainer& other) noexcept {
    RootSignatureSetElementContainer temp{other};
    swap(*this, temp);
    return *this;
}

RootSignatureSetElementContainer& RootSignatureSetElementContainer::operator=(RootSignatureSetElementContainer&& other) noexcept {
    RootSignatureSetElementContainer temp{std::move(other)};
    swap(*this, temp);
    return *this;
}

void swap(RootSignatureSetElementContainer& lhs, RootSignatureSetElementContainer& rhs) noexcept {
    using std::swap;
    swap(lhs._elem, rhs._elem);
    swap(lhs._staticSamplers, rhs._staticSamplers);
    lhs.Refresh();
    rhs.Refresh();
}

void RootSignatureSetElementContainer::Refresh() noexcept {
    _elem.StaticSamplers = std::span{_staticSamplers};
}

vector<RootSignatureSetElementContainer> RootSignatureSetElementContainer::FromView(std::span<const RootSignatureSetElement> elems) noexcept {
    vector<RootSignatureSetElementContainer> result;
    result.reserve(elems.size());
    for (const auto& i : elems) {
        result.emplace_back(i);
    }
    return result;
}

bool IsDepthStencilFormat(TextureFormat format) noexcept {
    switch (format) {
        case TextureFormat::S8:
        case TextureFormat::D16_UNORM:
        case TextureFormat::D32_FLOAT:
        case TextureFormat::D24_UNORM_S8_UINT:
        case TextureFormat::D32_FLOAT_S8_UINT: return true;
        default: return false;
    }
}

uint32_t GetVertexFormatSize(VertexFormat format) noexcept {
    switch (format) {
        case VertexFormat::UINT8X2:
        case VertexFormat::SINT8X2:
        case VertexFormat::UNORM8X2:
        case VertexFormat::SNORM8X2: return 2;
        case VertexFormat::UINT8X4:
        case VertexFormat::SINT8X4:
        case VertexFormat::UNORM8X4:
        case VertexFormat::SNORM8X4:
        case VertexFormat::UINT16X2:
        case VertexFormat::SINT16X2:
        case VertexFormat::UNORM16X2:
        case VertexFormat::SNORM16X2:
        case VertexFormat::FLOAT16X2:
        case VertexFormat::UINT32:
        case VertexFormat::SINT32:
        case VertexFormat::FLOAT32: return 4;
        case VertexFormat::UINT16X4:
        case VertexFormat::SINT16X4:
        case VertexFormat::UNORM16X4:
        case VertexFormat::SNORM16X4:
        case VertexFormat::FLOAT16X4:
        case VertexFormat::UINT32X2:
        case VertexFormat::SINT32X2:
        case VertexFormat::FLOAT32X2: return 8;
        case VertexFormat::UINT32X3:
        case VertexFormat::SINT32X3:
        case VertexFormat::FLOAT32X3: return 12;
        case VertexFormat::UINT32X4:
        case VertexFormat::SINT32X4:
        case VertexFormat::FLOAT32X4: return 16;
        case VertexFormat::UNKNOWN: return 0;
    }
    Unreachable();
}

uint32_t GetIndexFormatSize(IndexFormat format) noexcept {
    switch (format) {
        case IndexFormat::UINT16: return 2;
        case IndexFormat::UINT32: return 4;
    }
    Unreachable();
}

PrimitiveState DefaultPrimitiveState() noexcept {
    return {
        PrimitiveTopology::TriangleList,
        FrontFace::CW,
        CullMode::Back,
        PolygonMode::Fill,
        std::nullopt,
        true,
        false};
}

DepthStencilState DefaultDepthStencilState() noexcept {
    return {
        TextureFormat::D32_FLOAT,
        CompareFunction::Less,
        {
            0,
            0.0f,
            0.0f,
        },
        std::nullopt,
        true};
}

StencilState DefaultStencilState() noexcept {
    return {
        {
            CompareFunction::Always,
            StencilOperation::Keep,
            StencilOperation::Keep,
            StencilOperation::Keep,
        },
        {
            CompareFunction::Always,
            StencilOperation::Keep,
            StencilOperation::Keep,
            StencilOperation::Keep,
        },
        0xFF,
        0xFF};
}

MultiSampleState DefaultMultiSampleState() noexcept {
    return {
        .Count = 1,
        .Mask = 0xFFFFFFFF,
        .AlphaToCoverageEnable = false};
}

ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept {
    return {
        format,
        std::nullopt,
        ColorWrite::All};
}

BlendState DefaultBlendState() noexcept {
    return {
        {BlendFactor::One,
         BlendFactor::Zero,
         BlendOperation::Add},
        {BlendFactor::One,
         BlendFactor::Zero,
         BlendOperation::Add}};
}

IndexFormat MapIndexType(uint32_t size) noexcept {
    switch (size) {
        case 2: return IndexFormat::UINT16;
        case 4: return IndexFormat::UINT32;
        default: return IndexFormat::UINT32;
    }
}

std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        uint32_t wantSize = GetVertexFormatSize(want.Format);
        const VertexBufferEntry* found = nullptr;
        for (const auto& l : layouts) {
            uint32_t preSize = GetVertexDataSizeInBytes(l.Type, l.ComponentCount);
            if (l.Semantic == want.Semantic && l.SemanticIndex == want.SemanticIndex && preSize == wantSize) {
                found = &l;
                break;
            }
        }
        if (!found) {
            return std::nullopt;
        }
        VertexElement& ve = result.emplace_back();
        ve.Offset = found->Offset;
        ve.Semantic = found->Semantic;
        ve.SemanticIndex = found->SemanticIndex;
        ve.Format = want.Format;
        ve.Location = want.Location;
    }
    return result;
}

Nullable<shared_ptr<RootSignature>> CreateSerializedRootSignature(Device* device_, std::span<const byte> data) noexcept {
#ifdef RADRAY_ENABLE_D3D12
    if (device_->GetBackend() != RenderBackend::D3D12) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, Errors::InvalidOperation, "device");
        return nullptr;
    }
    auto device = d3d12::CastD3D12Object(device_);
    d3d12::ComPtr<ID3D12RootSignature> rootSig;
    if (HRESULT hr = device->_device->CreateRootSignature(0, data.data(), data.size(), IID_PPV_ARGS(&rootSig));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12Device", "CreateRootSignature", hr);
        return nullptr;
    }
    DynamicLibrary d3d12Dll{"d3d12"};
    if (!d3d12Dll.IsValid()) {
        return nullptr;
    }
    auto D3D12CreateVersionedRootSignatureDeserializer_F = d3d12Dll.GetFunction<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER>("D3D12CreateVersionedRootSignatureDeserializer");
    if (!D3D12CreateVersionedRootSignatureDeserializer_F) {
        return nullptr;
    }
    d3d12::ComPtr<ID3D12VersionedRootSignatureDeserializer> deserializer;
    if (HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer_F(data.data(), data.size(), IID_PPV_ARGS(&deserializer));
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "D3D12CreateVersionedRootSignatureDeserializer", d3d12::GetErrorName(hr), hr);
        return nullptr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc;
    if (HRESULT hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
        FAILED(hr)) {
        RADRAY_ERR_LOG("{} {}::{} {}", Errors::D3D12, "ID3D12VersionedRootSignatureDeserializer", "GetRootSignatureDescAtVersion", hr);
        return nullptr;
    }
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        RADRAY_ERR_LOG("{} {} {}", Errors::D3D12, "unknown version", desc->Version);
        return nullptr;
    }
    auto result = make_shared<d3d12::RootSigD3D12>(device, std::move(rootSig));
    result->_desc = d3d12::VersionedRootSignatureDescContainer{*desc};
    return result;
#else
    RADRAY_ERR_LOG("only d3d12 backend supports serialized root signature");
    return nullptr;
#endif
}

std::optional<RootSignatureDescriptor> GenerateRSDescFromHlslShaderDescs(std::span<const StagedHlslShaderDesc> descs) noexcept {
    vector<const HlslInputBindDesc*> mergedBindings;
    for (const auto& stagedDesc : descs) {
        if (stagedDesc.Desc == nullptr) {
            continue;
        }
        for (const auto& bind : stagedDesc.Desc->BoundResources) {
            bool alreadyMerged = false;
            for (const auto* merged : mergedBindings) {
                if (merged->BindPoint == bind.BindPoint && merged->Space == bind.Space && merged->BindCount == bind.BindCount) {
                    alreadyMerged = true;
                    break;
                }
            }
            if (!alreadyMerged) {
                mergedBindings.emplace_back(&bind);
            }
        }
    }
    // 检查绑定点是否存在重叠区域
    for (size_t i = 0; i < mergedBindings.size(); i++) {
        const auto* lhs = mergedBindings[i];
        const uint32_t lhsStart = lhs->BindPoint;
        const uint32_t lhsEnd = lhsStart + lhs->BindCount;
        for (size_t j = i + 1; j < mergedBindings.size(); j++) {
            const auto* rhs = mergedBindings[j];
            if (lhs->Space != rhs->Space) {
                continue;
            }
            const uint32_t rhsStart = rhs->BindPoint;
            const uint32_t rhsEnd = rhsStart + rhs->BindCount;
            const bool overlaps = (lhsStart < rhsEnd) && (rhsStart < lhsEnd);
            if (overlaps) {
                RADRAY_ERR_LOG("{} space:{} {}-{} overlaps {}-{}", Errors::DXC, lhs->Space, lhs->BindPoint, lhsEnd, rhs->BindPoint, rhsEnd);
                return std::nullopt;
            }
        }
    }

    vector<const HlslShaderBufferDesc*> mergedCbuffers;
    for (auto bind : mergedBindings) {
        // TODO:
    }

    // vector<const HlslShaderBufferDesc*> mergedCbuffers;
    // for (const auto& stagedDesc : descs) {
    //     if (stagedDesc.Desc == nullptr) {
    //         continue;
    //     }
    //     for (const auto& cb : stagedDesc.Desc->ConstantBuffers) {
    //         bool alreadyMerged = false;
    //         for (const auto* merged : mergedCbuffers) {
    //             if ((*merged) == cb) {
    //                 alreadyMerged = true;
    //                 break;
    //             }
    //         }
    //         if (!alreadyMerged) {
    //             mergedCbuffers.emplace_back(&cb);
    //         }
    //     }
    // }
    // if (!mergedCbuffers.empty()) {
    //     for (size_t i = 0; i < mergedCbuffers.size(); i++) {
    //         const auto* iCB = mergedCbuffers[i];
    //         for (size_t j = 0; j < mergedCbuffers.size(); j++) {
    //             if (i == j) continue;
    //             const auto* jCB = mergedCbuffers[j];

    //         }
    //     }
    // }
    return std::nullopt;
}

}  // namespace radray::render
