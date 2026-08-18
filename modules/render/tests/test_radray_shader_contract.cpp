#include "shader_contract_fixtures.h"

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace radray::render {
namespace {

std::filesystem::path ShaderFixtureRoot() {
    return std::filesystem::path{RADRAY_PROJECT_DIR} /
           "modules/render/tests/data/shader_sources";
}

std::filesystem::path ShaderArtifactRoot() {
    return std::filesystem::path{RADRAY_PROJECT_DIR} /
           "modules/render/tests/data/shader_artifacts";
}

vector<byte> ReadBinary(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return {};
    }
    file.seekg(0, std::ios::beg);
    vector<byte> data(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(data.data()), size);
    return file.good() || file.eof() ? data : vector<byte>{};
}

shader::GpuArtifactHash ExpectedArtifactHash(uint8_t fixtureIndex, shader::ShaderTarget target) {
    return test::ExpectedGpuArtifact(fixtureIndex, target);
}

TEST(RadRayShaderContract, HashIsLittleEndianPod) {
    static_assert(sizeof(shader::Hash128) == 16);
    static_assert(std::is_trivially_copyable_v<shader::Hash128>);

    shader::Hash128 value{};
    for (uint8_t index = 0; index < value.Bytes.size(); ++index) {
        value.Bytes[index] = index;
    }
    EXPECT_EQ(value.ToHex(), "000102030405060708090a0b0c0d0e0f");
}

TEST(RadRayShaderContract, CanonicalRequestIsStable) {
    shader::CompileVariantRequest request{
        .SourceName = "fixtures/texture_sampler.hlsl",
        .RootSource = {},
        .Defines = {{"USE_SHADOWS", "1"}, {"DEBUG_VIEW", "0"}},
        .Assignments = {{"QUALITY", "high"}, {"ALPHA_TEST", "off"}},
        .Targets = shader::ShaderTargetMask::All};

    const auto first = shader::EncodeCanonicalCompileVariantRequest(request);
    ASSERT_TRUE(first.has_value());

    std::swap(request.Defines[0], request.Defines[1]);
    std::swap(request.Assignments[0], request.Assignments[1]);
    const auto reordered = shader::EncodeCanonicalCompileVariantRequest(request);
    ASSERT_TRUE(reordered.has_value());
    EXPECT_EQ(*first, *reordered);

    request.SourceName = "moved/texture_sampler.hlsl";
    const auto moved = shader::EncodeCanonicalCompileVariantRequest(request);
    ASSERT_TRUE(moved.has_value());
    EXPECT_NE(*first, *moved);

    request.SourceName = "C:/shaderlib/texture_sampler.hlsl";
    EXPECT_FALSE(shader::EncodeCanonicalCompileVariantRequest(request).has_value());

    request.SourceName = "fixtures/texture_sampler.hlsl";
    request.Defines.push_back({"DEBUG_VIEW", "1"});
    EXPECT_FALSE(shader::EncodeCanonicalCompileVariantRequest(request).has_value());
}

TEST(RadRayShaderContract, FixtureTablePinsTopologyAndTargetFacts) {
    const auto fixtures = test::GetShaderContractFixtures();
    ASSERT_EQ(fixtures.size(), 12u);

    std::error_code error;
    for (const test::ShaderContractFixture& fixture : fixtures) {
        const auto path = ShaderFixtureRoot() / fixture.SourcePath.substr(std::string_view{"fixtures/"}.size());
        ASSERT_TRUE(std::filesystem::is_regular_file(path, error)) << path.string();
        ASSERT_EQ(error.value(), 0) << path.string();
        ASSERT_GT(std::filesystem::file_size(path, error), 0u) << path.string();
        ASSERT_EQ(error.value(), 0) << path.string();
        ASSERT_FALSE(fixture.Entries.empty());
        if (fixture.Kind == shader::ShaderKind::Compute) {
            ASSERT_EQ(fixture.Entries.size(), 1u);
            EXPECT_EQ(fixture.Entries.front().Stage, shader::ShaderStage::Compute);
        }
    }

    const test::ShaderContractFixture* texture = nullptr;
    const test::ShaderContractFixture* compute = nullptr;
    for (const test::ShaderContractFixture& fixture : fixtures) {
        if (fixture.Name == "texture_sampler") {
            texture = &fixture;
        } else if (fixture.Name == "compute") {
            compute = &fixture;
        }
    }
    ASSERT_NE(texture, nullptr);
    ASSERT_NE(compute, nullptr);
    ASSERT_EQ(texture->Bindings.size(), 2u);
    EXPECT_NE(texture->Bindings[0].D3D12Binding, texture->Bindings[0].VulkanBinding);
    EXPECT_NE(texture->Bindings[0].D3D12Group, texture->Bindings[0].VulkanSet);
    EXPECT_EQ(compute->Kind, shader::ShaderKind::Compute);
    EXPECT_EQ(compute->Bindings.front().Kind, test::FixtureResourceKind::StorageBuffer);
}

TEST(RadRayShaderContract, MetadataEnvelopeFailsClosed) {
    shader::WireMetadataEnvelope envelope{};
    envelope.Target = static_cast<uint8_t>(shader::ShaderTarget::DXIL);
    envelope.TotalSize = sizeof(shader::WireMetadataEnvelope) + 4;
    envelope.Bytecode = {
        sizeof(shader::WireMetadataEnvelope),
        4};
    envelope.GpuArtifact.Bytes[0] = 0x42;

    vector<byte> blob(envelope.TotalSize, static_cast<byte>(0));
    std::memcpy(blob.data(), &envelope, sizeof(envelope));
    blob.back() = static_cast<byte>(0xaa);

    EXPECT_TRUE(shader::ValidateWireMetadataEnvelope(
        blob,
        shader::ShaderTarget::DXIL,
        envelope.GpuArtifact));

    auto corrupted = blob;
    auto* corruptedEnvelope = reinterpret_cast<shader::WireMetadataEnvelope*>(corrupted.data());
    corruptedEnvelope->Bytecode.Offset = corruptedEnvelope->TotalSize - 1;
    EXPECT_FALSE(shader::ValidateWireMetadataEnvelope(
        corrupted,
        shader::ShaderTarget::DXIL,
        envelope.GpuArtifact));

    EXPECT_FALSE(shader::ValidateWireMetadataEnvelope(
        blob,
        shader::ShaderTarget::SPIRV,
        envelope.GpuArtifact));

    auto truncatedCurrent = blob;
    truncatedCurrent.resize(sizeof(shader::WireMetadataEnvelope) - 1);
    EXPECT_FALSE(shader::ValidateWireMetadataEnvelope(
        truncatedCurrent,
        shader::ShaderTarget::DXIL,
        envelope.GpuArtifact));
}

TEST(RadRayShaderContract, RawGoldenArtifactsAreVersionedAndTargetSpecific) {
    const auto fixtures = test::GetShaderContractFixtures();
    const auto artifactRoot = ShaderArtifactRoot();
    const shader::ShaderTarget targets[] = {
        shader::ShaderTarget::DXIL,
        shader::ShaderTarget::SPIRV};

    for (uint8_t fixtureIndex = 0; fixtureIndex < fixtures.size(); ++fixtureIndex) {
        for (const shader::ShaderTarget target : targets) {
            const char* suffix = target == shader::ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin";
            const auto path = artifactRoot / (string{fixtures[fixtureIndex].Name} + suffix);
            const vector<byte> blob = ReadBinary(path);
            ASSERT_GE(blob.size(), sizeof(shader::WireMetadataEnvelope)) << path.string();
            EXPECT_TRUE(shader::ValidateWireMetadataEnvelope(
                blob,
                target,
                ExpectedArtifactHash(fixtureIndex, target)))
                << path.string();
        }
    }
}

}  // namespace
}  // namespace radray::render
