#include <algorithm>
#include <gtest/gtest.h>
#include <radray/binary_io.h>
#include <radray/json.h>
#include "layout_test_data.h"

namespace radray {
namespace {
using namespace test;
TEST(ShaderLayout, MatrixArraySpanAndIndexedQueryExcludeTrailingPadding) {
    const auto desc = MatrixLayout();
    ASSERT_TRUE(shader::ValidateShaderTypeRecords(desc.Types));
    string error;
    ASSERT_TRUE(shader::ValidateShaderPipelineLayoutDesc(desc, &error)) << error;
    const auto array = FindShaderTypeMember(desc.Types, 0, "Matrices");
    const auto element = FindShaderTypeMember(desc.Types, 0, "Matrices[1]");
    ASSERT_TRUE(array && element);
    EXPECT_EQ(array->Size, 92u);
    EXPECT_EQ(array->ArrayStride, 48u);
    EXPECT_EQ(element->Kind, shader::ShaderTypeKind::Matrix);
    EXPECT_EQ(element->AbsoluteOffset, 48u);
    EXPECT_EQ(element->Size, 44u);
    EXPECT_EQ(element->ArrayStride, 0u);
    EXPECT_EQ(element->MatrixStride, 16u);
    EXPECT_EQ(element->VectorPayloadSize(), 12u);
    EXPECT_TRUE(element->HasInternalPadding());
    EXPECT_FALSE(FindShaderTypeMember(desc.Types, 0, "Matrices[2]"));
    EXPECT_FALSE(FindShaderTypeMember(desc.Types, 0, "Matrices[1]."));
    EXPECT_FALSE(FindShaderTypeMember(desc.Types, 0, "Matrices[-1]"));
}

TEST(ShaderLayout, RejectsArraySpanThatIncludesLastElementsTailPadding) {
    auto desc = MatrixLayout();
    desc.Types[1].Size = 96;
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(desc));
}

TEST(ShaderLayout, RejectsCyclesThroughUnderlyingTypeReferences) {
    shader::ShaderPipelineLayoutDesc desc;
    desc.Types.push_back({.Name = "Recursive", .Size = 16, .ElementSize = 16});
    desc.Types.push_back({.Name = "Self", .ParentIndex = 0, .Size = 16, .ElementSize = 16, .TypeIndex = 0});
    string error;
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(desc, &error));
    EXPECT_NE(error.find("cyclic"), string::npos);
}

TEST(ShaderLayout, FamilyRoundTripsJsonAndBinaryWithTargetInterfaces) {
    shader::ShaderFamilyLayoutDesc family{.SharedLayout = MatrixLayout()};
    shader::ShaderVariantTargetInterfaceDesc target{.Stages = gpu::ShaderStage::Compute, .ActiveBindings = {{2, 7}}};
    family.Variants.push_back({.Name = "Default", .DXIL = target, .SPIRV = target});
    const auto json = shader::WriteShaderFamilyLayoutJson(family);
    const auto binary = shader::WriteShaderFamilyLayoutBinary(family);
    ASSERT_TRUE(json && binary);
    shader::ShaderFamilyLayoutDesc decodedJson, decodedBinary;
    ASSERT_TRUE(shader::ReadShaderFamilyLayoutJson(*json, decodedJson));
    ASSERT_TRUE(shader::ReadShaderFamilyLayoutBinary(*binary, decodedBinary));
    EXPECT_EQ(shader::WriteShaderFamilyLayoutBinary(decodedJson), binary);
    EXPECT_EQ(shader::WriteShaderFamilyLayoutBinary(decodedBinary), binary);
}

TEST(ShaderLayout, ReadsBaselineMatrixLayout) {
    constexpr uint8_t baseline[]{
        0x52, 0x52, 0x53, 0x4c, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00, 0x4d, 0x61, 0x74, 0x65, 0x72, 0x69, 0x61, 0x6c, 0x02, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x6e, 0x73, 0x74, 0x61, 0x6e, 0x74,
        0x73, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00,
        0x00, 0x07, 0x00, 0x00, 0x00, 0x50, 0x61, 0x79, 0x6c, 0x6f, 0x61, 0x64, 0x00, 0x04, 0x00, 0x00,
        0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00,
        0x00, 0x5c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x4d,
        0x61, 0x74, 0x72, 0x69, 0x63, 0x65, 0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x03, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5c, 0x00, 0x00, 0x00,
        0x2c, 0x00, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x03, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00,
    };
    shader::ShaderPipelineLayoutDesc layout;
    ASSERT_TRUE(shader::ReadShaderPipelineLayoutBinary(std::as_bytes(std::span{baseline}), layout));
    const auto encoded = shader::WriteShaderPipelineLayoutBinary(layout);
    ASSERT_TRUE(encoded);
    EXPECT_EQ(encoded, shader::WriteShaderPipelineLayoutBinary(MatrixLayout()));
    EXPECT_TRUE(std::ranges::equal(*encoded, std::as_bytes(std::span{baseline})));
}

