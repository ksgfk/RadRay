#include <radray/render/utility.h>

namespace radray::render {

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
