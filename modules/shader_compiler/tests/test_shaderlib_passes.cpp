#include <radray/shader_compiler/client.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

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

TEST(RadRayShaderLibPass, PassesCompileAsAtomicTwoTargetVariants) {
    struct BindingFact {
        std::string_view Name;
        uint32_t DxilGroup;
        uint32_t DxilBinding;
        uint32_t SpirvGroup;
        uint32_t SpirvBinding;
        uint32_t StageMask;
        std::string_view PayloadType{};
    };
    struct PassCase {
        std::string_view SourceName;
        shader::ShaderKind Kind;
        size_t EntryCount;
        std::span<const BindingFact> Bindings;
        std::string_view AssignmentName;
        std::string_view AssignmentValue;
    };
    constexpr BindingFact forwardBindings[] = {
        {"ForwardView", 0, 0, 0, 0, 3, "ForwardViewData"},
        {"ForwardMaterial", 1, 0, 1, 0, 2, "ForwardMaterialData"},
        {"AlbedoTexture", 1, 0, 1, 1, 2},
        {"LinearSampler", 1, 0, 1, 2, 2},
        {"ForwardObject", 2, 0, 2, 0, 1, "ForwardObjectData"}};
    constexpr BindingFact computeBindings[] = {
        {"Output", 0, 0, 2, 6, 4}};
    constexpr BindingFact depthOnlyBindings[] = {
        {"ForwardView", 0, 0, 0, 0, 1, "ForwardDepthViewData"},
        {"ForwardObject", 2, 0, 2, 0, 1, "ForwardDepthObjectData"}};
    const PassCase cases[] = {
        {"shaderlib/pipelines/forward/forward.hlsl", shader::ShaderKind::Graphics, 2, forwardBindings, "QUALITY", "low"},
        {"modules/shader_compiler/tests/data/depth.hlsl", shader::ShaderKind::Graphics, 1, {}, "DEPTH_MODE", "regular"},
        {"shaderlib/pipelines/forward/depth_only.hlsl", shader::ShaderKind::Graphics, 1, depthOnlyBindings, "", ""},
        {"modules/shader_compiler/tests/data/compute.hlsl", shader::ShaderKind::Compute, 1, computeBindings, "COMPUTE_MODE", "clear"}};

    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const vector<std::filesystem::path> includePaths{ShaderlibRoot()};
    for (const PassCase& pass : cases) {
        const vector<byte> source = ReadBytes(std::filesystem::path{RADRAY_PROJECT_DIR} / pass.SourceName);
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
        if (pass.AssignmentName.empty()) request.Assignments.clear();
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
            EXPECT_EQ(envelope.BindingRecords.Size, pass.Bindings.size() * sizeof(shader::WireBindingRecord));
            vector<shader::WireBindingRecord> bindings(pass.Bindings.size());
            if (!bindings.empty()) {
                std::memcpy(
                    bindings.data(),
                    lane.Metadata.data() + envelope.BindingRecords.Offset,
                    envelope.BindingRecords.Size);
            }
            vector<shader::WireTypeRecord> types(
                envelope.TypeRecords.Size / sizeof(shader::WireTypeRecord));
            if (!types.empty()) {
                std::memcpy(
                    types.data(),
                    lane.Metadata.data() + envelope.TypeRecords.Offset,
                    envelope.TypeRecords.Size);
            }
            for (const BindingFact& expected : pass.Bindings) {
                const auto found = std::find_if(
                    bindings.begin(),
                    bindings.end(),
                    [&](const shader::WireBindingRecord& binding) {
                        const auto* name = reinterpret_cast<const char*>(
                            lane.Metadata.data() + binding.Name.Offset);
                        return std::string_view{name, binding.Name.Size} == expected.Name;
                    });
                ASSERT_NE(found, bindings.end()) << expected.Name;
                EXPECT_EQ(
                    found->Group,
                    lane.Target == shader::ShaderTarget::SPIRV
                        ? expected.SpirvGroup
                        : expected.DxilGroup);
                EXPECT_EQ(
                    found->Binding,
                    lane.Target == shader::ShaderTarget::SPIRV
                        ? expected.SpirvBinding
                        : expected.DxilBinding);
                EXPECT_EQ(found->StageMask, expected.StageMask);
                if (expected.PayloadType.empty()) {
                    EXPECT_EQ(found->TypeIndex, shader::kShaderNoType);
                } else {
                    ASSERT_LT(found->TypeIndex, types.size());
                    const shader::WireTypeRecord& payload = types[found->TypeIndex];
                    EXPECT_EQ(
                        payload.Kind,
                        static_cast<uint32_t>(shader::ShaderTypeKind::Struct));
                    EXPECT_EQ(payload.ParentIndex, shader::kShaderNoType);
                    const std::string_view payloadName{
                        reinterpret_cast<const char*>(
                            lane.Metadata.data() + payload.Name.Offset),
                        payload.Name.Size};
                    EXPECT_EQ(payloadName, expected.PayloadType);
                }
            }
            EXPECT_TRUE(shader::ValidateWireMetadataEnvelope(
                lane.Metadata,
                lane.Target,
                envelope.GpuArtifact));
        }
    }
}