TEST(ShaderLayout, VersionOneVertexEnumsKeepTheirOriginalWireValues) {
    for (uint32_t wireKind = 0; wireKind != 3; ++wireKind) {
        BinaryWriter writer;
        writer.U32(0x4c535252u);
        writer.U32(1);
        writer.U32(2);
        writer.String("Legacy");
        writer.Bool(true);
        writer.U32(1);  // Vertex stage in version 1.
        writer.Size32(1);
        writer.String("POSITION");
        writer.U32(0);
        writer.U32(3);
        writer.U32(wireKind);
        writer.U32(3);
        writer.Size32(0);
        writer.Size32(0);
        writer.Bool(false);
        const auto bytes = std::move(writer).TakeData();
        shader::ShaderVariantInterfaceDesc decoded;
        ASSERT_TRUE(shader::ReadShaderVariantInterfaceBinary(bytes, decoded));
        ASSERT_TRUE(decoded.DXIL);
        EXPECT_EQ(decoded.DXIL->VertexInputs[0].ComponentKind,
                  static_cast<shader::ShaderVertexComponentType>(wireKind + 1));
        EXPECT_EQ(shader::WriteShaderVariantInterfaceBinary(decoded), bytes);
    }
}

TEST(ShaderLayout, VersionOneJsonKeepsLegacyIntegerNames) {
    const std::string_view json = R"({"Version":1,"Kind":"ShaderVariantInterface","Data":{
        "Name":"Legacy","DXIL":{"Stages":1,"VertexInputs":[
            {"Semantic":"POSITION","SemanticIndex":0,"Location":3,"ComponentKind":"Sint","ComponentCount":3},
            {"Semantic":"COLOR","SemanticIndex":0,"Location":4,"ComponentKind":"Uint","ComponentCount":4}],
            "ActiveBindings":[],"ActivePushConstants":[]},"SPIRV":null}})";
    shader::ShaderVariantInterfaceDesc decoded;
    ASSERT_TRUE(shader::ReadShaderVariantInterfaceJson(json, decoded));
    EXPECT_EQ(decoded.DXIL->VertexInputs[0].ComponentKind, shader::ShaderVertexComponentType::SignedInteger);
    EXPECT_EQ(decoded.DXIL->VertexInputs[1].ComponentKind, shader::ShaderVertexComponentType::UnsignedInteger);
    const auto encoded = shader::WriteShaderVariantInterfaceJson(decoded);
    ASSERT_TRUE(encoded);
    const auto document = JsonDocument::Parse(*encoded);
    ASSERT_TRUE(document);
    EXPECT_EQ(document->Root()["Kind"].AsString(), "ShaderVariantInterface");
    EXPECT_NE(encoded->find("Sint"), string::npos);
    EXPECT_NE(encoded->find("Uint"), string::npos);
}

TEST(ShaderLayout, OptionalTargetCanBeOmittedOrExplicitlyNull) {
    shader::ShaderVariantInterfaceDesc program{.Name = "Program", .DXIL = shader::ShaderVariantTargetInterfaceDesc{.Stages = gpu::ShaderStage::Compute}};
    const auto json = shader::WriteShaderVariantInterfaceJson(program);
    ASSERT_TRUE(json);
    EXPECT_EQ(json->find("SPIRV"), string::npos);
    shader::ShaderVariantInterfaceDesc decoded;
    ASSERT_TRUE(shader::ReadShaderVariantInterfaceJson(*json, decoded));
    auto withNull = *json;
    const auto name = withNull.find("\"Name\"");
    ASSERT_NE(name, string::npos);
    withNull.insert(name, "\"SPIRV\": null,");
    ASSERT_TRUE(shader::ReadShaderVariantInterfaceJson(withNull, decoded));
    EXPECT_FALSE(decoded.SPIRV);
    EXPECT_EQ(shader::WriteShaderVariantInterfaceJson(decoded), json);
}

