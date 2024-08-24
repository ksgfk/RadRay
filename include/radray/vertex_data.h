#pragma once

#include <optional>
#include <string_view>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

enum class VertexSemantic {
    POSITION,
    NORMAL,
    TEXCOORD,
    TANGENT,
    COLOR,
    PSIZE,
    BINORMAL,
    BLENDINDICES,
    BLENDWEIGHT,
    POSITIONT
};

enum class VertexIndexType {
    UInt32,
    UInt16
};

struct VertexLayout {
    VertexSemantic Semantic;
    uint32_t SemanticIndex;
    uint32_t Size;
    uint32_t Offset;
};

class VertexData {
public:
    static std::optional<VertexSemantic> StringToEnumSemantic(const radray::string& s) noexcept;

    radray::vector<VertexLayout> layouts;
    radray::unique_ptr<byte[]> vertexData;
    radray::unique_ptr<byte[]> indexData;
    uint64_t vertexSize;
    uint64_t indexSize;
    VertexIndexType indexType;
    uint32_t indexCount;
};

std::string_view to_string(VertexSemantic e) noexcept;
std::string_view to_string(VertexIndexType val) noexcept;

}  // namespace radray

template <class CharT>
struct fmt::formatter<radray::VertexSemantic, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(radray::VertexSemantic val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct fmt::formatter<radray::VertexIndexType, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(radray::VertexIndexType val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(to_string(val), ctx);
    }
};