// A keyword group that no code reads compiles to identical bytecode for every value,
// which makes the declared variant axis a lie. forward.hlsl branches its point light
// range window on QUALITY, so the two variants must not produce the same bytecode.
TEST(RadRayShaderLibPass, ForwardQualityKeywordChangesBytecode) {
    Client client;
    ASSERT_TRUE(client.IsAvailable());
    const vector<std::filesystem::path> includePaths{ShaderlibRoot()};
    constexpr std::string_view sourceName = "pipelines/forward/forward.hlsl";
    const vector<byte> source = ReadBytes(ShaderlibRoot() / "pipelines/forward/forward.hlsl");
    ASSERT_FALSE(source.empty());
    const DiscoveryResult discovery = client.DiscoverSourceContract(
        shader::SourceContractRequest{
            .SourceName = string{sourceName},
            .RootSource = source,
            .Defines = {},
            .Targets = shader::ShaderTargetMask::All,
            .Policy = {}},
        includePaths);
    ASSERT_TRUE(discovery.Succeeded())
        << (discovery.Diagnostics.empty() ? "" : discovery.Diagnostics.back().Message);

    const auto compileQuality = [&](std::string_view value) {
        return client.CompileVariant(
            shader::CompileVariantRequest{
                .SourceName = string{sourceName},
                .RootSource = source,
                .Defines = {},
                .Assignments = {{string{"QUALITY"}, string{value}}},
                .Targets = shader::ShaderTargetMask::All,
                .ExpectedContract = discovery.Contract.Hash},
            includePaths);
    };
    const shader::CompileVariantResult low = compileQuality("low");
    const shader::CompileVariantResult high = compileQuality("high");
    ASSERT_EQ(low.Status, shader::CompileStatus::Success)
        << (low.Diagnostics.empty() ? "" : low.Diagnostics.back().Message);
    ASSERT_EQ(high.Status, shader::CompileStatus::Success)
        << (high.Diagnostics.empty() ? "" : high.Diagnostics.back().Message);
    ASSERT_EQ(low.Lanes.size(), 2u);
    ASSERT_EQ(high.Lanes.size(), 2u);

    for (const shader::ShaderTarget target :
         {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        const auto findLane = [target](const shader::CompileVariantResult& result) {
            return std::find_if(
                result.Lanes.begin(),
                result.Lanes.end(),
                [target](const shader::CompileTargetLane& lane) noexcept {
                    return lane.Target == target;
                });
        };
        const auto lowLane = findLane(low);
        const auto highLane = findLane(high);
        ASSERT_NE(lowLane, low.Lanes.end());
        ASSERT_NE(highLane, high.Lanes.end());
        EXPECT_NE(lowLane->Bytecode, highLane->Bytecode)
            << "QUALITY produced identical bytecode for target "
            << static_cast<uint32_t>(target);
    }
}

TEST(RadRayShaderLibPass, PassesDeclareBothTargetBindingsExplicitly) {
    struct BindingCase {
        std::string_view RelativePath;
        bool HasResources;
    };
    constexpr BindingCase cases[] = {
        {"shaderlib/pipelines/forward/bindings.hlsli", true},
        {"modules/shader_compiler/tests/data/depth.hlsl", false},
        {"modules/shader_compiler/tests/data/compute.hlsl", true}};

    for (const BindingCase& pass : cases) {
        const vector<byte> source = ReadBytes(std::filesystem::path{RADRAY_PROJECT_DIR} / pass.RelativePath);
        ASSERT_FALSE(source.empty()) << pass.RelativePath;
        const string text{
            reinterpret_cast<const char*>(source.data()),
            source.size()};
        if (pass.HasResources) {
            EXPECT_NE(text.find("register("), string::npos) << pass.RelativePath;
            EXPECT_NE(text.find("VK_BINDING("), string::npos) << pass.RelativePath;
        } else {
            EXPECT_EQ(text.find("register("), string::npos) << pass.RelativePath;
            EXPECT_EQ(text.find("VK_BINDING("), string::npos) << pass.RelativePath;
        }
        EXPECT_EQ(text.find("RADRAY_FORWARD_"), string::npos) << pass.RelativePath;
        EXPECT_NE(text.find("#include <core/platform.hlsli>"), string::npos) << pass.RelativePath;
    }
}

}  // namespace
}  // namespace radray::shader_compiler