TEST(ShaderLayout, MalformedReadsPreserveOutput) {
    auto output = MatrixLayout();
    const auto original = shader::WriteShaderPipelineLayoutBinary(output);
    ASSERT_TRUE(original);
    auto corrupt = *original;
    corrupt[4] = byte{2};
    EXPECT_FALSE(shader::ReadShaderPipelineLayoutBinary(corrupt, output));
    EXPECT_EQ(shader::WriteShaderPipelineLayoutBinary(output), original);
    EXPECT_FALSE(shader::ReadShaderPipelineLayoutBinary(std::span{*original}.first(original->size() - 1), output));
    EXPECT_FALSE(shader::ReadShaderPipelineLayoutJson("{\"Version\":1,\"Kind\":\"ShaderPipelineLayout\",\"Data\":{}}", output));
    EXPECT_EQ(shader::WriteShaderPipelineLayoutBinary(output), original);
}

TEST(ShaderLayout, VariantJsonRejectsMislabeledVariantData) {
    const auto json = shader::WriteShaderVariantInterfaceJson({
        .Name = "Candidate", .DXIL = shader::ShaderVariantTargetInterfaceDesc{.Stages = gpu::ShaderStage::Compute}});
    ASSERT_TRUE(json);
    constexpr std::string_view kind = "ShaderVariantInterface";
    const auto pos = json->find(kind);
    ASSERT_NE(pos, string::npos);
    for (const auto* invalidKind : {"ShaderProgramInterface", "ShaderFamilyLayout"}) {
        auto mislabeled = *json;
        mislabeled.replace(pos, kind.size(), invalidKind);
        shader::ShaderVariantInterfaceDesc output{.Name = "Original"};
        EXPECT_FALSE(shader::ReadShaderVariantInterfaceJson(mislabeled, output)) << invalidKind;
        EXPECT_EQ(output.Name, "Original");
    }
}

TEST(ShaderLayout, ActiveBindingMustExistOnItsTarget) {
    auto desc = MatrixLayout();
    desc.Groups[0].Bindings[0].Binding.Vulkan.reset();
    shader::ShaderVariantInterfaceDesc program{.Name = "Main", .SPIRV = shader::ShaderVariantTargetInterfaceDesc{.Stages = gpu::ShaderStage::Compute, .ActiveBindings = {{2, 7}}}};
    EXPECT_FALSE(shader::ValidateShaderVariantInterfaceDesc(program, &desc));
}



TEST(ShaderLayout, RejectsNativePositionConflictsAndOverflow) {
    auto desc = ResourceLayout();
    desc.Groups[0].Bindings[1].Binding.Vulkan = desc.Groups[0].Bindings[0].Binding.Vulkan;
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(desc));
    desc = ResourceLayout();
    auto other = desc.Groups[0];
    other.Name = "Other";
    other.GroupSlot = 1;
    other.Bindings.erase(other.Bindings.begin());
    desc.Groups[0].Bindings.pop_back();
    desc.Groups.push_back(std::move(other));
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(desc));
    desc = ResourceLayout();
    desc.Groups[0].Bindings[0].Binding.D3D12->Register = std::numeric_limits<uint32_t>::max();
    desc.Groups[0].Bindings[0].Binding.Count = 2;
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(desc));
    desc = MatrixLayout();
    desc.Types[1].ElementCount = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(shader::ValidateShaderPipelineLayoutDesc(desc));
}

TEST(ShaderLayout, BinaryRejectsEveryTruncatedPrefixAndExcessiveCountsWithoutChangingOutput) {
    const auto expected = MatrixLayout();
    const auto bytes = shader::WriteShaderPipelineLayoutBinary(expected);
    ASSERT_TRUE(bytes);
    auto output = expected;
    for (size_t count = 0; count < bytes->size(); ++count) {
        EXPECT_FALSE(shader::ReadShaderPipelineLayoutBinary(std::span{*bytes}.first(count), output));
    }
    auto corrupted = *bytes;
    std::fill(corrupted.begin() + 12, corrupted.begin() + 16, byte{0xff});
    EXPECT_FALSE(shader::ReadShaderPipelineLayoutBinary(corrupted, output));
    EXPECT_EQ(shader::WriteShaderPipelineLayoutBinary(output), bytes);
}


}  // namespace
}  // namespace radray
