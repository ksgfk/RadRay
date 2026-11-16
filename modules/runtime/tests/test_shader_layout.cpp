#include <gtest/gtest.h>
#include <radray/binary_io.h>
#include "test_asset_fakes.h"

namespace radray {
namespace {
using namespace test;
TEST(ShaderLayout, MatrixArraySpanAndIndexedQueryExcludeTrailingPadding) {
    const auto desc = MatrixLayout();
    string error;
    ASSERT_TRUE(ValidateShaderPipelineLayoutDesc(desc, &error)) << error;
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
    EXPECT_FALSE(ValidateShaderPipelineLayoutDesc(desc));
}

TEST(ShaderLayout, RejectsCyclesThroughUnderlyingTypeReferences) {
    ShaderPipelineLayoutDesc desc;
    desc.Types.push_back({.Name = "Recursive", .Size = 16, .ElementSize = 16});
    desc.Types.push_back({.Name = "Self", .ParentIndex = 0, .Size = 16, .ElementSize = 16, .TypeIndex = 0});
    string error;
    EXPECT_FALSE(ValidateShaderPipelineLayoutDesc(desc, &error));
    EXPECT_NE(error.find("cyclic"), string::npos);
}

TEST(ShaderLayout, FamilyRoundTripsJsonAndBinaryWithTargetInterfaces) {
    ShaderFamilyLayoutDesc family{.SharedLayout = MatrixLayout()};
    ShaderTargetProgramInterfaceDesc target{.Stages = render::ShaderStage::Compute, .ActiveBindings = {{2, 7}}};
    family.Variants.push_back({.Name = "Default", .DXIL = target, .SPIRV = target});
    const auto json = WriteShaderFamilyLayoutJson(family);
    const auto binary = WriteShaderFamilyLayoutBinary(family);
    ASSERT_TRUE(json && binary);
    ShaderFamilyLayoutDesc decodedJson, decodedBinary;
    ASSERT_TRUE(ReadShaderFamilyLayoutJson(*json, decodedJson));
    ASSERT_TRUE(ReadShaderFamilyLayoutBinary(*binary, decodedBinary));
    EXPECT_EQ(WriteShaderFamilyLayoutBinary(decodedJson), binary);
    EXPECT_EQ(WriteShaderFamilyLayoutBinary(decodedBinary), binary);
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
        ShaderProgramInterfaceDesc decoded;
        ASSERT_TRUE(ReadShaderProgramInterfaceBinary(bytes, decoded));
        ASSERT_TRUE(decoded.DXIL);
        EXPECT_EQ(decoded.DXIL->VertexInputs[0].ComponentKind,
                  static_cast<shader::ShaderVertexComponentType>(wireKind + 1));
        EXPECT_EQ(WriteShaderProgramInterfaceBinary(decoded), bytes);
    }
}

TEST(ShaderLayout, VersionOneJsonKeepsLegacyIntegerNames) {
    const std::string_view json = R"({"Version":1,"Kind":"ShaderProgramInterface","Data":{
        "Name":"Legacy","DXIL":{"Stages":1,"VertexInputs":[
            {"Semantic":"POSITION","SemanticIndex":0,"Location":3,"ComponentKind":"Sint","ComponentCount":3},
            {"Semantic":"COLOR","SemanticIndex":0,"Location":4,"ComponentKind":"Uint","ComponentCount":4}],
            "ActiveBindings":[],"ActivePushConstants":[]},"SPIRV":null}})";
    ShaderProgramInterfaceDesc decoded;
    ASSERT_TRUE(ReadShaderProgramInterfaceJson(json, decoded));
    EXPECT_EQ(decoded.DXIL->VertexInputs[0].ComponentKind, shader::ShaderVertexComponentType::SignedInteger);
    EXPECT_EQ(decoded.DXIL->VertexInputs[1].ComponentKind, shader::ShaderVertexComponentType::UnsignedInteger);
    const auto encoded = WriteShaderProgramInterfaceJson(decoded);
    ASSERT_TRUE(encoded);
    EXPECT_NE(encoded->find("Sint"), string::npos);
    EXPECT_NE(encoded->find("Uint"), string::npos);
}

TEST(ShaderLayout, MalformedReadsPreserveOutput) {
    auto output = MatrixLayout();
    const auto original = WriteShaderPipelineLayoutBinary(output);
    ASSERT_TRUE(original);
    auto corrupt = *original;
    corrupt[4] = byte{2};
    EXPECT_FALSE(ReadShaderPipelineLayoutBinary(corrupt, output));
    EXPECT_EQ(WriteShaderPipelineLayoutBinary(output), original);
    EXPECT_FALSE(ReadShaderPipelineLayoutBinary(std::span{*original}.first(original->size() - 1), output));
    EXPECT_FALSE(ReadShaderPipelineLayoutJson("{\"Version\":1,\"Kind\":\"ShaderPipelineLayout\",\"Data\":{}}", output));
    EXPECT_EQ(WriteShaderPipelineLayoutBinary(output), original);
}

