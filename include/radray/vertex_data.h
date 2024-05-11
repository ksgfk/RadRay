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
    POSITIONT,
    FOG,
    TESSFACTOR
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
    static const char* EnumSemanticToString(VertexSemantic e) noexcept;
    static std::optional<VertexSemantic> StringToEnumSemantic(const std::string& s) noexcept;

    std::vector<VertexLayout> layouts;
    std::unique_ptr<byte[]> vertexData;
    std::unique_ptr<byte[]> indexData;
    uint64_t vertexSize;
    uint64_t indexSize;
    VertexIndexType indexType;
    uint32_t indexCount;
};

}  // namespace radray

template<>
struct std::formatter<radray::VertexSemantic> : public std::formatter<const char*> {
    auto format(radray::VertexSemantic const& val, std::format_context& ctx) const -> decltype(ctx.out());
};

template<>
struct std::formatter<radray::VertexIndexType> : public std::formatter<const char*> {
    auto format(radray::VertexIndexType const& val, std::format_context& ctx) const -> decltype(ctx.out());
};
