#include <radray/vertex_data.h>

#include <cctype>
#include <algorithm>

namespace radray {

std::string_view to_string(VertexSemantic e) noexcept {
    switch (e) {
        case VertexSemantic::POSITION: return "POSITION";
        case VertexSemantic::NORMAL: return "NORMAL";
        case VertexSemantic::TEXCOORD: return "TEXCOORD";
        case VertexSemantic::TANGENT: return "TANGENT";
        case VertexSemantic::COLOR: return "COLOR";
        case VertexSemantic::PSIZE: return "PSIZE";
        case VertexSemantic::BINORMAL: return "BINORMAL";
        case VertexSemantic::BLENDINDICES: return "BLENDINDICES";
        case VertexSemantic::BLENDWEIGHT: return "BLENDWEIGHT";
        case VertexSemantic::POSITIONT: return "POSITIONT";
        default: return "Unknown";
    }
}

std::string_view to_string(VertexIndexType val) noexcept {
    switch (val) {
        case radray::VertexIndexType::UInt16: return "UInt16";
        case radray::VertexIndexType::UInt32: return "UInt32";
        default: return "Unknown";
    }
}

std::optional<VertexSemantic> VertexData::StringToEnumSemantic(const std::string& s) noexcept {
    std::string e{s};
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::toupper(c); });
    if (e == "POSITION") {
        return VertexSemantic::POSITION;
    } else if (e == "NORMAL") {
        return VertexSemantic::NORMAL;
    } else if (e == "TEXCOORD") {
        return VertexSemantic::TEXCOORD;
    } else if (e == "TANGENT") {
        return VertexSemantic::TANGENT;
    } else if (e == "COLOR") {
        return VertexSemantic::COLOR;
    } else if (e == "PSIZE") {
        return VertexSemantic::PSIZE;
    } else if (e == "BINORMAL") {
        return VertexSemantic::BINORMAL;
    } else if (e == "BLENDINDICES") {
        return VertexSemantic::BLENDINDICES;
    } else if (e == "BLENDWEIGHT") {
        return VertexSemantic::BLENDWEIGHT;
    } else if (e == "POSITIONT") {
        return VertexSemantic::POSITIONT;
    }
    return std::nullopt;
}

}  // namespace radray
