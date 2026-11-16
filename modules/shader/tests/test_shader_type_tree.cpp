#include <radray/shader/shader_type_tree.h>
#include <gtest/gtest.h>

namespace radray::shader {
TEST(ShaderTypeTree, ResolvesMatrixArrayWithStoragePadding) {
    const vector<ShaderTypeRecord> types{
        {.Name = "Payload", .Size = 92, .ElementSize = 92},
        {.Name = "Matrices", .ParentIndex = 0, .Kind = ShaderTypeKind::Array, .ElementKind = ShaderTypeKind::Matrix, .ElementCount = 2, .Size = 92, .ElementSize = 44, .Stride = 48, .MatrixStride = 16, .ScalarKind = ShaderScalarKind::Float, .RowCount = 3, .ColumnCount = 3}};
    ASSERT_TRUE(ValidateShaderTypeRecords(types));
    const auto member = FindShaderTypeMember(types, 0, "Matrices[1]");
    ASSERT_TRUE(member);
    EXPECT_EQ(member->AbsoluteOffset, 48u);
    EXPECT_EQ(member->Size, 44u);
    EXPECT_TRUE(member->HasInternalPadding());
    EXPECT_FALSE(FindShaderTypeMember(types, 0, "Matrices[2]"));
}
}  // namespace radray::shader
