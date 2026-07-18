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

CompiledShaderStage MakeStage(
    ShaderTarget target,
    vector<string> defines,
    byte marker) {
    CompiledShaderStage result;
    result.Target = target;
    result.Category = GetShaderBlobCategory(target);
    result.PassIndex = 0;
    result.Stage = ShaderStage::Vertex;
    result.Defines = std::move(defines);
    result.EntryPoint = "VSMain";
    result.Bytecode = {marker, byte{0x12}, byte{0x34}};
    if (target == ShaderTarget::DXIL) {
        HlslShaderDesc reflection;
        reflection.Creator = "test";
        result.Reflection = reflection;
        result.ReflectionPayload = SerializeHlslShaderDesc(reflection).value();
    } else {
        SpirvShaderDesc reflection;
        reflection.ComputeInfo = SpirvComputeInfo{1, 1, 1};
        result.Reflection = reflection;
        result.ReflectionPayload = SerializeSpirvShaderDesc(reflection).value();
    }
    result.BinaryHash = HashShaderBytes(result.Bytecode);
    result.InterfaceHash = HashShaderBytes(std::as_bytes(std::span{
        result.ReflectionPayload.data(), result.ReflectionPayload.size()}));
    return result;
}

ShaderBinary MakeBinary() {
    ShaderBinary result;
    result.Asset.AssetId = Guid::NewGuid();
    ShaderPassDesc pass;
    pass.Name = "BinaryTest";
    pass.SourcePath = "forward_pipeline/error_pass.hlsl";
    pass.KeywordGroups = {ShaderKeywordGroupDesc{
        .Alternatives = {"", "USE_TEST=1"},
        .Stages = ShaderStage::Vertex}};
    pass.Variants = {ShaderVariantDesc{}, ShaderVariantDesc{{"USE_TEST=1"}}};
    std::get<ShaderGraphicsPassDesc>(pass.Program).VertexEntry = "VSMain";
    result.Asset.Passes.emplace_back(std::move(pass));
    result.Stages = {
        MakeStage(ShaderTarget::DXIL, {}, byte{0x01}),
        MakeStage(ShaderTarget::DXIL, {"USE_TEST=1"}, byte{0x02}),
        MakeStage(ShaderTarget::SPIRV, {}, byte{0x03}),
        MakeStage(ShaderTarget::SPIRV, {"USE_TEST=1"}, byte{0x04})};
    return result;
}

}  // namespace

TEST(ShaderBinaryTest, RoundTripsDeterministicallyAndFindsTargets) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / fmt::format("radray_shader_binary_{}", Guid::NewGuid());
    const std::filesystem::path firstPath = directory / "first.bin";
    const std::filesystem::path secondPath = directory / "second.bin";
    ShaderBinary source = MakeBinary();
    ASSERT_TRUE(source.IsValid());
    ASSERT_TRUE(WriteShaderBinary(firstPath, source));
    const auto firstBytes = ReadBinaryFile(firstPath);
    ASSERT_TRUE(firstBytes.has_value());

    auto loaded = ReadShaderBinary(firstPath);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->Asset, source.Asset);
    ASSERT_EQ(loaded->Stages.size(), 4u);
    auto spirv = loaded->Find(ShaderTarget::SPIRV, 0, ShaderStage::Vertex, {"USE_TEST=1"});
    ASSERT_TRUE(spirv.HasValue());
    EXPECT_EQ(spirv.Get()->Category, ShaderBlobCategory::SPIRV);
    EXPECT_TRUE(spirv.Get()->Reflection.has_value());
    EXPECT_FALSE(loaded->Find(ShaderTarget::DXIL, 0, ShaderStage::Pixel, {}).HasValue());

    std::ranges::reverse(loaded->Stages);
    ASSERT_TRUE(WriteShaderBinary(firstPath, *loaded));
    EXPECT_EQ(ReadBinaryFile(firstPath), firstBytes);
    ASSERT_TRUE(WriteShaderBinary(secondPath, *loaded));
    EXPECT_EQ(ReadBinaryFile(firstPath), ReadBinaryFile(secondPath));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}

TEST(ShaderBinaryTest, RequiresCompleteTargetPartitionsAndMatchingReflection) {
    ShaderBinary source = MakeBinary();
    EXPECT_TRUE(DoesShaderDefineAffectStage(
        source.Asset.Passes.front(),
        "USE_TEST=1",
        ShaderStage::Vertex));
    EXPECT_FALSE(DoesShaderDefineAffectStage(
        source.Asset.Passes.front(),
        "USE_TEST=1",
        ShaderStage::Pixel));
    source.Stages.erase(source.Stages.begin());
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    std::erase_if(source.Stages, [](const CompiledShaderStage& stage) noexcept {
        return stage.Target == ShaderTarget::SPIRV;
    });
    EXPECT_TRUE(source.IsValid());

    source = MakeBinary();
    source.Stages.front().Target = static_cast<ShaderTarget>(2);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    SpirvShaderDesc spirvReflection;
    spirvReflection.ComputeInfo = SpirvComputeInfo{1, 1, 1};
    source.Stages.front().Reflection = spirvReflection;
    source.Stages.front().ReflectionPayload = SerializeSpirvShaderDesc(spirvReflection).value();
    source.Stages.front().InterfaceHash = HashShaderBytes(std::as_bytes(std::span{
        source.Stages.front().ReflectionPayload.data(),
        source.Stages.front().ReflectionPayload.size()}));
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().SM = static_cast<HlslShaderModel>(-1);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().KeywordGroups.front().Scope = static_cast<ShaderKeywordScope>(2);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    std::get<ShaderGraphicsPassDesc>(source.Asset.Passes.front().Program).Cull = static_cast<CullMode>(3);
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().SourcePath.push_back(static_cast<char>(0xff));
    EXPECT_FALSE(source.IsValid());

    source = MakeBinary();
    source.Asset.Passes.front().Variants.erase(source.Asset.Passes.front().Variants.begin());
    std::erase_if(source.Stages, [](const CompiledShaderStage& stage) noexcept {
        return stage.Defines.empty();
    });
    EXPECT_FALSE(source.IsValid());
}

TEST(ShaderBinaryTest, RejectsCorruptionTruncationAndDuplicateRecords) {
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

    source.Stages.emplace_back(source.Stages.front());
    EXPECT_FALSE(source.IsValid());
    EXPECT_FALSE(WriteShaderBinary(path, source));
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
}
