#include <radray/runtime/vertex_factory.h>

#include <radray/logger.h>

namespace radray {

render::VertexFormat VertexFactory::ToVertexFormat(VertexDataType type, uint16_t componentCount) noexcept {
    switch (type) {
        case VertexDataType::FLOAT:
            switch (componentCount) {
                case 1: return render::VertexFormat::FLOAT32;
                case 2: return render::VertexFormat::FLOAT32X2;
                case 3: return render::VertexFormat::FLOAT32X3;
                case 4: return render::VertexFormat::FLOAT32X4;
                default: return render::VertexFormat::UNKNOWN;
            }
        case VertexDataType::UINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::UINT32;
                case 2: return render::VertexFormat::UINT32X2;
                case 3: return render::VertexFormat::UINT32X3;
                case 4: return render::VertexFormat::UINT32X4;
                default: return render::VertexFormat::UNKNOWN;
            }
        case VertexDataType::SINT:
            switch (componentCount) {
                case 1: return render::VertexFormat::SINT32;
                case 2: return render::VertexFormat::SINT32X2;
                case 3: return render::VertexFormat::SINT32X3;
                case 4: return render::VertexFormat::SINT32X4;
                default: return render::VertexFormat::UNKNOWN;
            }
        default:
            return render::VertexFormat::UNKNOWN;
    }
}

VertexFactory::Layout VertexFactory::BuildLayout(const MeshPrimitive& primitive) {
    Layout layout{};
    uint32_t location = 0;
    for (const VertexBufferEntry& entry : primitive.VertexBuffers) {
        render::VertexFormat fmt = ToVertexFormat(entry.Type, entry.ComponentCount);
        if (fmt == render::VertexFormat::UNKNOWN) {
            RADRAY_ERR_LOG("VertexFactory: unsupported attribute '{}' (type/count not 32-bit scalar)", entry.Semantic);
            layout.Elements.clear();
            layout.Stride = 0;
            return layout;
        }
        layout.Elements.emplace_back(render::VertexElement{
            .Offset = entry.Offset,
            .Semantic = entry.Semantic,
            .SemanticIndex = entry.SemanticIndex,
            .Format = fmt,
            .Location = location++});
        layout.Stride = entry.Stride;  // single interleaved buffer: stride is shared
    }
    return layout;
}

string VertexFactory::BuildSignature(const render::VertexBufferLayout& layout) {
    string sig = fmt::format("stride={};", layout.ArrayStride);
    for (const render::VertexElement& e : layout.Elements) {
        sig += fmt::format(
            "[{}:{}@{}={}]",
            e.Semantic,
            e.SemanticIndex,
            e.Offset,
            static_cast<uint32_t>(e.Format));
    }
    return sig;
}

}  // namespace radray
