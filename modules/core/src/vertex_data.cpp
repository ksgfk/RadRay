#include <radray/vertex_data.h>

#include <cctype>
#include <algorithm>

#include <radray/utility.h>

namespace radray {

uint32_t VertexData::GetStride() const noexcept {
    uint32_t stride = 0;
    for (const auto& layout : Layouts) {
        stride += layout.Size;
    }
    return stride;
}

std::string_view to_string(VertexIndexType val) noexcept {
    switch (val) {
        case radray::VertexIndexType::UInt16: return "UInt16";
        case radray::VertexIndexType::UInt32: return "UInt32";
    }
    Unreachable();
}

}  // namespace radray
