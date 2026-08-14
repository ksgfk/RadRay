#include <radray/shader_compiler/client.h>

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

namespace radray::shader_compiler {
namespace {

vector<byte> ReadBytes(const std::filesystem::path& path) {
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
    vector<byte> result(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(result.data()), size);
    return file.good() || file.eof() ? result : vector<byte>{};
}

std::filesystem::path ShaderlibRoot() {
    return std::filesystem::path{RADRAY_PROJECT_DIR} / "shaderlib";
}

TEST(RadRayShaderLibPass, ProductPassesCompileAsAtomicTwoTargetVariants) {
    struct PassCase {
        std::string_view SourceName;
        std::string_view RelativePath;
        shader::ShaderKind Kind;
        size_t EntryCount;
        size_t BindingCount;
        uint32_t DxilGroup;
        uint32_t DxilBindings[2];
        uint32_t SpirvGroup;
        uint32_t SpirvBindings[2];
        std::string_view AssignmentName;
        std::string_view AssignmentValue;
    };
    constexpr PassCase cases[] = {
        {"passes/forward.hlsl", "passes/forward.hlsl", shader::ShaderKind::Graphics, 2, 2, 0, {0, 0}, 2, {6, 7}, "QUALITY", "low"},
        {"passes/depth.hlsl", "passes/depth.hlsl", shader::ShaderKind::Graphics, 1, 0, 0, {0, 0}, 0, {0, 0}, "DEPTH_MODE", "regular"},
        {"passes/compute.hlsl", "passes/compute.hlsl", shader::ShaderKind::Compute, 1, 1, 0, {0, 0}, 2, {6, 0}, "COMPUTE_MODE", "clear"}};

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const vector<std::filesystem::path> includePaths{ShaderlibRoot()};
    for (const PassCase& pass : cases) {
        const vector<byte> source = ReadBytes(ShaderlibRoot() / pass.RelativePath);
        ASSERT_FALSE(source.empty()) << pass.SourceName;
        const DiscoveryResult discovery = client.DiscoverSourceContract(
            shader::SourceContractRequest{
                .SourceName = string{pass.SourceName},
                .RootSource = source,
                .Defines = {},
                .Targets = shader::ShaderTargetMask::All,
                .Policy = {}},
            includePaths);
        ASSERT_TRUE(discovery.Succeeded())
            << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);
        EXPECT_EQ(discovery.Contract.Kind, pass.Kind);
        EXPECT_EQ(discovery.Contract.EntryPoints.size(), pass.EntryCount);

        shader::CompileVariantRequest request{
            .SourceName = string{pass.SourceName},
            .RootSource = source,
            .Defines = {},
            .Assignments = {{string{pass.AssignmentName}, string{pass.AssignmentValue}}},
            .Targets = shader::ShaderTargetMask::All,
            .ExpectedContract = discovery.Contract.Hash};
        const shader::CompileVariantResult result = client.CompileVariant(request, includePaths);
        ASSERT_EQ(result.Status, shader::CompileStatus::Success)
            << (result.Diagnostics.empty() ? "" : result.Diagnostics.back().Message);
        ASSERT_EQ(result.Lanes.size(), 2u);
        for (const shader::CompileTargetLane& lane : result.Lanes) {
            EXPECT_EQ(lane.Stages.size(), pass.EntryCount);
            EXPECT_FALSE(lane.Bytecode.empty());
            ASSERT_GE(lane.Metadata.size(), sizeof(shader::WireMetadataEnvelope));
            shader::WireMetadataEnvelope envelope{};
            std::memcpy(&envelope, lane.Metadata.data(), sizeof(envelope));
            EXPECT_EQ(envelope.BindingRecords.Size, pass.BindingCount * sizeof(shader::WireBindingRecord));
            vector<shader::WireBindingRecord> bindings(pass.BindingCount);
            if (!bindings.empty()) {
                std::memcpy(
                    bindings.data(),
                    lane.Metadata.data() + envelope.BindingRecords.Offset,
                    envelope.BindingRecords.Size);
            }
            const uint32_t expectedGroup =
                lane.Target == shader::ShaderTarget::SPIRV ? pass.SpirvGroup : pass.DxilGroup;
            for (size_t index = 0; index < bindings.size(); ++index) {
                EXPECT_EQ(bindings[index].Group, expectedGroup);
                const uint32_t expectedBinding = lane.Target == shader::ShaderTarget::SPIRV
                                                     ? pass.SpirvBindings[index]
                                                     : pass.DxilBindings[index];
                EXPECT_EQ(bindings[index].Binding, expectedBinding);
                EXPECT_EQ(
                    bindings[index].StageMask,
                    1u << static_cast<uint8_t>(
                              pass.Kind == shader::ShaderKind::Compute ? shader::ShaderStage::Compute
                                                                        : shader::ShaderStage::Pixel));
            }
            EXPECT_TRUE(shader::ValidateWireMetadataEnvelope(
                lane.Metadata,
                lane.Target,
                envelope.GpuArtifact));
        }
    }
}

TEST(RadRayShaderLibPass, ProductPassesDeclareBothTargetBindingsExplicitly) {
    struct BindingCase {
        std::string_view Name;
        bool HasResources;
    };
    constexpr BindingCase cases[] = {
        {"forward.hlsl", true},
        {"depth.hlsl", false},
        {"compute.hlsl", true}};

    const std::filesystem::path root = ShaderlibRoot() / "passes";
    for (const BindingCase& pass : cases) {
        const vector<byte> source = ReadBytes(root / pass.Name);
        ASSERT_FALSE(source.empty()) << pass.Name;
        const string text{
            reinterpret_cast<const char*>(source.data()),
            source.size()};
        if (pass.HasResources) {
            EXPECT_NE(text.find("register("), string::npos) << pass.Name;
            EXPECT_NE(text.find("VK_BINDING("), string::npos) << pass.Name;
        } else {
            EXPECT_EQ(text.find("register("), string::npos) << pass.Name;
            EXPECT_EQ(text.find("VK_BINDING("), string::npos) << pass.Name;
        }
        EXPECT_EQ(text.find("RADRAY_FORWARD_"), string::npos) << pass.Name;
        EXPECT_NE(text.find("#include <core/platform.hlsli>"), string::npos) << pass.Name;
    }
}

}  // namespace
}  // namespace radray::shader_compiler
