#include <algorithm>
#include <filesystem>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/file.h>
#include <radray/guid.h>
#include <radray/shader/shader_binary.h>

using namespace radray;
using namespace radray::shader;

namespace {

ShaderReflectionRecord MakeReflection(ShaderTarget target) {
    ShaderReflectionDesc reflection;
    std::optional<string> payload;
    if (target == ShaderTarget::DXIL) {
        HlslShaderDesc value;
        value.Creator = "test";
        payload = SerializeHlslShaderDesc(value);
        reflection = std::move(value);
    } else {
        SpirvShaderDesc value;
        payload = SerializeSpirvShaderDesc(value);
        reflection = std::move(value);
    }
    EXPECT_TRUE(payload.has_value());
    return ShaderReflectionRecord{
        .Target = target,
        .Reflection = std::move(reflection),
        .Hash = HashShaderBytes(std::as_bytes(std::span{payload->data(), payload->size()}))};
}

ShaderStageInterfaceDesc MakeStageInterface(ShaderTarget target) {
    ShaderStageInterfaceBuildResult result;
    if (target == ShaderTarget::DXIL) {
        result = NormalizeHlslInterface(HlslShaderDesc{}, ShaderStage::Vertex);
    } else {
        result = NormalizeSpirvInterface(SpirvShaderDesc{}, ShaderStage::Vertex);
    }
    EXPECT_TRUE(result.Succeeded());
    return std::move(*result.Interface);
}

ShaderStageArtifact MakeArtifact(
    ShaderTarget target,
    vector<string> defines,
    byte marker,
    uint32_t reflectionIndex) {
    ShaderStageArtifact result{
        .Target = target,
        .Category = GetShaderBlobCategory(target),
        .PassIndex = 0,
        .Stage = ShaderStage::Vertex,
        .Defines = std::move(defines),
        .EntryPoint = "VSMain",
        .Bytecode = {marker, byte{0x12}, byte{0x34}},
        .ReflectionIndex = reflectionIndex,
        .InterfaceIndex = 0};
    result.BinaryHash = HashShaderBytes(result.Bytecode);
    return result;
}

ShaderBinary MakeBinary() {
    ShaderBinary result;
    result.Asset.AssetId = Guid::NewGuid();
    ShaderPassDesc pass;
    pass.Name = "BinaryTest";
    pass.SourcePath = "forward_pipeline/error_pass.hlsl";
    pass.SourceIdentity = {0x12345678u, 0xabcdef01u};
    pass.VariantDomain.KeywordGroups = {ShaderKeywordGroupDesc{
        .Alternatives = {"", "USE_TEST=1"},
        .Stages = ShaderStage::Vertex}};
    pass.BakeSet.Variants = {ShaderVariantKey{}, ShaderVariantKey{{"USE_TEST=1"}}};
    std::get<ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    result.Asset.Passes.emplace_back(std::move(pass));

    const ShaderStageInterfaceDesc dxilInterface = MakeStageInterface(ShaderTarget::DXIL);
    const ShaderStageInterfaceDesc spirvInterface = MakeStageInterface(ShaderTarget::SPIRV);
    EXPECT_EQ(dxilInterface, spirvInterface);
    result.StageInterfaces = {dxilInterface};
    auto program = MergeGraphicsStageInterfaces(dxilInterface);
    EXPECT_TRUE(program.Succeeded());
    result.ProgramInterfaces = {std::move(*program.Interface)};
    result.Reflections = {MakeReflection(ShaderTarget::DXIL), MakeReflection(ShaderTarget::SPIRV)};
    result.StageArtifacts = {
        MakeArtifact(ShaderTarget::DXIL, {}, byte{0x01}, 0),
        MakeArtifact(ShaderTarget::DXIL, {"USE_TEST=1"}, byte{0x02}, 0),
        MakeArtifact(ShaderTarget::SPIRV, {}, byte{0x03}, 1),
        MakeArtifact(ShaderTarget::SPIRV, {"USE_TEST=1"}, byte{0x04}, 1)};
    result.ProgramVariants = {
        ShaderProgramVariantArtifact{.Target = ShaderTarget::DXIL, .StageArtifactIndices = {0}, .InterfaceIndex = 0},
        ShaderProgramVariantArtifact{
            .Target = ShaderTarget::DXIL,
            .Defines = {"USE_TEST=1"},
            .StageArtifactIndices = {1},
            .InterfaceIndex = 0},
        ShaderProgramVariantArtifact{.Target = ShaderTarget::SPIRV, .StageArtifactIndices = {2}, .InterfaceIndex = 0},
        ShaderProgramVariantArtifact{
            .Target = ShaderTarget::SPIRV,
            .Defines = {"USE_TEST=1"},
            .StageArtifactIndices = {3},
            .InterfaceIndex = 0}};
    return result;
}

void RemoveSpirvPartition(ShaderBinary& binary) {
    std::erase_if(binary.ProgramVariants, [](const ShaderProgramVariantArtifact& value) {
        return value.Target == ShaderTarget::SPIRV;
    });
    std::erase_if(binary.StageArtifacts, [](const ShaderStageArtifact& value) {
        return value.Target == ShaderTarget::SPIRV;
    });
    std::erase_if(binary.Reflections, [](const ShaderReflectionRecord& value) {
        return value.Target == ShaderTarget::SPIRV;
    });
}

}  // namespace

