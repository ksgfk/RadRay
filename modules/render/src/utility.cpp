#include <radray/render/utility.h>

namespace radray::render {

RootSignatureSetElementContainer::RootSignatureSetElementContainer(const RootSignatureSetElement& elem) noexcept
    : _elem(elem),
      _staticSamplers(elem.StaticSamplers.begin(), elem.StaticSamplers.end()) {
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
        case VertexFormat::UNKNOWN:
        default: return 0;
    }
}

uint32_t GetIndexFormatSize(IndexFormat format) noexcept {
    switch (format) {
        case IndexFormat::UINT16: return 2;
        case IndexFormat::UINT32: return 4;
        default: return 0;
    }
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

IndexFormat MapIndexType(VertexIndexType type) noexcept {
    switch (type) {
        case VertexIndexType::UInt16: return IndexFormat::UINT16;
        case VertexIndexType::UInt32: return IndexFormat::UINT32;
        default: return IndexFormat::UINT16;
    }
}

std::optional<vector<VertexElement>> MapVertexElements(std::span<VertexLayout> layouts, std::span<SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        const VertexLayout* found = nullptr;
        for (const auto& l : layouts) {
            uint32_t wantSize = GetVertexFormatSize(want.Format);
            if (l.Semantic == want.Semantic && l.SemanticIndex == want.SemanticIndex && l.Size == wantSize) {
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

}  // namespace radray::render
