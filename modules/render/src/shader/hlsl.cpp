#include <radray/render/shader/hlsl.h>

#include <radray/utility.h>

namespace radray::render {

bool HlslShaderTypeDesc::IsPrimitive() const noexcept {
    return Class == HlslShaderVariableClass::SCALAR ||
           Class == HlslShaderVariableClass::VECTOR ||
           Class == HlslShaderVariableClass::MATRIX_ROWS ||
           Class == HlslShaderVariableClass::MATRIX_COLUMNS;
}

size_t HlslShaderTypeDesc::GetSizeInBytes() const noexcept {
    switch (Type) {
        case HlslShaderVariableType::INT16:
        case HlslShaderVariableType::UINT16:
        case HlslShaderVariableType::FLOAT16:
            return 2 * Columns * Rows;
        case HlslShaderVariableType::UINT8:
            return 1 * Columns * Rows;
        case HlslShaderVariableType::DOUBLE:
        case HlslShaderVariableType::INT64:
        case HlslShaderVariableType::UINT64:
            return 8 * Columns * Rows;
        case HlslShaderVariableType::BOOL:
        case HlslShaderVariableType::INT:
        case HlslShaderVariableType::FLOAT:
        case HlslShaderVariableType::UINT:
            return 4 * Columns * Rows;
        default:
            return 0;
    }
}

bool HlslInputBindDesc::IsUnboundArray() const noexcept {
    return BindCount == 0;
}

Nullable<const HlslShaderBufferDesc*> HlslShaderDesc::FindCBufferByName(std::string_view name) const noexcept {
    auto it = std::find_if(ConstantBuffers.begin(), ConstantBuffers.end(), [&](const HlslShaderBufferDesc& cb) {
        return cb.Name == name;
    });
    return it == ConstantBuffers.end() ? Nullable<const HlslShaderBufferDesc*>{} : Nullable<const HlslShaderBufferDesc*>{&(*it)};
}

bool IsBufferDimension(HlslSRVDimension dim) noexcept {
    switch (dim) {
        case HlslSRVDimension::BUFFER:
        case HlslSRVDimension::BUFFEREX: return true;
        default: return false;
    }
}

}  // namespace radray::render
