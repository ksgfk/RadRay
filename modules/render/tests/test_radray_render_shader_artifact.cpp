#include <radray/render/pipeline_layout_types.h>
#include <radray/render/backend_shader_artifact.h>

#include "shader_contract_fixtures.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace radray::render {
namespace {

using shader::DecodeDxilShaderArtifact;
using shader::DecodeShaderArtifact;
using shader::DecodeSpirvShaderArtifact;
using shader::DxilShaderArtifactView;
using shader::ShaderArtifactDecodeOptions;
using shader::ShaderArtifactView;
using shader::SpirvShaderArtifactView;

// The identity moves with the wire schema, so every fixture decode names the same constant instead
// of repeating the literal and drifting apart from it.
constexpr uint64_t kFixtureToolchainIdentity = shader::kShaderToolchainIdentity;

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

shader::GpuArtifactHash ExpectedArtifact(uint8_t fixture, shader::ShaderTarget target) {
    shader::GpuArtifactHash result{};
    result.Bytes[0] = fixture;
    result.Bytes[1] = static_cast<uint8_t>(target);
    result.Bytes[2] = 0x4d;
    result.Bytes[3] = 0x30;
    return result;
}

std::optional<size_t> FindFixtureIndex(std::string_view name) {
    const auto fixtures = test::GetShaderContractFixtures();
    for (size_t index = 0; index < fixtures.size(); ++index) {
        if (fixtures[index].Name == name) {
            return index;
        }
    }
    return std::nullopt;
}

vector<byte> ReadFixtureArtifact(std::string_view name, shader::ShaderTarget target) {
    const std::optional<size_t> fixtureIndex = FindFixtureIndex(name);
    if (!fixtureIndex.has_value()) {
        return {};
    }
    const string suffix = target == shader::ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin";
    return ReadBinary(
        std::filesystem::path{RADRAY_PROJECT_DIR} /
        "modules/render/tests/data/shader_artifacts" /
        (string{name} + suffix));
}

ShaderArtifactDecodeOptions FixtureDecodeOptions(
    std::string_view name,
    shader::ShaderTarget target) {
    const std::optional<size_t> fixtureIndex = FindFixtureIndex(name);
    EXPECT_TRUE(fixtureIndex.has_value());
    return ShaderArtifactDecodeOptions{
        .Target = target,
        .ExpectedGpuArtifact = test::ExpectedGpuArtifact(fixtureIndex.value_or(0), target),
        .ExpectedToolchainIdentity = kFixtureToolchainIdentity};
}

shader::DiagnosticResult<ShaderArtifactView> DecodeFixtureArtifact(
    std::string_view name,
    shader::ShaderTarget target) {
    const vector<byte> blob = ReadFixtureArtifact(name, target);
    if (blob.empty()) {
        return {std::nullopt, "fixture artifact is missing"};
    }
    return DecodeShaderArtifact(blob, FixtureDecodeOptions(name, target));
}

// Neither backend has an adapter step any more: the resolved layout is what native creation
// consumes, so that is what these assertions read. Modifiers stay empty here: these fixtures pin
// what the compiler published, not what a pipeline asks for on top of it.
std::optional<ResolvedD3D12Layout> ResolveFixtureD3D12Layout(
    std::string_view name) {
    const vector<byte> blob = ReadFixtureArtifact(name, shader::ShaderTarget::DXIL);
    if (blob.empty()) {
        return std::nullopt;
    }
    const auto artifact = DecodeDxilShaderArtifact(
                              blob,
                              FixtureDecodeOptions(name, shader::ShaderTarget::DXIL))
                              .Value;
    return artifact.has_value() ? ResolveD3D12Layout(artifact.value()) : std::nullopt;
}

std::optional<ResolvedVulkanLayout> ResolveFixtureVulkanLayout(
    std::string_view name) {
    const vector<byte> blob = ReadFixtureArtifact(name, shader::ShaderTarget::SPIRV);
    if (blob.empty()) {
        return std::nullopt;
    }
    const auto artifact = DecodeSpirvShaderArtifact(
                              blob,
                              FixtureDecodeOptions(name, shader::ShaderTarget::SPIRV))
                              .Value;
    if (!artifact.has_value()) {
        return std::nullopt;
    }
    return ResolveVulkanLayout(artifact.value());
}

vector<byte> MakeSyntheticArtifact() {
    constexpr uint32_t entryCount = 2;
    constexpr uint32_t bindingCount = 2;
    constexpr uint32_t typeCount = 2;
    constexpr uint32_t rootConstantCount = 1;
    constexpr std::string_view names[] = {
        "VSMain", "PSMain", "Color", "Linear", "Constants", "Value", "PushBlock"};
    constexpr uint32_t nameBytes = 6 + 6 + 5 + 6 + 9 + 5 + 9;
    const uint32_t entryOffset = sizeof(shader::WireMetadataEnvelope);
    const uint32_t bindingOffset = entryOffset + entryCount * sizeof(shader::WireEntryRecord);
    const uint32_t typeOffset = bindingOffset + bindingCount * sizeof(shader::WireBindingRecord);
    const uint32_t rootOffset = typeOffset + typeCount * sizeof(shader::WireTypeRecord);
    const uint32_t nameOffset = rootOffset + rootConstantCount * sizeof(shader::WireRootConstantRecord);
    const uint32_t bytecodeOffset = nameOffset + nameBytes;
    const uint32_t totalSize = bytecodeOffset + 4;

    shader::WireMetadataEnvelope envelope{};
    envelope.Target = static_cast<uint8_t>(shader::ShaderTarget::DXIL);
    envelope.StageMask =
        (1u << static_cast<uint8_t>(shader::ShaderStage::Vertex)) |
        (1u << static_cast<uint8_t>(shader::ShaderStage::Pixel));
    envelope.TotalSize = totalSize;
    envelope.EntryRecords = {entryOffset, entryCount * sizeof(shader::WireEntryRecord)};
    envelope.BindingRecords = {bindingOffset, bindingCount * sizeof(shader::WireBindingRecord)};
    envelope.TypeRecords = {typeOffset, typeCount * sizeof(shader::WireTypeRecord)};
    envelope.RootConstantRecords = {rootOffset, rootConstantCount * sizeof(shader::WireRootConstantRecord)};
    envelope.Bytecode = {bytecodeOffset, 4};
    envelope.GpuArtifact = ExpectedArtifact(0, shader::ShaderTarget::DXIL);

    vector<byte> blob(totalSize);
    std::memcpy(blob.data(), &envelope, sizeof(envelope));

    uint32_t nameCursor = nameOffset;
    const auto nextName = [&](std::string_view name) {
        const shader::WireBlobRange range{nameCursor, static_cast<uint32_t>(name.size())};
        std::memcpy(blob.data() + nameCursor, name.data(), name.size());
        nameCursor += static_cast<uint32_t>(name.size());
        return range;
    };
    vector<shader::WireEntryRecord> entries{
        {nextName(names[0]), static_cast<uint8_t>(shader::ShaderStage::Vertex), 0, 0, 0, 4, 0},
        {nextName(names[1]), static_cast<uint8_t>(shader::ShaderStage::Pixel), 0, 0, 0, 4, 0}};
    constexpr uint32_t pixelStage = 1u << static_cast<uint8_t>(shader::ShaderStage::Pixel);
    vector<shader::WireBindingRecord> bindings{
        {.Name = nextName(names[2]),
         .Group = 0,
         .Binding = 0,
         .Type = static_cast<uint32_t>(shader::ShaderBindingKind::Texture),
         .Count = 1,
         .StageMask = pixelStage},
        {.Name = nextName(names[3]),
         .Group = 0,
         .Binding = 1,
         .Type = static_cast<uint32_t>(shader::ShaderBindingKind::Sampler),
         .Count = 1,
         .StageMask = pixelStage}};
    vector<shader::WireTypeRecord> types{
        {.Name = nextName(names[4]),
         .ParentIndex = shader::kShaderNoType,
         .Kind = static_cast<uint32_t>(shader::ShaderTypeKind::Struct),
         .ElementCount = 1,
         .Offset = 0,
         .Size = 16,
         .Stride = 16},
        {.Name = nextName(names[5]),
         .ParentIndex = 0,
         .Kind = static_cast<uint32_t>(shader::ShaderTypeKind::Vector),
         .ElementCount = 1,
         .Offset = 0,
         .Size = 16,
         .Stride = 16}};
    // A push block is keyed on its declaration name now, so the record carries one.
    vector<shader::WireRootConstantRecord> rootConstants{
        {.Name = nextName(names[6]), .RegisterSpace = 0, .Register = 0, .Offset = 0, .Size = 16, .StageMask = pixelStage}};
    std::memcpy(blob.data() + entryOffset, entries.data(), envelope.EntryRecords.Size);
    std::memcpy(blob.data() + bindingOffset, bindings.data(), envelope.BindingRecords.Size);
    std::memcpy(blob.data() + typeOffset, types.data(), envelope.TypeRecords.Size);
    std::memcpy(blob.data() + rootOffset, rootConstants.data(), envelope.RootConstantRecords.Size);
    blob[bytecodeOffset + 0] = byte{0x01};
    blob[bytecodeOffset + 1] = byte{0x02};
    blob[bytecodeOffset + 2] = byte{0x03};
    blob[bytecodeOffset + 3] = byte{0x04};
    return blob;
}

TEST(RadRayRenderShaderArtifact, LeafPaddingIsDxilOnly) {
    for (auto target : {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        auto blob = MakeSyntheticArtifact();
        shader::WireMetadataEnvelope envelope{};
        std::memcpy(&envelope, blob.data(), sizeof(envelope));
        envelope.Target = static_cast<uint8_t>(target);
        envelope.ToolchainIdentity = kFixtureToolchainIdentity;
        std::memcpy(blob.data(), &envelope, sizeof(envelope));
        const ShaderArtifactDecodeOptions options{
            .Target = target, .ExpectedGpuArtifact = envelope.GpuArtifact, .ExpectedToolchainIdentity = kFixtureToolchainIdentity};
        ASSERT_TRUE(DecodeShaderArtifact(blob, options).Value.has_value());
        shader::WireTypeRecord leaf{};
        const auto offset = envelope.TypeRecords.Offset + sizeof(shader::WireTypeRecord);
        std::memcpy(&leaf, blob.data() + offset, sizeof(leaf));
        leaf.Stride = 12;
        leaf.ScalarKind = static_cast<uint32_t>(shader::ShaderScalarKind::Float);
        leaf.RowCount = 1;
        leaf.ColumnCount = 3;
        std::memcpy(blob.data() + offset, &leaf, sizeof(leaf));

        auto diagnosticResult1 = DecodeShaderArtifact(blob, options);
        const auto padded = std::move(diagnosticResult1.Value);
        EXPECT_EQ(padded.has_value(), target == shader::ShaderTarget::DXIL);
        EXPECT_EQ(diagnosticResult1.Diagnostics.empty(), target == shader::ShaderTarget::DXIL);
        auto matrixBlob = MakeSyntheticArtifact();
        std::memcpy(&envelope, matrixBlob.data(), sizeof(envelope));
        envelope.Target = static_cast<uint8_t>(target);
        envelope.ToolchainIdentity = kFixtureToolchainIdentity;
        std::memcpy(matrixBlob.data(), &envelope, sizeof(envelope));
        shader::WireTypeRecord root{};
        std::memcpy(&root, matrixBlob.data() + envelope.TypeRecords.Offset, sizeof(root));
        std::memcpy(&leaf, matrixBlob.data() + offset, sizeof(leaf));
        root.Size = root.Stride = 64;
        leaf.Kind = static_cast<uint32_t>(shader::ShaderTypeKind::Matrix);
        leaf.Size = leaf.Stride = 64;
        leaf.ScalarKind = static_cast<uint32_t>(shader::ShaderScalarKind::Float);
        leaf.RowCount = leaf.ColumnCount = 4;
        std::memcpy(matrixBlob.data() + envelope.TypeRecords.Offset, &root, sizeof(root));
        std::memcpy(matrixBlob.data() + offset, &leaf, sizeof(leaf));
        ASSERT_TRUE(DecodeShaderArtifact(matrixBlob, options).Value.has_value());
        root.Size = root.Stride = 80;
        leaf.Size = 80;
        std::memcpy(matrixBlob.data() + envelope.TypeRecords.Offset, &root, sizeof(root));
        std::memcpy(matrixBlob.data() + offset, &leaf, sizeof(leaf));
        auto diagnosticResult2 = DecodeShaderArtifact(matrixBlob, options);
        EXPECT_FALSE(diagnosticResult2.Succeeded());
        EXPECT_NE(diagnosticResult2.Diagnostics.find("type"), string::npos);
    }
}

TEST(RadRayRenderShaderArtifact, BindingHandleRejectsForeignLayoutAddress) {
    int firstLayout = 0;
    int secondLayout = 0;
    const uintptr_t firstAddress = reinterpret_cast<uintptr_t>(&firstLayout);
    const uintptr_t secondAddress = reinterpret_cast<uintptr_t>(&secondLayout);
    const vector<BackendBindingName> records(1);
    const BindingHandle handle = BindingHandleAccess::Make(0, firstAddress);

    EXPECT_TRUE(handle.IsValid());
    EXPECT_EQ(FindBackendBindingRecord(records, firstAddress, handle).Get(), &records[0]);
    EXPECT_FALSE(FindBackendBindingRecord(records, secondAddress, handle).HasValue());
    EXPECT_FALSE(FindBackendBindingRecord(records, 0, handle).HasValue());
    EXPECT_FALSE(FindBackendBindingRecord(records, firstAddress, {}).HasValue());
    EXPECT_FALSE(FindBackendBindingRecord(records, firstAddress, BindingHandleAccess::Make(1, firstAddress)).HasValue());
    EXPECT_FALSE(BindingHandleAccess::Make(0, 0).IsValid());
}

TEST(RadRayRenderShaderArtifact, BindingHandlePreservesUpperAddressBits) {
    if constexpr (sizeof(uintptr_t) > sizeof(uint32_t)) {
        const uintptr_t firstAddress = static_cast<uintptr_t>(0x0000000112345678ull);
        const uintptr_t secondAddress = static_cast<uintptr_t>(0x0000000212345678ull);
        const vector<BackendBindingName> records(1);
        const BindingHandle handle = BindingHandleAccess::Make(0, firstAddress);

        EXPECT_EQ(BindingHandleAccess::Generation(handle), firstAddress);
        EXPECT_NE(handle, BindingHandleAccess::Make(0, secondAddress));
        EXPECT_FALSE(FindBackendBindingRecord(records, secondAddress, handle).HasValue());
    }
}

TEST(RadRayRenderShaderArtifact, BackendMappingsRejectUnknownValues) {
    const auto d3d12Target = GetShaderTargetForBackend(RenderBackend::D3D12);
    ASSERT_TRUE(d3d12Target.has_value());
    EXPECT_EQ(d3d12Target.value(), shader::ShaderTarget::DXIL);
    const auto vulkanTarget = GetShaderTargetForBackend(RenderBackend::Vulkan);
    ASSERT_TRUE(vulkanTarget.has_value());
    EXPECT_EQ(vulkanTarget.value(), shader::ShaderTarget::SPIRV);
    EXPECT_FALSE(GetShaderTargetForBackend(RenderBackend::MAX_COUNT).has_value());

    const auto dxilCategory = GetShaderBlobCategory(shader::ShaderTarget::DXIL);
    ASSERT_TRUE(dxilCategory.has_value());
    EXPECT_EQ(dxilCategory.value(), ShaderBlobCategory::DXIL);
    const auto spirvCategory = GetShaderBlobCategory(shader::ShaderTarget::SPIRV);
    ASSERT_TRUE(spirvCategory.has_value());
    EXPECT_EQ(spirvCategory.value(), ShaderBlobCategory::SPIRV);
    EXPECT_FALSE(GetShaderBlobCategory(static_cast<shader::ShaderTarget>(0xffu)).has_value());
}

TEST(RadRayRenderShaderArtifact, DecodesEveryRawGoldenLaneWithoutCompiler) {
    // Driven off the fixture table so the golden hash indices cannot drift from it.
    const std::span<const test::ShaderContractFixture> fixtures =
        test::GetShaderContractFixtures();
    const std::filesystem::path root = std::filesystem::path{RADRAY_PROJECT_DIR} /
                                       "modules/render/tests/data/shader_artifacts";
    for (uint8_t index = 0; index < fixtures.size(); ++index) {
        const std::string_view fixtureName = fixtures[index].Name;
        for (const shader::ShaderTarget target : {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
            const string suffix = target == shader::ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin";
            const vector<byte> blob = ReadBinary(root / (string{fixtureName} + suffix));
            ASSERT_FALSE(blob.empty());

            auto diagnosticResult3 = DecodeShaderArtifact(
                blob,
                ShaderArtifactDecodeOptions{
                    .Target = target,
                    .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index, target),
                    .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
            const auto artifact = std::move(diagnosticResult3.Value);
            ASSERT_TRUE(artifact.has_value()) << diagnosticResult3.Diagnostics;
            EXPECT_FALSE(artifact->Bytecode().empty());
            for (const shader::WireEntryRecord& entry : artifact->Entries()) {
                const auto stageBytecode = artifact->FindStageBytecode(
                    static_cast<shader::ShaderStage>(entry.Stage));
                ASSERT_TRUE(stageBytecode.has_value());
                EXPECT_EQ(stageBytecode->size(), entry.InterfaceSize);
                EXPECT_EQ(
                    stageBytecode->data(),
                    artifact->Bytecode().data() + entry.InterfaceOffset);
            }

            // Each target resolves into its own layout type, so the shared assertions compare
            // counts taken from whichever lane is under test.
            std::optional<size_t> resolvedBindingCount;
            std::optional<size_t> resolvedPushCount;
            if (target == shader::ShaderTarget::DXIL) {
                auto diagnosticResult4 = DecodeDxilShaderArtifact(
                    blob,
                    ShaderArtifactDecodeOptions{
                        .Target = target,
                        .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index, target),
                        .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
                const auto typed = std::move(diagnosticResult4.Value);
                ASSERT_TRUE(typed.has_value()) << diagnosticResult4.Diagnostics;
                const auto resolved = ResolveD3D12Layout(typed.value());
                ASSERT_TRUE(resolved.has_value());
                resolvedBindingCount = resolved->Bindings.size();
                resolvedPushCount = resolved->PushConstants.size();
            } else {
                auto diagnosticResult5 = DecodeSpirvShaderArtifact(
                    blob,
                    ShaderArtifactDecodeOptions{
                        .Target = target,
                        .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index, target),
                        .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
                const auto typed = std::move(diagnosticResult5.Value);
                ASSERT_TRUE(typed.has_value()) << diagnosticResult5.Diagnostics;
                const auto resolved = ResolveVulkanLayout(typed.value());
                ASSERT_TRUE(resolved.has_value());
                resolvedBindingCount = resolved->Bindings.size();
                resolvedPushCount = resolved->PushBlock.has_value() ? 1u : 0u;
            }
            ASSERT_TRUE(resolvedBindingCount.has_value());
            ASSERT_TRUE(resolvedPushCount.has_value());

            const test::ShaderContractFixture& fixture = test::GetShaderContractFixtures()[index];
            ASSERT_EQ(artifact->Entries().size(), fixture.Entries.size());
            for (const test::FixtureEntryFact& expected : fixture.Entries) {
                const auto entry = std::find_if(
                    artifact->Entries().begin(),
                    artifact->Entries().end(),
                    [&](const shader::WireEntryRecord& value) noexcept {
                        return artifact->GetName(value.Name).value_or(std::string_view{}) == expected.Name;
                    });
                ASSERT_NE(entry, artifact->Entries().end()) << expected.Name;
                EXPECT_EQ(entry->Stage, static_cast<uint8_t>(expected.Stage));
                EXPECT_NE(entry->InterfaceSize, 0u);
            }

            const size_t expectedBindingCount = static_cast<size_t>(std::count_if(
                fixture.Bindings.begin(),
                fixture.Bindings.end(),
                [](const test::FixtureBindingFact& value) noexcept {
                    return value.Kind != test::FixtureResourceKind::RootConstant;
                }));
            ASSERT_EQ(artifact->Bindings().size(), expectedBindingCount);
            EXPECT_EQ(resolvedBindingCount.value(), expectedBindingCount);
            // Only the DXIL lane carries the serialized policy, and it carries it exactly when the
            // source declared one.
            const bool hasPolicy = fixture.Name == std::string_view{"shadow_static_sampler"} ||
                                   fixture.Name == std::string_view{"multiple_root_constants"};
            EXPECT_EQ(
                !artifact->SerializedRootSignature().empty(),
                hasPolicy && target == shader::ShaderTarget::DXIL)
                << fixture.Name;
            for (const test::FixtureBindingFact& expected : fixture.Bindings) {
                if (expected.Kind == test::FixtureResourceKind::RootConstant) {
                    continue;
                }
                const auto binding = artifact->FindBinding(expected.Name);
                ASSERT_TRUE(binding.has_value()) << expected.Name;
                const uint32_t expectedGroup = target == shader::ShaderTarget::DXIL
                                                   ? expected.D3D12Group
                                                   : expected.VulkanSet;
                const uint32_t expectedBinding = target == shader::ShaderTarget::DXIL
                                                     ? expected.D3D12Binding
                                                     : expected.VulkanBinding;
                EXPECT_EQ(binding->Record.Group, expectedGroup) << expected.Name;
                EXPECT_EQ(binding->Record.Binding, expectedBinding) << expected.Name;
                EXPECT_EQ(binding->Record.StageMask, expected.StageMask) << expected.Name;
                EXPECT_EQ(
                    binding->Record.Type,
                    static_cast<uint32_t>(test::ExpectedWireKind(expected.Kind)))
                    << expected.Name;
                // Placement is where the two targets legitimately disagree: the same policy slot is
                // a D3D12 static sampler with no table entry, and a Vulkan table entry whose state
                // comes from a published sampler record.
                const uint32_t expectedPlacement = static_cast<uint32_t>(
                    expected.PolicyStaticSampler && target == shader::ShaderTarget::DXIL
                        ? shader::ShaderBindingPlacement::StaticSampler
                        : shader::ShaderBindingPlacement::Table);
                EXPECT_EQ(binding->Record.Placement, expectedPlacement) << expected.Name;
                if (expected.PolicyStaticSampler && target == shader::ShaderTarget::SPIRV) {
                    ASSERT_LT(binding->Record.SamplerIndex, artifact->Samplers().size())
                        << expected.Name;
                } else {
                    EXPECT_EQ(binding->Record.SamplerIndex, shader::kShaderNoSampler)
                        << expected.Name;
                }
                if (expected.PayloadType.empty()) {
                    EXPECT_EQ(binding->Record.TypeIndex, shader::kShaderNoType)
                        << expected.Name;
                } else {
                    ASSERT_LT(binding->Record.TypeIndex, artifact->Types().size())
                        << expected.Name;
                    const shader::WireTypeRecord& payload =
                        artifact->Types()[binding->Record.TypeIndex];
                    EXPECT_EQ(
                        payload.Kind,
                        static_cast<uint32_t>(shader::ShaderTypeKind::Struct))
                        << expected.Name;
                    EXPECT_EQ(payload.ParentIndex, shader::kShaderNoType)
                        << expected.Name;
                    EXPECT_EQ(
                        artifact->GetName(payload.Name).value_or(std::string_view{}),
                        expected.PayloadType)
                        << expected.Name;
                }
            }

            const uint32_t expectedTypeRecordCount = target == shader::ShaderTarget::SPIRV &&
                                                             fixture.SpirvTypeRecordCount != 0
                                                         ? fixture.SpirvTypeRecordCount
                                                         : fixture.TypeRecordCount;
            EXPECT_EQ(artifact->Types().size(), expectedTypeRecordCount);
            if (fixture.Name == std::string_view{"nested_types"}) {
                constexpr std::string_view names[] = {
                    "NestedLeaf",
                    "Direction",
                    "Weight",
                    "NestedArray",
                    "Values",
                    "NestedRoot",
                    "Transform",
                    "Data"};
                constexpr uint32_t parents[] = {
                    0xffffffffu,
                    0u,
                    0u,
                    0xffffffffu,
                    3u,
                    0xffffffffu,
                    5u,
                    5u};
                constexpr uint32_t kinds[] = {4u, 2u, 1u, 4u, 5u, 4u, 3u, 4u};
                constexpr uint32_t elementCounts[] = {1u, 1u, 1u, 1u, 2u, 1u, 1u, 1u};
                constexpr uint32_t offsets[] = {0u, 0u, 12u, 0u, 0u, 0u, 0u, 64u};
                constexpr uint32_t sizes[] = {16u, 12u, 4u, 32u, 32u, 96u, 64u, 32u};
                constexpr uint32_t strides[] = {16u, 12u, 4u, 32u, 16u, 96u, 64u, 32u};
                constexpr uint32_t typeIndices[] = {
                    shader::kShaderNoType,
                    shader::kShaderNoType,
                    shader::kShaderNoType,
                    shader::kShaderNoType,
                    0u,
                    shader::kShaderNoType,
                    shader::kShaderNoType,
                    3u};
                ASSERT_EQ(artifact->Types().size(), std::size(names));
                for (size_t typeIndex = 0; typeIndex < std::size(names); ++typeIndex) {
                    const shader::WireTypeRecord& type = artifact->Types()[typeIndex];
                    ASSERT_EQ(artifact->GetName(type.Name).value_or(std::string_view{}), names[typeIndex]);
                    EXPECT_EQ(type.ParentIndex, parents[typeIndex]);
                    EXPECT_EQ(type.Kind, kinds[typeIndex]);
                    EXPECT_EQ(type.ElementCount, elementCounts[typeIndex]);
                    EXPECT_EQ(type.Offset, offsets[typeIndex]);
                    EXPECT_EQ(type.Size, sizes[typeIndex]);
                    EXPECT_EQ(type.Stride, strides[typeIndex]);
                    EXPECT_EQ(type.TypeIndex, typeIndices[typeIndex]);
                }
            }
            if (fixture.Name == std::string_view{"unused_resource"}) {
                EXPECT_FALSE(artifact->FindBinding("UnusedTexture").has_value());
            }
            const size_t rootFactCount = static_cast<size_t>(std::count_if(
                fixture.Bindings.begin(),
                fixture.Bindings.end(),
                [](const test::FixtureBindingFact& value) noexcept {
                    return value.Kind == test::FixtureResourceKind::RootConstant;
                }));
            const size_t expectedRootCount = fixture.HasSingleSpirvPushBlock
                                                 ? target == shader::ShaderTarget::SPIRV ? 1u : 0u
                                             : target == shader::ShaderTarget::DXIL ? rootFactCount
                                                                                    : 0u;
            ASSERT_EQ(artifact->RootConstants().size(), expectedRootCount);
            EXPECT_EQ(resolvedPushCount.value(), expectedRootCount);
            if (expectedRootCount != 0) {
                for (size_t rootIndex = 0; rootIndex < expectedRootCount; ++rootIndex) {
                    const test::FixtureBindingFact& expected = fixture.Bindings[std::find_if(
                                                                                    fixture.Bindings.begin(),
                                                                                    fixture.Bindings.end(),
                                                                                    [](const test::FixtureBindingFact& value) noexcept {
                                                                                        return value.Kind == test::FixtureResourceKind::RootConstant;
                                                                                    }) -
                                                                                fixture.Bindings.begin() + rootIndex];
                    const shader::WireRootConstantRecord& rootConstant = artifact->RootConstants()[rootIndex];
                    EXPECT_EQ(
                        artifact->GetName(rootConstant.Name).value_or(std::string_view{}),
                        expected.Name);
                    EXPECT_EQ(rootConstant.RegisterSpace, expected.D3D12Group);
                    EXPECT_EQ(rootConstant.Register, expected.D3D12Binding);
                    EXPECT_EQ(rootConstant.StageMask, expected.StageMask);
                    EXPECT_NE(rootConstant.Size, 0u);
                    if (expected.PayloadType.empty()) {
                        EXPECT_EQ(rootConstant.TypeIndex, shader::kShaderNoType);
                    } else {
                        ASSERT_LT(rootConstant.TypeIndex, artifact->Types().size());
                        const shader::WireTypeRecord& payload = artifact->Types()[rootConstant.TypeIndex];
                        EXPECT_EQ(
                            payload.Kind,
                            static_cast<uint32_t>(shader::ShaderTypeKind::Struct));
                        EXPECT_EQ(payload.ParentIndex, shader::kShaderNoType);
                        EXPECT_EQ(
                            artifact->GetName(payload.Name).value_or(std::string_view{}),
                            expected.PayloadType);
                    }
                    if (fixture.HasSingleSpirvPushBlock) {
                        EXPECT_EQ(rootConstant.Flags & 1u, 1u);
                    }
                }
            }
        }
    }
}

TEST(RadRayRenderShaderArtifact, PolicyStaticSamplerLandsPerTargetWithoutLosingState) {
    const vector<byte> dxilBlob = ReadFixtureArtifact("shadow_static_sampler", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(dxilBlob.empty());
    auto diagnosticResult6 = DecodeDxilShaderArtifact(
        dxilBlob,
        FixtureDecodeOptions("shadow_static_sampler", shader::ShaderTarget::DXIL));
    const auto dxil = std::move(diagnosticResult6.Value);
    ASSERT_TRUE(dxil.has_value()) << diagnosticResult6.Diagnostics;

    // On D3D12 the static sampler exists only inside the serialized carrier: it owns no descriptor
    // table slot, so the layout must not reserve one, and no sampler state is published separately.
    const auto dxilSampler = dxil->Generic().FindBinding("ShadowSampler");
    ASSERT_TRUE(dxilSampler.has_value());
    EXPECT_EQ(
        dxilSampler->Record.Placement,
        static_cast<uint32_t>(shader::ShaderBindingPlacement::StaticSampler));
    EXPECT_EQ(dxilSampler->Record.SamplerIndex, shader::kShaderNoSampler);
    EXPECT_TRUE(dxil->Generic().Samplers().empty());
    EXPECT_FALSE(dxil->Generic().SerializedRootSignature().empty());

    const auto dxilLayout = ResolveD3D12Layout(dxil.value());
    ASSERT_TRUE(dxilLayout.has_value());
    EXPECT_TRUE(dxilLayout->HasExplicitCarrier());
    // Every sampler this fixture declares is a policy static sampler, so the resolved layout must
    // keep that placement: an entry that came back as a table sampler would mean the layout had
    // silently reserved a descriptor slot the carrier does not describe.
    for (const ResolvedD3D12Binding& resolvedBinding : dxilLayout->Bindings) {
        if (resolvedBinding.LogicalKind != shader::ShaderBindingKind::Sampler) {
            continue;
        }
        EXPECT_EQ(resolvedBinding.Placement, shader::ShaderBindingPlacement::StaticSampler);
    }

    const vector<byte> spirvBlob = ReadFixtureArtifact("shadow_static_sampler", shader::ShaderTarget::SPIRV);
    ASSERT_FALSE(spirvBlob.empty());
    auto diagnosticResult7 = DecodeSpirvShaderArtifact(
        spirvBlob,
        FixtureDecodeOptions("shadow_static_sampler", shader::ShaderTarget::SPIRV));
    const auto spirv = std::move(diagnosticResult7.Value);
    ASSERT_TRUE(spirv.has_value()) << diagnosticResult7.Diagnostics;

    // On Vulkan the same policy slot is a table entry plus a full immutable sampler state. The
    // comparison filter the policy asked for has to survive as compare-enabled state, because a
    // downgrade here would silently change filtering.
    const auto spirvSampler = spirv->Generic().FindBinding("ShadowSampler");
    ASSERT_TRUE(spirvSampler.has_value());
    EXPECT_EQ(
        spirvSampler->Record.Placement,
        static_cast<uint32_t>(shader::ShaderBindingPlacement::Table));
    ASSERT_LT(spirvSampler->Record.SamplerIndex, spirv->Generic().Samplers().size());
    EXPECT_TRUE(spirv->Generic().SerializedRootSignature().empty());
    const shader::WireSamplerRecord& state =
        spirv->Generic().Samplers()[spirvSampler->Record.SamplerIndex];
    EXPECT_EQ(state.CompareEnable, 1u);
    EXPECT_EQ(state.MagFilter, 1u);
    EXPECT_EQ(state.MinFilter, 1u);
    EXPECT_EQ(state.MipmapMode, 0u);

    // Vulkan native layout creation consumes the resolved layout directly, so the state has to
    // survive resolution as well: the published record is what becomes the VkSampler.
    const auto spirvLayout = ResolveVulkanLayout(spirv.value());
    ASSERT_TRUE(spirvLayout.has_value());
    const auto resolvedSampler = std::find_if(
        spirvLayout->Bindings.begin(),
        spirvLayout->Bindings.end(),
        [](const ResolvedVulkanBinding& value) noexcept { return value.Name == "ShadowSampler"; });
    ASSERT_NE(resolvedSampler, spirvLayout->Bindings.end());
    EXPECT_EQ(resolvedSampler->LogicalKind, shader::ShaderBindingKind::Sampler);
    EXPECT_EQ(resolvedSampler->Placement, VulkanBufferDescriptorPlacement::Regular);
    ASSERT_LT(resolvedSampler->ImmutableSamplerIndex, spirvLayout->ImmutableSamplers.size());
    EXPECT_EQ(spirvLayout->ImmutableSamplers[resolvedSampler->ImmutableSamplerIndex], state);
}

TEST(RadRayRenderShaderArtifact, VertexInterfaceMatchesExternalLayoutForBothTargets) {
    for (const shader::ShaderTarget target : {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        auto diagnosticResult8 = DecodeFixtureArtifact("nested_types", target);
        const auto artifact = std::move(diagnosticResult8.Value);
        ASSERT_TRUE(artifact.has_value()) << diagnosticResult8.Diagnostics;
        ASSERT_EQ(artifact->VertexInputs().size(), 1u);
        const shader::WireVertexInputRecord& input = artifact->VertexInputs().front();
        EXPECT_EQ(artifact->GetName(input.Semantic).value_or(std::string_view{}), "POSITION");
        EXPECT_EQ(input.SemanticIndex, 0u);
        EXPECT_EQ(input.Location, 0u);
        EXPECT_EQ(
            input.ComponentType,
            static_cast<uint32_t>(shader::ShaderVertexComponentType::Float));
        EXPECT_EQ(input.ComponentCount, 3u);

        const VertexBufferLayout buffer{
            .Binding = 0,
            .ArrayStride = 12,
            .StepMode = VertexStepMode::Vertex};
        const VertexAttribute attribute{
            .BufferBinding = 0,
            .Offset = 0,
            .Semantic = "POSITION",
            .SemanticIndex = 0,
            .Format = VertexFormat::FLOAT32X3,
            .Location = 0};
        const VertexInputState state{
            .Buffers = std::span{&buffer, 1},
            .Attributes = std::span{&attribute, 1}};
        EXPECT_TRUE(ValidateVertexInputStateAgainstArtifact(state, *artifact));

        VertexAttribute wrongLocation = attribute;
        wrongLocation.Location = 1;
        const VertexInputState wrongLocationState{
            .Buffers = std::span{&buffer, 1},
            .Attributes = std::span{&wrongLocation, 1}};
        EXPECT_FALSE(ValidateVertexInputStateAgainstArtifact(wrongLocationState, *artifact));

        VertexAttribute wrongFormat = attribute;
        wrongFormat.Format = VertexFormat::FLOAT32X4;
        const VertexInputState wrongFormatState{
            .Buffers = std::span{&buffer, 1},
            .Attributes = std::span{&wrongFormat, 1}};
        EXPECT_FALSE(ValidateVertexInputStateAgainstArtifact(wrongFormatState, *artifact));
    }
}

TEST(RadRayRenderShaderArtifact, MultipleDxilRootConstantsRemainIndependent) {
    auto diagnosticResult9 = DecodeFixtureArtifact(
        "multiple_root_constants",
        shader::ShaderTarget::DXIL);
    const auto artifact = std::move(diagnosticResult9.Value);
    ASSERT_TRUE(artifact.has_value()) << diagnosticResult9.Diagnostics;
    ASSERT_EQ(artifact->RootConstants().size(), 2u);
    EXPECT_EQ(artifact->RootConstants()[0].RegisterSpace, 0u);
    EXPECT_EQ(artifact->RootConstants()[0].Register, 0u);
    EXPECT_EQ(artifact->RootConstants()[0].Size, 64u);
    EXPECT_EQ(artifact->RootConstants()[1].RegisterSpace, 0u);
    EXPECT_EQ(artifact->RootConstants()[1].Register, 1u);
    EXPECT_EQ(artifact->RootConstants()[1].Size, 16u);

    const auto layout = ResolveFixtureD3D12Layout("multiple_root_constants");
    ASSERT_TRUE(layout.has_value());
    ASSERT_EQ(layout->PushConstants.size(), 2u);
    EXPECT_EQ(layout->PushConstants[0].Register, 0u);
    EXPECT_EQ(layout->PushConstants[0].Size, 64u);
    EXPECT_EQ(layout->PushConstants[1].Register, 1u);
    EXPECT_EQ(layout->PushConstants[1].Size, 16u);
}

TEST(RadRayRenderShaderArtifact, SingleSpirvPushBlockDoesNotBecomeMultipleConstants) {
    auto diagnosticResult10 = DecodeFixtureArtifact("spirv_push_constant", shader::ShaderTarget::DXIL);
    const auto dxil = std::move(diagnosticResult10.Value);
    ASSERT_TRUE(dxil.has_value()) << diagnosticResult10.Diagnostics;
    EXPECT_TRUE(dxil->RootConstants().empty());

    auto diagnosticResult11 = DecodeFixtureArtifact("spirv_push_constant", shader::ShaderTarget::SPIRV);
    const auto spirv = std::move(diagnosticResult11.Value);
    ASSERT_TRUE(spirv.has_value()) << diagnosticResult11.Diagnostics;
    ASSERT_EQ(spirv->RootConstants().size(), 1u);
    EXPECT_EQ(spirv->RootConstants()[0].RegisterSpace, 0u);
    EXPECT_EQ(spirv->RootConstants()[0].Register, 0u);
    EXPECT_EQ(spirv->RootConstants()[0].Size, 16u);
    EXPECT_EQ(spirv->RootConstants()[0].Flags & 1u, 1u);

    const auto layout = ResolveFixtureVulkanLayout("spirv_push_constant");
    ASSERT_TRUE(layout.has_value());
    ASSERT_TRUE(layout->PushBlock.has_value());
    EXPECT_EQ(layout->PushBlock->Size, 16u);
}

TEST(RadRayRenderShaderArtifact, NestedTypeTreeLayoutFactsRemainTargetStable) {
    constexpr std::string_view names[] = {
        "NestedLeaf",
        "Direction",
        "Weight",
        "NestedArray",
        "Values",
        "NestedRoot",
        "Transform",
        "Data"};
    constexpr uint32_t parents[] = {0xffffffffu, 0u, 0u, 0xffffffffu, 3u, 0xffffffffu, 5u, 5u};
    constexpr uint32_t kinds[] = {4u, 2u, 1u, 4u, 5u, 4u, 3u, 4u};
    constexpr uint32_t elementCounts[] = {1u, 1u, 1u, 1u, 2u, 1u, 1u, 1u};
    constexpr uint32_t offsets[] = {0u, 0u, 12u, 0u, 0u, 0u, 0u, 64u};
    constexpr uint32_t sizes[] = {16u, 12u, 4u, 32u, 32u, 96u, 64u, 32u};
    constexpr uint32_t strides[] = {16u, 12u, 4u, 32u, 16u, 96u, 64u, 32u};

    for (const shader::ShaderTarget target : {shader::ShaderTarget::DXIL, shader::ShaderTarget::SPIRV}) {
        auto diagnosticResult12 = DecodeFixtureArtifact("nested_types", target);
        const auto artifact = std::move(diagnosticResult12.Value);
        ASSERT_TRUE(artifact.has_value()) << diagnosticResult12.Diagnostics;
        ASSERT_EQ(artifact->Types().size(), std::size(names));
        for (size_t index = 0; index < std::size(names); ++index) {
            const shader::WireTypeRecord& type = artifact->Types()[index];
            ASSERT_EQ(artifact->GetName(type.Name).value_or(std::string_view{}), names[index]);
            EXPECT_EQ(type.ParentIndex, parents[index]);
            EXPECT_EQ(type.Kind, kinds[index]);
            EXPECT_EQ(type.ElementCount, elementCounts[index]);
            EXPECT_EQ(type.Offset, offsets[index]);
            EXPECT_EQ(type.Size, sizes[index]);
            EXPECT_EQ(type.Stride, strides[index]);
        }
    }
}

TEST(RadRayRenderShaderArtifact, TypeTreeMutationDoesNotChangeGpuArtifactIdentity) {
    const std::filesystem::path path = std::filesystem::path{RADRAY_PROJECT_DIR} /
                                       "modules/render/tests/data/shader_artifacts/nested_types.dxil.bin";
    const vector<byte> original = ReadBinary(path);
    ASSERT_FALSE(original.empty());
    const std::optional<size_t> fixtureIndex = FindFixtureIndex("nested_types");
    ASSERT_TRUE(fixtureIndex.has_value());

    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, original.data(), sizeof(envelope));
    ASSERT_EQ(envelope.TypeRecords.Size, 8u * sizeof(shader::WireTypeRecord));
    vector<shader::WireTypeRecord> types(8);
    std::memcpy(types.data(), original.data() + envelope.TypeRecords.Offset, envelope.TypeRecords.Size);
    // Reorder the two occupied ranges without overlap: the type tree may change
    // independently of GPU identity, but must still describe a valid layout.
    types[6].Offset = 32;
    types[7].Offset = 0;
    vector<byte> mutated = original;
    std::memcpy(mutated.data() + envelope.TypeRecords.Offset, types.data(), envelope.TypeRecords.Size);

    auto diagnosticResult13 = DecodeDxilShaderArtifact(
        mutated,
        ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::DXIL,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(
                fixtureIndex.value(), shader::ShaderTarget::DXIL),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
    const auto artifact = std::move(diagnosticResult13.Value);
    ASSERT_TRUE(artifact.has_value()) << diagnosticResult13.Diagnostics;
    EXPECT_EQ(
        artifact->Generic().Envelope().GpuArtifact,
        test::ExpectedGpuArtifact(fixtureIndex.value(), shader::ShaderTarget::DXIL));
    // The former mutation placed Transform at [16,80) over Data at [64,96).
    types[6].Offset = 16;
    types[7].Offset = 64;
    std::memcpy(mutated.data() + envelope.TypeRecords.Offset, types.data(), envelope.TypeRecords.Size);
    auto diagnosticResult14 = DecodeDxilShaderArtifact(
        mutated,
        ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::DXIL,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(
                fixtureIndex.value(), shader::ShaderTarget::DXIL),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
    EXPECT_FALSE(diagnosticResult14.Succeeded());
    EXPECT_NE(diagnosticResult14.Diagnostics.find("type"), string::npos);
}

TEST(RadRayRenderShaderArtifact, FailsClosedForIdentityAndWireCorruption) {
    const std::filesystem::path path = std::filesystem::path{RADRAY_PROJECT_DIR} /
                                       "modules/render/tests/data/shader_artifacts/no_resource_graphics.dxil.bin";
    const shader::ShaderTarget target = shader::ShaderTarget::DXIL;
    const shader::GpuArtifactHash expected = test::ExpectedGpuArtifact(0, target);
    const vector<byte> original = ReadBinary(path);
    ASSERT_FALSE(original.empty());

    auto decode = [&](const vector<byte>& blob,
                      uint64_t toolchain = 0) {
        return DecodeShaderArtifact(
            blob,
            ShaderArtifactDecodeOptions{
                .Target = target,
                .ExpectedGpuArtifact = expected,
                .ExpectedToolchainIdentity = toolchain});
    };

    auto diagnosticResult15 = decode(original, 0xdeadbeefull);
    EXPECT_FALSE(diagnosticResult15.Succeeded());
    EXPECT_NE(diagnosticResult15.Diagnostics.find("toolchain"), string::npos);
    EXPECT_FALSE(decode(vector<byte>{original.begin(), original.begin() + sizeof(shader::WireMetadataEnvelope) - 1}).Value.has_value());

    auto badMagic = original;
    auto* magic = reinterpret_cast<uint32_t*>(badMagic.data());
    *magic = 0;
    EXPECT_FALSE(decode(badMagic).Value.has_value());

    auto badRange = original;
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, badRange.data(), sizeof(envelope));
    envelope.Bytecode.Offset = envelope.TotalSize - 1;
    std::memcpy(badRange.data(), &envelope, sizeof(envelope));
    EXPECT_FALSE(decode(badRange).Value.has_value());
}

TEST(RadRayRenderShaderArtifact, RejectsDuplicateAndMalformedRecords) {
    const vector<byte> original = MakeSyntheticArtifact();
    const auto decode = [&](const vector<byte>& blob, uint8_t fixture) {
        return DecodeShaderArtifact(
            blob,
            ShaderArtifactDecodeOptions{
                .Target = shader::ShaderTarget::DXIL,
                .ExpectedGpuArtifact = ExpectedArtifact(fixture, shader::ShaderTarget::DXIL)});
    };

    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, original.data(), sizeof(envelope));
    auto spirvRootSignature = original;
    envelope.Target = static_cast<uint8_t>(shader::ShaderTarget::SPIRV);
    envelope.GpuArtifact = ExpectedArtifact(0, shader::ShaderTarget::SPIRV);
    envelope.RootSignature = envelope.Bytecode;
    std::memcpy(spirvRootSignature.data(), &envelope, sizeof(envelope));

    auto diagnosticResult16 = DecodeShaderArtifact(
        spirvRootSignature,
        ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::SPIRV,
            .ExpectedGpuArtifact = ExpectedArtifact(0, shader::ShaderTarget::SPIRV)});
    EXPECT_FALSE(diagnosticResult16.Succeeded());
    EXPECT_NE(diagnosticResult16.Diagnostics.find("root signature"), string::npos);

    std::memcpy(&envelope, original.data(), sizeof(envelope));
    vector<shader::WireEntryRecord> entries(2);
    std::memcpy(
        entries.data(),
        original.data() + envelope.EntryRecords.Offset,
        envelope.EntryRecords.Size);
    entries[1].Name = entries[0].Name;
    vector<byte> duplicateEntry = original;
    std::memcpy(
        duplicateEntry.data() + envelope.EntryRecords.Offset,
        entries.data(),
        envelope.EntryRecords.Size);

    auto diagnosticResult17 = decode(duplicateEntry, 0);
    EXPECT_FALSE(diagnosticResult17.Succeeded());
    EXPECT_NE(diagnosticResult17.Diagnostics.find("duplicate entry"), string::npos);

    vector<shader::WireBindingRecord> bindings(2);
    std::memcpy(
        bindings.data(),
        original.data() + envelope.BindingRecords.Offset,
        envelope.BindingRecords.Size);
    bindings[1].Group = bindings[0].Group;
    bindings[1].Binding = bindings[0].Binding;
    bindings[1].Type = bindings[0].Type;
    vector<byte> duplicateBinding = original;
    std::memcpy(
        duplicateBinding.data() + envelope.BindingRecords.Offset,
        bindings.data(),
        envelope.BindingRecords.Size);

    auto diagnosticResult18 = decode(duplicateBinding, 0);
    EXPECT_FALSE(diagnosticResult18.Succeeded());
    EXPECT_NE(diagnosticResult18.Diagnostics.find("duplicate binding"), string::npos);
    const auto expectInvalidBindingOwner = [&](auto&& mutate) {
        vector<byte> mutated = original;
        vector<shader::WireBindingRecord> records(
            envelope.BindingRecords.Size / sizeof(shader::WireBindingRecord));
        std::memcpy(
            records.data(),
            original.data() + envelope.BindingRecords.Offset,
            envelope.BindingRecords.Size);
        mutate(records.front());
        std::memcpy(
            mutated.data() + envelope.BindingRecords.Offset,
            records.data(),
            envelope.BindingRecords.Size);

        auto diagnosticResult19 = decode(mutated, 0);
        EXPECT_FALSE(diagnosticResult19.Succeeded());
        EXPECT_NE(diagnosticResult19.Diagnostics.find("binding"), string::npos);
    };
    expectInvalidBindingOwner([](shader::WireBindingRecord& binding) {
        binding.Type = static_cast<uint32_t>(shader::ShaderBindingKind::CBuffer);
    });
    expectInvalidBindingOwner([](shader::WireBindingRecord& binding) {
        binding.Type = static_cast<uint32_t>(shader::ShaderBindingKind::CBuffer);
        binding.TypeIndex = 2;
    });
    expectInvalidBindingOwner([](shader::WireBindingRecord& binding) {
        binding.Type = static_cast<uint32_t>(shader::ShaderBindingKind::CBuffer);
        binding.TypeIndex = 1;
    });
    expectInvalidBindingOwner([](shader::WireBindingRecord& binding) {
        binding.TypeIndex = 0;
    });

    vector<shader::WireTypeRecord> types(
        envelope.TypeRecords.Size / sizeof(shader::WireTypeRecord));
    std::memcpy(
        types.data(),
        original.data() + envelope.TypeRecords.Offset,
        envelope.TypeRecords.Size);
    types.back().ParentIndex = static_cast<uint32_t>(types.size());
    vector<byte> invalidType = original;
    std::memcpy(
        invalidType.data() + envelope.TypeRecords.Offset,
        types.data(),
        envelope.TypeRecords.Size);

    auto diagnosticResult20 = decode(invalidType, 0);
    EXPECT_FALSE(diagnosticResult20.Succeeded());
    EXPECT_NE(diagnosticResult20.Diagnostics.find("type"), string::npos);

    const auto expectInvalidSyntheticType = [&](auto&& mutate) {
        vector<byte> mutated = original;
        vector<shader::WireTypeRecord> records(
            envelope.TypeRecords.Size / sizeof(shader::WireTypeRecord));
        std::memcpy(
            records.data(),
            original.data() + envelope.TypeRecords.Offset,
            envelope.TypeRecords.Size);
        mutate(records.front());
        std::memcpy(
            mutated.data() + envelope.TypeRecords.Offset,
            records.data(),
            envelope.TypeRecords.Size);

        auto diagnosticResult21 = decode(mutated, 0);
        EXPECT_FALSE(diagnosticResult21.Succeeded());
        EXPECT_NE(diagnosticResult21.Diagnostics.find("type"), string::npos);
    };
    expectInvalidSyntheticType([](shader::WireTypeRecord& type) { type.Kind = 0; });
    expectInvalidSyntheticType([](shader::WireTypeRecord& type) { type.ElementCount = 0; });
    expectInvalidSyntheticType([](shader::WireTypeRecord& type) { type.Offset = 1; });
    expectInvalidSyntheticType([](shader::WireTypeRecord& type) { type.Stride = 8; });
    expectInvalidSyntheticType([](shader::WireTypeRecord& type) {
        type.Kind = static_cast<uint32_t>(shader::ShaderTypeKind::Array);
        type.ElementCount = 2;
        type.Size = 15;
    });
    expectInvalidSyntheticType([](shader::WireTypeRecord& type) { type.ParentIndex = 0; });

    const vector<byte> nested = ReadBinary(
        std::filesystem::path{RADRAY_PROJECT_DIR} /
        "modules/render/tests/data/shader_artifacts/nested_types.dxil.bin");
    ASSERT_FALSE(nested.empty());
    const std::optional<size_t> nestedFixtureIndex = FindFixtureIndex("nested_types");
    ASSERT_TRUE(nestedFixtureIndex.has_value());
    shader::WireMetadataEnvelope nestedEnvelope{};
    std::memcpy(&nestedEnvelope, nested.data(), sizeof(nestedEnvelope));
    ASSERT_EQ(nestedEnvelope.TypeRecords.Size, 8u * sizeof(shader::WireTypeRecord));
    const auto expectInvalidNestedType = [&](auto&& mutate) {
        vector<byte> mutated = nested;
        vector<shader::WireTypeRecord> records(8);
        std::memcpy(
            records.data(),
            nested.data() + nestedEnvelope.TypeRecords.Offset,
            nestedEnvelope.TypeRecords.Size);
        mutate(records);
        std::memcpy(
            mutated.data() + nestedEnvelope.TypeRecords.Offset,
            records.data(),
            nestedEnvelope.TypeRecords.Size);
        const auto artifact = DecodeShaderArtifact(
                                  mutated,
                                  ShaderArtifactDecodeOptions{
                                      .Target = shader::ShaderTarget::DXIL,
                                      .ExpectedGpuArtifact = test::ExpectedGpuArtifact(
                                          nestedFixtureIndex.value(), shader::ShaderTarget::DXIL),
                                      .ExpectedToolchainIdentity = kFixtureToolchainIdentity})
                                  .Value;
        EXPECT_FALSE(artifact.has_value());
    };
    expectInvalidNestedType([](vector<shader::WireTypeRecord>& types) { types[7].Offset = 80; });
    expectInvalidNestedType([](vector<shader::WireTypeRecord>& types) { types[1].ParentIndex = 2; });
    expectInvalidNestedType([](vector<shader::WireTypeRecord>& types) { types[2].Name = types[1].Name; });
    expectInvalidNestedType([](vector<shader::WireTypeRecord>& types) { types[7].TypeIndex = 1; });

    ASSERT_EQ(nestedEnvelope.VertexInputRecords.Size, sizeof(shader::WireVertexInputRecord));
    vector<shader::WireVertexInputRecord> vertexInputs(1);
    std::memcpy(
        vertexInputs.data(),
        nested.data() + nestedEnvelope.VertexInputRecords.Offset,
        nestedEnvelope.VertexInputRecords.Size);
    vertexInputs.front().ComponentCount = 0;
    vector<byte> invalidVertexInput = nested;
    std::memcpy(
        invalidVertexInput.data() + nestedEnvelope.VertexInputRecords.Offset,
        vertexInputs.data(),
        nestedEnvelope.VertexInputRecords.Size);

    auto diagnosticResult22 = DecodeShaderArtifact(
        invalidVertexInput,
        ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::DXIL,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(
                nestedFixtureIndex.value(), shader::ShaderTarget::DXIL),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
    EXPECT_FALSE(
        diagnosticResult22.Succeeded());
    EXPECT_NE(diagnosticResult22.Diagnostics.find("vertex input"), string::npos);

    const auto expectInvalidRootOwner = [&](auto&& mutate) {
        vector<byte> mutated = original;
        vector<shader::WireRootConstantRecord> records(
            envelope.RootConstantRecords.Size /
            sizeof(shader::WireRootConstantRecord));
        std::memcpy(
            records.data(),
            original.data() + envelope.RootConstantRecords.Offset,
            envelope.RootConstantRecords.Size);
        mutate(records.front());
        std::memcpy(
            mutated.data() + envelope.RootConstantRecords.Offset,
            records.data(),
            envelope.RootConstantRecords.Size);

        auto diagnosticResult23 = decode(mutated, 0);
        EXPECT_FALSE(diagnosticResult23.Succeeded());
        EXPECT_NE(diagnosticResult23.Diagnostics.find("root constant"), string::npos);
    };
    expectInvalidRootOwner([](shader::WireRootConstantRecord& constant) {
        constant.TypeIndex = 2;
    });
    expectInvalidRootOwner([](shader::WireRootConstantRecord& constant) {
        constant.TypeIndex = 1;
    });
    expectInvalidRootOwner([](shader::WireRootConstantRecord& constant) {
        constant.TypeIndex = 0;
        constant.Size = 12;
    });

    vector<shader::WireRootConstantRecord> constants(1);
    std::memcpy(
        constants.data(),
        original.data() + envelope.RootConstantRecords.Offset,
        envelope.RootConstantRecords.Size);
    constants.front().Size = 2;
    vector<byte> invalidRoot = original;
    std::memcpy(
        invalidRoot.data() + envelope.RootConstantRecords.Offset,
        constants.data(),
        envelope.RootConstantRecords.Size);

    auto diagnosticResult24 = decode(invalidRoot, 0);
    EXPECT_FALSE(diagnosticResult24.Succeeded());
    EXPECT_NE(diagnosticResult24.Diagnostics.find("root constant"), string::npos);
}

TEST(RadRayRenderShaderArtifact, BackendPreparationPreservesDecoderDiagnostic) {
    const ShaderArtifactDecodeOptions options{.Target = shader::ShaderTarget::DXIL};
    const auto decoded = DecodeDxilShaderArtifact({}, options);
    const auto prepared = PrepareBackendShaderArtifact(RenderBackend::D3D12, {}, options, {});
    const auto hash = ResolveBackendLayoutHash(RenderBackend::D3D12, {}, options, {});
    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_FALSE(prepared.Succeeded());
    EXPECT_FALSE(hash.Succeeded());
    EXPECT_FALSE(decoded.Diagnostics.empty());
    EXPECT_EQ(prepared.Diagnostics, decoded.Diagnostics);
    EXPECT_EQ(hash.Diagnostics, decoded.Diagnostics);

    const auto mismatch = PrepareBackendShaderArtifact(RenderBackend::Vulkan, {}, options, {});
    EXPECT_FALSE(mismatch.Succeeded());
    EXPECT_NE(mismatch.Diagnostics.find("target"), string::npos);
}

}  // namespace
}  // namespace radray::render
