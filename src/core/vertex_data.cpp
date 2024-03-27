#include <radray/vertex_data.h>

namespace radray {

const char* EnumSemanticToString(InputElementSemantic e) noexcept {
    switch (e) {
        case InputElementSemantic::POSITION: return "POSITION";
        case InputElementSemantic::NORMAL: return "NORMAL";
        case InputElementSemantic::TEXCOORD: return "TEXCOORD";
        case InputElementSemantic::TANGENT: return "TANGENT";
        case InputElementSemantic::COLOR: return "COLOR";
        case InputElementSemantic::PSIZE: return "PSIZE";
        case InputElementSemantic::BINORMAL: return "BINORMAL";
        case InputElementSemantic::BLENDINDICES: return "BLENDINDICES";
        case InputElementSemantic::BLENDWEIGHT: return "BLENDWEIGHT";
        case InputElementSemantic::POSITIONT: return "POSITIONT";
        case InputElementSemantic::FOG: return "FOG";
        case InputElementSemantic::TESSFACTOR: return "TESSFACTOR";
        default: return "UNKNOWN";
    }
}

}  // namespace radray
