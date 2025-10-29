#pragma once

#include <radray/vertex_data.h>
#include <radray/render/common.h>

namespace radray::render {

class RootSignatureSetElementContainer {
public:
    explicit RootSignatureSetElementContainer(const RootSignatureSetElement& elem) noexcept;

    static vector<RootSignatureSetElementContainer> FromView(std::span<const RootSignatureSetElement> elems) noexcept;

public:
    RootSignatureSetElement _elem;
    vector<SamplerDescriptor> _staticSamplers;
};

struct SemanticMapping {
    std::string_view Semantic;
    uint32_t SemanticIndex;
    uint32_t Location;
    VertexFormat Format;
};

bool IsDepthStencilFormat(TextureFormat format) noexcept;
uint32_t GetVertexFormatSize(VertexFormat format) noexcept;
uint32_t GetIndexFormatSize(IndexFormat format) noexcept;
PrimitiveState DefaultPrimitiveState() noexcept;
DepthStencilState DefaultDepthStencilState() noexcept;
StencilState DefaultStencilState() noexcept;
MultiSampleState DefaultMultiSampleState() noexcept;
ColorTargetState DefaultColorTargetState(TextureFormat format) noexcept;
BlendState DefaultBlendState() noexcept;
IndexFormat MapIndexType(VertexIndexType type) noexcept;
std::optional<vector<VertexElement>> MapVertexElements(std::span<VertexLayout> layouts, std::span<SemanticMapping> semantics) noexcept;

}  // namespace radray::render
