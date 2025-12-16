#include <radray/render/render_utility.h>

namespace radray::render {

std::optional<vector<VertexElement>> MapVertexElements(std::span<const VertexBufferEntry> layouts, std::span<const SemanticMapping> semantics) noexcept {
    vector<VertexElement> result;
    result.reserve(semantics.size());
    for (const auto& want : semantics) {
        uint32_t wantSize = GetVertexFormatSizeInBytes(want.Format);
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