TEST(ShaderBinaryTest, RoundTripsDeterministicallyAndFindsTargets) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_shader_binary_{}", Guid::NewGuid());
    const std::filesystem::path firstPath = directory / "first.bin";
    const std::filesystem::path secondPath = directory / "second.bin";
    ShaderBinary source = MakeBinary();
    ASSERT_TRUE(source.IsValid());
    EXPECT_TRUE(source.IsBakeComplete(ShaderTarget::DXIL));
    EXPECT_TRUE(source.IsBakeComplete(ShaderTarget::SPIRV));
    ASSERT_TRUE(WriteShaderBinary(firstPath, source));
    const auto firstBytes = ReadBinaryFile(firstPath);
    ASSERT_TRUE(firstBytes.has_value());

    auto loaded = ReadShaderBinary(firstPath);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Asset, source.Asset);
    ASSERT_EQ(loaded->StageArtifacts.size(), 4u);
    auto spirv = loaded->FindStageArtifact(
        ShaderTarget::SPIRV, 0, ShaderStage::Vertex, {"USE_TEST=1"});
    ASSERT_TRUE(spirv.HasValue());
    EXPECT_EQ(spirv.Get()->Category, ShaderBlobCategory::SPIRV);
    EXPECT_TRUE(loaded->GetReflection(*spirv.Get()).HasValue());
    EXPECT_FALSE(loaded->FindStageArtifact(ShaderTarget::DXIL, 0, ShaderStage::Pixel, {}).HasValue());

    std::ranges::reverse(loaded->ProgramVariants);
    ASSERT_TRUE(WriteShaderBinary(firstPath, *loaded));
    EXPECT_EQ(ReadBinaryFile(firstPath), firstBytes);
    ASSERT_TRUE(WriteShaderBinary(secondPath, *loaded));
    EXPECT_EQ(ReadBinaryFile(firstPath), ReadBinaryFile(secondPath));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(ShaderBinaryTest, SeparatesStructuralValidityFromBakeCompleteness) {
    ShaderBinary source = MakeBinary();
    RemoveSpirvPartition(source);
    ASSERT_TRUE(source.IsValid());
    EXPECT_TRUE(source.IsBakeComplete(ShaderTarget::DXIL));
    EXPECT_FALSE(source.IsBakeComplete(ShaderTarget::SPIRV));

    source.ProgramVariants.erase(source.ProgramVariants.begin() + 1);
    source.StageArtifacts.erase(source.StageArtifacts.begin() + 1);
    ASSERT_TRUE(source.IsValid());
    EXPECT_FALSE(source.IsBakeComplete(ShaderTarget::DXIL));

    source = MakeBinary();
    source.StageArtifacts.front().Target = static_cast<ShaderTarget>(2);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.StageArtifacts.front().ReflectionIndex = 1;
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().SM = static_cast<HlslShaderModel>(-1);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().VariantDomain.KeywordGroups.front().Scope = static_cast<ShaderKeywordScope>(2);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().BakeSet.Variants.erase(
        source.Asset.Passes.front().BakeSet.Variants.begin());
    EXPECT_FALSE(source.IsValid());
}

TEST(ShaderBinaryTest, RejectsCorruptionTruncationAndDanglingRecords) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_shader_binary_invalid_{}", Guid::NewGuid());
    const std::filesystem::path path = directory / "shader.bin";
    ShaderBinary source = MakeBinary();
    ASSERT_TRUE(WriteShaderBinary(path, source));
    auto valid = ReadBinaryFile(path);
    ASSERT_TRUE(valid.has_value());

    vector<byte> corrupt = *valid;
    corrupt.back() ^= byte{0xff};
    ASSERT_TRUE(WriteBinaryFile(path, corrupt));
    EXPECT_FALSE(ReadShaderBinary(path).has_value());

    vector<byte> truncated{valid->begin(), valid->begin() + valid->size() / 2};
    ASSERT_TRUE(WriteBinaryFile(path, truncated));
    EXPECT_FALSE(ReadShaderBinary(path).has_value());

    vector<byte> badVersion = *valid;
    badVersion[8] ^= byte{0x7f};
    ASSERT_TRUE(WriteBinaryFile(path, badVersion));
    EXPECT_FALSE(ReadShaderBinary(path).has_value());

    source.ProgramVariants.emplace_back(source.ProgramVariants.front());
    EXPECT_FALSE(source.IsValid());
    EXPECT_FALSE(WriteShaderBinary(path, source));

    source = MakeBinary();
    source.ProgramVariants.front().StageArtifactIndices.front() = kInvalidShaderTableIndex;
    EXPECT_FALSE(source.IsValid());
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
