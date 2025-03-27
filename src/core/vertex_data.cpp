#include <radray/vertex_data.h>

#include <cctype>
#include <algorithm>

#include <radray/utility.h>

namespace radray {

std::string_view to_string(VertexIndexType val) noexcept {
    switch (val) {
        case radray::VertexIndexType::UInt16: return "UInt16";
        case radray::VertexIndexType::UInt32: return "UInt32";
    }
    Unreachable();
}

}  // namespace radray
