#pragma once

#include <radray/shader/shader_compiler_contract.h>
#include <radray/nullable.h>

namespace radray::shader {

struct ShaderTypeRecord {
    string Name;
    std::optional<uint32_t> ParentIndex;
    ShaderTypeKind Kind{ShaderTypeKind::Struct};
    ShaderTypeKind ElementKind{ShaderTypeKind::Struct};
    uint32_t ElementCount{1};
    uint32_t Offset{0};
    uint32_t Size{0};
    uint32_t ElementSize{0};
    uint32_t Stride{0};
    uint32_t MatrixStride{0};
    std::optional<uint32_t> TypeIndex;
    ShaderScalarKind ScalarKind{ShaderScalarKind::None};
    uint32_t RowCount{0};
    uint32_t ColumnCount{0};
    bool RowMajor{false};
};

class ShaderResolvedMember {
public:
    ShaderTypeKind Kind{ShaderTypeKind::Scalar};
    ShaderTypeKind ElementKind{ShaderTypeKind::Scalar};
    ShaderScalarKind ScalarKind{ShaderScalarKind::None};
    uint32_t AbsoluteOffset{0};
    uint32_t Size{0};
    uint32_t ElementSize{0};
    uint32_t ElementCount{1};
    uint32_t ArrayStride{0};
    uint32_t MatrixStride{0};
    uint32_t RowCount{0};
    uint32_t ColumnCount{0};
    bool RowMajor{false};
    std::optional<uint32_t> TypeIndex;

    uint32_t VectorPayloadSize() const noexcept;
    bool HasInternalPadding() const noexcept;

private:
    friend std::optional<ShaderResolvedMember> FindShaderTypeMember(std::span<const ShaderTypeRecord>, uint32_t, std::string_view);
    bool _hasInternalPadding{false};
};

bool ValidateShaderTypeRecords(std::span<const ShaderTypeRecord> types, Nullable<string*> diagnostic = nullptr);
// Requires validated types. The result does not borrow the path or type records.
std::optional<ShaderResolvedMember> FindShaderTypeMember(std::span<const ShaderTypeRecord> types, uint32_t rootIndex, std::string_view path);

}  // namespace radray::shader
