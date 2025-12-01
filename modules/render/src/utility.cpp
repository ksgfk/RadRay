#include <radray/render/utility.h>

#include <algorithm>
#include <limits>

#include <radray/errors.h>

#ifdef RADRAY_ENABLE_D3D12
#include <radray/render/backend/d3d12_impl.h>
#endif

#ifdef RADRAY_ENABLE_VULKAN
#include <radray/render/backend/vulkan_impl.h>
#endif

namespace radray::render {

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

}  // namespace radray::render
