#pragma once

#include <memory>
#include <vector>
#include <string>
#include <optional>

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
    static std::optional<VertexSemantic> StringToEnumSemantic(const std::string& s) noexcept;

    std::vector<VertexLayout> layouts;
    std::unique_ptr<byte[]> vertexData;
    std::unique_ptr<byte[]> indexData;
    uint64_t vertexSize;
    uint64_t indexSize;
    VertexIndexType indexType;
    uint32_t indexCount;
};

const char* to_string(VertexSemantic e) noexcept;
const char* to_string(VertexIndexType val) noexcept;

}  // namespace radray

template <class CharT>
struct std::formatter<radray::VertexSemantic, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::VertexSemantic val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};

template <class CharT>
struct std::formatter<radray::VertexIndexType, CharT> : std::formatter<const char*, CharT> {
    template <class FormatContext>
    auto format(radray::VertexIndexType val, FormatContext& ctx) const {
        return formatter<const char*, CharT>::format(to_string(val), ctx);
    }
};
