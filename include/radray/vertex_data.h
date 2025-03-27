#pragma once

#include <optional>
#include <string_view>

#include <radray/types.h>
#include <radray/logger.h>

namespace radray {

class VertexSemantic {
public:
    static constexpr std::string_view POSITION = "POSITION";
    static constexpr std::string_view NORMAL = "NORMAL";
    static constexpr std::string_view TEXCOORD = "TEXCOORD";
    static constexpr std::string_view TANGENT = "TANGENT";
    static constexpr std::string_view COLOR = "COLOR";
    static constexpr std::string_view PSIZE = "PSIZE";
    static constexpr std::string_view BINORMAL = "BINORMAL";
    static constexpr std::string_view BLENDINDICES = "BLENDINDICES";
    static constexpr std::string_view BLENDWEIGHT = "BLENDWEIGHT";
    static constexpr std::string_view POSITIONT = "POSITIONT";
};

enum class VertexIndexType {
    UInt32,
    UInt16
};

class VertexLayout {
public:
    radray::string Semantic;
    uint32_t SemanticIndex;
    uint32_t Size;
    uint32_t Offset;
};

class VertexData {
public:
    radray::vector<VertexLayout> Layouts;
    radray::unique_ptr<byte[]> VertexData;
    radray::unique_ptr<byte[]> IndexData;
    uint32_t VertexSize;
    uint32_t IndexSize;
    VertexIndexType IndexType;
    uint32_t IndexCount;
};

std::string_view to_string(VertexIndexType val) noexcept;

}  // namespace radray

template <class CharT>
struct fmt::formatter<radray::VertexIndexType, CharT> : fmt::formatter<std::string_view, CharT> {
    template <class FormatContext>
    auto format(radray::VertexIndexType val, FormatContext& ctx) const {
        return formatter<std::string_view, CharT>::format(to_string(val), ctx);
    }
};
