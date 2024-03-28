#pragma once

#include <memory>
#include <vector>
#include <string>
#include <radray/types.h>

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
    uint32 SemanticIndex;
    uint32 Size;
    uint32 Offset;
};

const char* EnumSemanticToString(VertexSemantic e) noexcept;
VertexSemantic StringToEnumSemantic(const std::string& s) noexcept;

class VertexData {
public:
    std::vector<VertexLayout> layouts;
    std::unique_ptr<uint8[]> vertexData;
    std::unique_ptr<uint8[]> indexData;
    uint64 vertexSize;
    uint64 indexSize;
    VertexIndexType indexType;
};

}  // namespace radray