TEST(ShaderLayout, ActiveBindingMustExistOnItsTarget) {
    auto desc = MatrixLayout();
    desc.Groups[0].Bindings[0].Binding.Vulkan.reset();
    ShaderProgramInterfaceDesc program{.Name = "Main", .SPIRV = ShaderTargetProgramInterfaceDesc{.Stages = render::ShaderStage::Compute, .ActiveBindings = {{2, 7}}}};
    EXPECT_FALSE(ValidateShaderProgramInterfaceDesc(program, &desc));
}

TEST(ShaderVertexInput, ResolvesVariantLocationsWithoutGpuStreams) {
    VertexInput mesh;
    mesh.Layouts = {{0, 12, render::VertexStepMode::Vertex}};
    mesh.Attributes = {{0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3},
                       {0, 0, "NORMAL", 0, render::VertexFormat::FLOAT32X3}};
    ASSERT_TRUE(ValidateVertexInputLayout(mesh));
    const array signature{ShaderVertexAttributeSignatureDesc{"NORMAL", 0, 5, shader::ShaderVertexComponentType::Float, 3}};
    ResolvedVertexInputState result;
    ASSERT_TRUE(ResolveVertexInputState(mesh, signature, result));
    ASSERT_EQ(result.Attributes.size(), 1u);
    EXPECT_EQ(result.Attributes[0].Semantic, "NORMAL");
    EXPECT_EQ(result.Attributes[0].Location, 5u);
    auto invalid = signature;
    invalid[0].ComponentCount = 4;
    EXPECT_FALSE(ResolveVertexInputState(mesh, invalid, result));
    EXPECT_EQ(result.Attributes[0].Location, 5u);
    invalid[0].ComponentCount = 3;
    invalid[0].ComponentKind = shader::ShaderVertexComponentType::UnsignedInteger;
    EXPECT_FALSE(ResolveVertexInputState(mesh, invalid, result));
    mesh.Layouts[0].ArrayStride = 0;
    EXPECT_FALSE(ValidateVertexInputLayout(mesh));
}

TEST(ShaderLayout, RejectsNativePositionConflictsAndOverflow) {
    auto desc = ResourceLayout();
    desc.Groups[0].Bindings[1].Binding.Vulkan = desc.Groups[0].Bindings[0].Binding.Vulkan;
    EXPECT_FALSE(ValidateShaderPipelineLayoutDesc(desc));
    desc = ResourceLayout();
    auto other = desc.Groups[0];
    other.Name = "Other";
    other.GroupSlot = 1;
    other.Bindings.erase(other.Bindings.begin());
    desc.Groups[0].Bindings.pop_back();
    desc.Groups.push_back(std::move(other));
    EXPECT_FALSE(ValidateShaderPipelineLayoutDesc(desc));
    desc = ResourceLayout();
    desc.Groups[0].Bindings[0].Binding.D3D12->Register = std::numeric_limits<uint32_t>::max();
    desc.Groups[0].Bindings[0].Binding.Count = 2;
    EXPECT_FALSE(ValidateShaderPipelineLayoutDesc(desc));
    desc = MatrixLayout();
    desc.Types[1].ElementCount = std::numeric_limits<uint32_t>::max();
    EXPECT_FALSE(ValidateShaderPipelineLayoutDesc(desc));
}

TEST(ShaderLayout, BinaryRejectsEveryTruncatedPrefixAndExcessiveCountsWithoutChangingOutput) {
    const auto expected = MatrixLayout();
    const auto bytes = WriteShaderPipelineLayoutBinary(expected);
    ASSERT_TRUE(bytes);
    auto output = expected;
    for (size_t count = 0; count < bytes->size(); ++count) {
        EXPECT_FALSE(ReadShaderPipelineLayoutBinary(std::span{*bytes}.first(count), output));
    }
    auto corrupted = *bytes;
    std::fill(corrupted.begin() + 12, corrupted.begin() + 16, byte{0xff});
    EXPECT_FALSE(ReadShaderPipelineLayoutBinary(corrupted, output));
    EXPECT_EQ(WriteShaderPipelineLayoutBinary(output), bytes);
}

TEST(ShaderVertexInput, EmptyVertexSignatureProducesNoAttributes) {
    VertexInput mesh;
    mesh.Layouts = {{0, 12, render::VertexStepMode::Vertex}};
    mesh.Attributes = {{0, 0, "POSITION", 0, render::VertexFormat::FLOAT32X3}};
    ShaderTargetProgramInterfaceDesc interface;
    interface.Stages = render::ShaderStage::Vertex;
    ResolvedVertexInputState result;
    EXPECT_TRUE(PrepareMaterialVertexInput(interface, mesh, result));
    EXPECT_TRUE(result.Attributes.empty());
    interface.Stages = render::ShaderStage::Compute;
    EXPECT_FALSE(PrepareMaterialVertexInput(interface, mesh, result));
}
}  // namespace
}  // namespace radray
