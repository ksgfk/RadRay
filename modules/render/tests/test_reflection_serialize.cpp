#include <optional>

#include <gtest/gtest.h>

#include <radray/render/shader/hlsl.h>
#include <radray/render/shader/spirv.h>

namespace radray::render {
namespace {

// 构造一个字段丰富的 HlslShaderDesc, 覆盖各类嵌套结构与枚举。
HlslShaderDesc MakeHlslSample() {
    HlslShaderDesc d{};
    d.Creator = "test-creator";
    d.Version = 96;
    d.Flags = 0x1234;
    d.MinFeatureLevel = HlslFeatureLevel::LEVEL11_0;
    d.GroupSizeX = 8;
    d.GroupSizeY = 4;
    d.GroupSizeZ = 1;

    HlslShaderBufferDesc cb{};
    cb.Name = "gMaterial";
    cb.Type = HlslCBufferType::CBUFFER;
    cb.Size = 96;
    cb.Flags = 2;
    cb.IsViewInHlsl = true;
    cb.Variables = {0, 1};
    d.ConstantBuffers.push_back(cb);

    HlslInputBindDesc bind{};
    bind.Name = "gBaseColorMap";
    bind.Type = HlslShaderInputType::TEXTURE;
    bind.BindPoint = 3;
    bind.BindCount = 1;
    bind.ReturnType = HlslResourceReturnType::FLOAT;
    bind.Dimension = HlslSRVDimension::TEXTURE2D;
    bind.NumSamples = 0;
    bind.Space = 0;
    bind.Flags = 0;
    bind.VkBinding = 7;
    bind.VkSet = 2;
    d.BoundResources.push_back(bind);

    HlslSignatureParameterDesc in{};
    in.SemanticName = "POSITION";
    in.SemanticIndex = 0;
    in.Register = 0;
    in.SystemValueType = HlslSystemValueType::UNDEFINED;
    in.ComponentType = HlslRegisterComponentType::FLOAT32;
    in.Stream = 0;
    d.InputParameters.push_back(in);

    HlslSignatureParameterDesc out{};
    out.SemanticName = "SV_Target";
    out.SemanticIndex = 0;
    out.Register = 0;
    out.SystemValueType = HlslSystemValueType::TARGET;
    out.ComponentType = HlslRegisterComponentType::FLOAT32;
    d.OutputParameters.push_back(out);

    HlslShaderVariableDesc var{};
    var.Name = "BaseColor";
    var.Type = HlslShaderTypeId{5};
    var.StartOffset = 16;
    var.Size = 16;
    var.uFlags = 1;
    d.Variables.push_back(var);

    HlslShaderTypeDesc type{};
    type.Name = "MaterialConstants";
    type.Class = HlslShaderVariableClass::MATRIX_COLUMNS;
    type.Type = HlslShaderVariableType::FLOAT;
    type.Rows = 4;
    type.Columns = 4;
    type.Elements = 0;
    type.Offset = 0;
    type.Members.push_back(HlslShaderTypeMember{.Name = "m", .Type = HlslShaderTypeId{2}});
    d.Types.push_back(type);

    return d;
}

SpirvShaderDesc MakeSpirvSample() {
    SpirvShaderDesc d{};

    SpirvTypeInfo t{};
    t.Name = "MaterialConstants";
    t.BaseType = SpirvBaseType::Struct;
    t.VectorSize = 1;
    t.Columns = 1;
    t.ArraySize = 0;
    t.ArrayStride = 0;
    t.MatrixStride = 16;
    t.Size = 96;
    t.RowMajor = false;
    t.Members.push_back(SpirvTypeMember{.Name = "BaseColor", .Offset = 0, .Size = 16, .TypeIndex = 1});
    d.Types.push_back(t);

    d.VertexInputs.push_back(SpirvVertexInput{.Name = "POSITION", .Location = 0, .TypeIndex = 2});

    SpirvResourceBinding r{};
    r.Name = "gBaseColorMap";
    r.Kind = SpirvResourceKind::SampledImage;
    r.Set = 2;
    r.Binding = 7;
    r.HlslRegister = 3;
    r.HlslSpace = 0;
    r.ArraySize = 0;
    r.TypeIndex = 0;
    r.UniformBufferSize = 0;
    r.ReadOnly = true;
    r.WriteOnly = false;
    r.IsViewInHlsl = true;
    r.IsUnboundedArray = false;
    SpirvImageInfo img{};
    img.Dim = SpirvImageDim::Dim2D;
    img.Arrayed = false;
    img.Multisampled = false;
    img.Depth = false;
    img.SampledType = 4;
    r.ImageInfo = img;
    d.ResourceBindings.push_back(r);

    d.ConstantRanges.push_back(SpirvPushConstantRange{
        .Name = "pc", .Offset = 0, .Size = 64, .TypeIndex = 0, .IsViewInHlsl = true});

    d.ComputeInfo = SpirvComputeInfo{.LocalSizeX = 8, .LocalSizeY = 4, .LocalSizeZ = 1};

    return d;
}

TEST(ReflectionSerializeTest, HlslRoundTrip) {
    HlslShaderDesc src = MakeHlslSample();
    auto json = SerializeHlslShaderDesc(src);
    ASSERT_TRUE(json.has_value());
    auto back = DeserializeHlslShaderDesc(json.value());
    ASSERT_TRUE(back.has_value());
    const HlslShaderDesc& d = back.value();

    EXPECT_EQ(d.Creator, src.Creator);
    EXPECT_EQ(d.Version, src.Version);
    EXPECT_EQ(d.Flags, src.Flags);
    EXPECT_EQ(d.MinFeatureLevel, src.MinFeatureLevel);
    EXPECT_EQ(d.GroupSizeX, src.GroupSizeX);
    EXPECT_EQ(d.GroupSizeY, src.GroupSizeY);
    EXPECT_EQ(d.GroupSizeZ, src.GroupSizeZ);

    ASSERT_EQ(d.ConstantBuffers.size(), 1u);
    EXPECT_EQ(d.ConstantBuffers[0].Name, "gMaterial");
    EXPECT_EQ(d.ConstantBuffers[0].Type, HlslCBufferType::CBUFFER);
    EXPECT_EQ(d.ConstantBuffers[0].Size, 96u);
    EXPECT_TRUE(d.ConstantBuffers[0].IsViewInHlsl);
    ASSERT_EQ(d.ConstantBuffers[0].Variables.size(), 2u);
    EXPECT_EQ(d.ConstantBuffers[0].Variables[1], 1u);

    ASSERT_EQ(d.BoundResources.size(), 1u);
    const HlslInputBindDesc& b = d.BoundResources[0];
    EXPECT_EQ(b.Name, "gBaseColorMap");
    EXPECT_EQ(b.Type, HlslShaderInputType::TEXTURE);
    EXPECT_EQ(b.BindPoint, 3u);
    EXPECT_EQ(b.Dimension, HlslSRVDimension::TEXTURE2D);
    ASSERT_TRUE(b.VkBinding.has_value());
    EXPECT_EQ(b.VkBinding.value(), 7u);
    ASSERT_TRUE(b.VkSet.has_value());
    EXPECT_EQ(b.VkSet.value(), 2u);

    ASSERT_EQ(d.InputParameters.size(), 1u);
    EXPECT_EQ(d.InputParameters[0].SemanticName, "POSITION");
    ASSERT_EQ(d.OutputParameters.size(), 1u);
    EXPECT_EQ(d.OutputParameters[0].SystemValueType, HlslSystemValueType::TARGET);

    ASSERT_EQ(d.Variables.size(), 1u);
    EXPECT_EQ(d.Variables[0].Name, "BaseColor");
    EXPECT_EQ(d.Variables[0].Type.Value, 5u);
    EXPECT_EQ(d.Variables[0].StartOffset, 16u);

    ASSERT_EQ(d.Types.size(), 1u);
    EXPECT_EQ(d.Types[0].Name, "MaterialConstants");
    EXPECT_EQ(d.Types[0].Class, HlslShaderVariableClass::MATRIX_COLUMNS);
    EXPECT_EQ(d.Types[0].Rows, 4u);
    ASSERT_EQ(d.Types[0].Members.size(), 1u);
    EXPECT_EQ(d.Types[0].Members[0].Name, "m");
    EXPECT_EQ(d.Types[0].Members[0].Type.Value, 2u);
}

TEST(ReflectionSerializeTest, SpirvRoundTrip) {
    SpirvShaderDesc src = MakeSpirvSample();
    auto json = SerializeSpirvShaderDesc(src);
    ASSERT_TRUE(json.has_value());
    auto back = DeserializeSpirvShaderDesc(json.value());
    ASSERT_TRUE(back.has_value());
    const SpirvShaderDesc& d = back.value();

    ASSERT_EQ(d.Types.size(), 1u);
    EXPECT_EQ(d.Types[0].Name, "MaterialConstants");
    EXPECT_EQ(d.Types[0].BaseType, SpirvBaseType::Struct);
    EXPECT_EQ(d.Types[0].Size, 96u);
    EXPECT_EQ(d.Types[0].MatrixStride, 16u);
    ASSERT_EQ(d.Types[0].Members.size(), 1u);
    EXPECT_EQ(d.Types[0].Members[0].Name, "BaseColor");
    EXPECT_EQ(d.Types[0].Members[0].Size, 16u);

    ASSERT_EQ(d.VertexInputs.size(), 1u);
    EXPECT_EQ(d.VertexInputs[0].Name, "POSITION");
    EXPECT_EQ(d.VertexInputs[0].Location, 0u);

    ASSERT_EQ(d.ResourceBindings.size(), 1u);
    const SpirvResourceBinding& b = d.ResourceBindings[0];
    EXPECT_EQ(b.Name, "gBaseColorMap");
    EXPECT_EQ(b.Kind, SpirvResourceKind::SampledImage);
    EXPECT_EQ(b.Set, 2u);
    EXPECT_EQ(b.Binding, 7u);
    ASSERT_TRUE(b.HlslRegister.has_value());
    EXPECT_EQ(b.HlslRegister.value(), 3u);
    EXPECT_TRUE(b.IsViewInHlsl);
    ASSERT_TRUE(b.ImageInfo.has_value());
    EXPECT_EQ(b.ImageInfo->Dim, SpirvImageDim::Dim2D);
    EXPECT_EQ(b.ImageInfo->SampledType, 4u);

    ASSERT_EQ(d.ConstantRanges.size(), 1u);
    EXPECT_EQ(d.ConstantRanges[0].Name, "pc");
    EXPECT_EQ(d.ConstantRanges[0].Size, 64u);
    EXPECT_TRUE(d.ConstantRanges[0].IsViewInHlsl);

    ASSERT_TRUE(d.ComputeInfo.has_value());
    EXPECT_EQ(d.ComputeInfo->LocalSizeX, 8u);
    EXPECT_EQ(d.ComputeInfo->LocalSizeY, 4u);
    EXPECT_EQ(d.ComputeInfo->LocalSizeZ, 1u);
}

TEST(ReflectionSerializeTest, VersionMismatchRejected) {
    // 手工构造一个版本不符的 JSON, 应被拒绝。
    const char* badJson = R"({"FormatVersion": 999, "Kind": "hlsl"})";
    auto back = DeserializeHlslShaderDesc(badJson);
    EXPECT_FALSE(back.has_value());
}

TEST(ReflectionSerializeTest, EmptyDescRoundTrip) {
    HlslShaderDesc empty{};
    auto json = SerializeHlslShaderDesc(empty);
    ASSERT_TRUE(json.has_value());
    auto back = DeserializeHlslShaderDesc(json.value());
    ASSERT_TRUE(back.has_value());
    EXPECT_TRUE(back->ConstantBuffers.empty());
    EXPECT_TRUE(back->BoundResources.empty());
    EXPECT_TRUE(back->Types.empty());
}

}  // namespace
}  // namespace radray::render
