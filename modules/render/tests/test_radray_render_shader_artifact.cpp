#include <radray/render/backend/pipeline_layout_types.h>
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
using shader::ShaderArtifactDecodeError;
using shader::ShaderArtifactDecodeOptions;
using shader::ShaderArtifactView;
using shader::SpirvShaderArtifactView;

// The identity moves with the wire schema, so every fixture decode names the same constant instead
// of repeating the literal and drifting apart from it.
constexpr uint64_t kFixtureToolchainIdentity = 0x0000000001090211ull;

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

std::optional<ShaderArtifactView> DecodeFixtureArtifact(
    std::string_view name,
    shader::ShaderTarget target,
    ShaderArtifactDecodeError* error) {
    const vector<byte> blob = ReadFixtureArtifact(name, target);
    if (blob.empty()) {
        return std::nullopt;
    }
    return DecodeShaderArtifact(blob, FixtureDecodeOptions(name, target), error);
}

// Neither backend has an adapter step any more: the resolved layout is what native creation
// consumes, so that is what these assertions read. Modifiers stay empty here: these fixtures pin
// what the compiler published, not what a pipeline asks for on top of it.
std::optional<ResolvedD3D12Layout> ResolveFixtureD3D12Layout(
    std::string_view name,
    ShaderArtifactDecodeError* error) {
    const vector<byte> blob = ReadFixtureArtifact(name, shader::ShaderTarget::DXIL);
    if (blob.empty()) {
        return std::nullopt;
    }
    const auto artifact = DecodeDxilShaderArtifact(
        blob,
        FixtureDecodeOptions(name, shader::ShaderTarget::DXIL),
        error);
    return artifact.has_value() ? ResolveD3D12Layout(artifact.value()) : std::nullopt;
}

std::optional<ResolvedVulkanLayout> ResolveFixtureVulkanLayout(
    std::string_view name,
    ShaderArtifactDecodeError* error) {
    const vector<byte> blob = ReadFixtureArtifact(name, shader::ShaderTarget::SPIRV);
    if (blob.empty()) {
        return std::nullopt;
    }
    const auto artifact = DecodeSpirvShaderArtifact(
        blob,
        FixtureDecodeOptions(name, shader::ShaderTarget::SPIRV),
        error);
    if (!artifact.has_value()) {
        return std::nullopt;
    }
    return ResolveVulkanLayout(artifact.value());
}

vector<byte> MakeSyntheticArtifact() {
    constexpr uint32_t entryCount = 2;
    constexpr uint32_t bindingCount = 2;
    constexpr uint32_t typeCount = 1;
    constexpr uint32_t rootConstantCount = 1;
    constexpr std::string_view names[] = {
        "VSMain", "PSMain", "Color", "Linear", "Constants", "PushBlock"};
    constexpr uint32_t nameBytes = 6 + 6 + 5 + 6 + 9 + 9;
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
        {nextName(names[4]), 0xffffffffu, 4, 1, 0, 16, 16, 0}};
    // A push block is keyed on its declaration name now, so the record carries one.
    vector<shader::WireRootConstantRecord> rootConstants{
        {.Name = nextName(names[5]), .RegisterSpace = 0, .Register = 0, .Offset = 0, .Size = 16,
         .StageMask = pixelStage}};
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
            ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
            const auto artifact = DecodeShaderArtifact(
                blob,
                ShaderArtifactDecodeOptions{
                    .Target = target,
                    .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index, target),
                    .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
                &error);
            ASSERT_TRUE(artifact.has_value()) << static_cast<uint32_t>(error);
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
                const auto typed = DecodeDxilShaderArtifact(
                    blob,
                    ShaderArtifactDecodeOptions{
                        .Target = target,
                        .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index, target),
                        .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
                    &error);
                ASSERT_TRUE(typed.has_value()) << static_cast<uint32_t>(error);
                const auto resolved = ResolveD3D12Layout(typed.value());
                ASSERT_TRUE(resolved.has_value());
                resolvedBindingCount = resolved->Bindings.size();
                resolvedPushCount = resolved->PushConstants.size();
            } else {
                const auto typed = DecodeSpirvShaderArtifact(
                    blob,
                    ShaderArtifactDecodeOptions{
                        .Target = target,
                        .ExpectedGpuArtifact = test::ExpectedGpuArtifact(index, target),
                        .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
                    &error);
                ASSERT_TRUE(typed.has_value()) << static_cast<uint32_t>(error);
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
                                                 : target == shader::ShaderTarget::DXIL ? rootFactCount : 0u;
            ASSERT_EQ(artifact->RootConstants().size(), expectedRootCount);
            EXPECT_EQ(resolvedPushCount.value(), expectedRootCount);
            if (expectedRootCount != 0) {
                for (size_t rootIndex = 0; rootIndex < expectedRootCount; ++rootIndex) {
                    const test::FixtureBindingFact& expected = fixture.Bindings[
                        std::find_if(
                            fixture.Bindings.begin(),
                            fixture.Bindings.end(),
                            [](const test::FixtureBindingFact& value) noexcept {
                                return value.Kind == test::FixtureResourceKind::RootConstant;
                            }) -
                        fixture.Bindings.begin() + rootIndex];
                    const shader::WireRootConstantRecord& root = artifact->RootConstants()[rootIndex];
                    EXPECT_EQ(root.RegisterSpace, expected.D3D12Group);
                    EXPECT_EQ(root.Register, expected.D3D12Binding);
                    EXPECT_EQ(root.StageMask, expected.StageMask);
                    EXPECT_NE(root.Size, 0u);
                    if (fixture.HasSingleSpirvPushBlock) {
                        EXPECT_EQ(root.Flags & 1u, 1u);
                    }
                }
            }
        }
    }
}

TEST(RadRayRenderShaderArtifact, PolicyStaticSamplerLandsPerTargetWithoutLosingState) {
    ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
    const vector<byte> dxilBlob = ReadFixtureArtifact("shadow_static_sampler", shader::ShaderTarget::DXIL);
    ASSERT_FALSE(dxilBlob.empty());
    const auto dxil = DecodeDxilShaderArtifact(
        dxilBlob,
        FixtureDecodeOptions("shadow_static_sampler", shader::ShaderTarget::DXIL),
        &error);
    ASSERT_TRUE(dxil.has_value()) << static_cast<uint32_t>(error);

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
    const auto spirv = DecodeSpirvShaderArtifact(
        spirvBlob,
        FixtureDecodeOptions("shadow_static_sampler", shader::ShaderTarget::SPIRV),
        &error);
    ASSERT_TRUE(spirv.has_value()) << static_cast<uint32_t>(error);

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
        ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
        const auto artifact = DecodeFixtureArtifact("nested_types", target, &error);
        ASSERT_TRUE(artifact.has_value()) << static_cast<uint32_t>(error);
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
    ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
    const auto artifact = DecodeFixtureArtifact(
        "multiple_root_constants",
        shader::ShaderTarget::DXIL,
        &error);
    ASSERT_TRUE(artifact.has_value()) << static_cast<uint32_t>(error);
    ASSERT_EQ(artifact->RootConstants().size(), 2u);
    EXPECT_EQ(artifact->RootConstants()[0].RegisterSpace, 0u);
    EXPECT_EQ(artifact->RootConstants()[0].Register, 0u);
    EXPECT_EQ(artifact->RootConstants()[0].Size, 64u);
    EXPECT_EQ(artifact->RootConstants()[1].RegisterSpace, 0u);
    EXPECT_EQ(artifact->RootConstants()[1].Register, 1u);
    EXPECT_EQ(artifact->RootConstants()[1].Size, 16u);

    const auto layout = ResolveFixtureD3D12Layout("multiple_root_constants", &error);
    ASSERT_TRUE(layout.has_value()) << static_cast<uint32_t>(error);
    ASSERT_EQ(layout->PushConstants.size(), 2u);
    EXPECT_EQ(layout->PushConstants[0].Register, 0u);
    EXPECT_EQ(layout->PushConstants[0].Size, 64u);
    EXPECT_EQ(layout->PushConstants[1].Register, 1u);
    EXPECT_EQ(layout->PushConstants[1].Size, 16u);
}

TEST(RadRayRenderShaderArtifact, SingleSpirvPushBlockDoesNotBecomeMultipleConstants) {
    ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
    const auto dxil = DecodeFixtureArtifact("spirv_push_constant", shader::ShaderTarget::DXIL, &error);
    ASSERT_TRUE(dxil.has_value()) << static_cast<uint32_t>(error);
    EXPECT_TRUE(dxil->RootConstants().empty());

    const auto spirv = DecodeFixtureArtifact("spirv_push_constant", shader::ShaderTarget::SPIRV, &error);
    ASSERT_TRUE(spirv.has_value()) << static_cast<uint32_t>(error);
    ASSERT_EQ(spirv->RootConstants().size(), 1u);
    EXPECT_EQ(spirv->RootConstants()[0].RegisterSpace, 0u);
    EXPECT_EQ(spirv->RootConstants()[0].Register, 0u);
    EXPECT_EQ(spirv->RootConstants()[0].Size, 16u);
    EXPECT_EQ(spirv->RootConstants()[0].Flags & 1u, 1u);

    const auto layout = ResolveFixtureVulkanLayout("spirv_push_constant", &error);
    ASSERT_TRUE(layout.has_value()) << static_cast<uint32_t>(error);
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
        ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
        const auto artifact = DecodeFixtureArtifact("nested_types", target, &error);
        ASSERT_TRUE(artifact.has_value()) << static_cast<uint32_t>(error);
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

    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, original.data(), sizeof(envelope));
    ASSERT_EQ(envelope.TypeRecords.Size, 8u * sizeof(shader::WireTypeRecord));
    vector<shader::WireTypeRecord> types(8);
    std::memcpy(types.data(), original.data() + envelope.TypeRecords.Offset, envelope.TypeRecords.Size);
    types[6].Offset = 16;
    vector<byte> mutated = original;
    std::memcpy(mutated.data() + envelope.TypeRecords.Offset, types.data(), envelope.TypeRecords.Size);

    ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
    const auto artifact = DecodeDxilShaderArtifact(
        mutated,
        ShaderArtifactDecodeOptions{
            .Target = shader::ShaderTarget::DXIL,
            .ExpectedGpuArtifact = test::ExpectedGpuArtifact(8, shader::ShaderTarget::DXIL),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
        &error);
    ASSERT_TRUE(artifact.has_value()) << static_cast<uint32_t>(error);
    EXPECT_EQ(artifact->Generic().Envelope().GpuArtifact, test::ExpectedGpuArtifact(8, shader::ShaderTarget::DXIL));
}

TEST(RadRayRenderShaderArtifact, FailsClosedForIdentityAndWireCorruption) {
    const std::filesystem::path path = std::filesystem::path{RADRAY_PROJECT_DIR} /
                                       "modules/render/tests/data/shader_artifacts/no_resource_graphics.dxil.bin";
    const shader::ShaderTarget target = shader::ShaderTarget::DXIL;
    const shader::GpuArtifactHash expected = test::ExpectedGpuArtifact(0, target);
    const vector<byte> original = ReadBinary(path);
    ASSERT_FALSE(original.empty());

    auto decode = [&](const vector<byte>& blob,
                      uint64_t toolchain = 0,
                      ShaderArtifactDecodeError* error = nullptr) {
        return DecodeShaderArtifact(
            blob,
            ShaderArtifactDecodeOptions{
                .Target = target,
                .ExpectedGpuArtifact = expected,
                .ExpectedToolchainIdentity = toolchain},
            error);
    };
    ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(decode(original, 0xdeadbeefull, &error).has_value());
    EXPECT_EQ(error, ShaderArtifactDecodeError::ToolchainMismatch);
    EXPECT_FALSE(decode(vector<byte>{original.begin(), original.begin() + sizeof(shader::WireMetadataEnvelope) - 1}).has_value());

    auto badMagic = original;
    auto* magic = reinterpret_cast<uint32_t*>(badMagic.data());
    *magic = 0;
    EXPECT_FALSE(decode(badMagic).has_value());

    auto badRange = original;
    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, badRange.data(), sizeof(envelope));
    envelope.Bytecode.Offset = envelope.TotalSize - 1;
    std::memcpy(badRange.data(), &envelope, sizeof(envelope));
    EXPECT_FALSE(decode(badRange).has_value());

}

TEST(RadRayRenderShaderArtifact, RejectsDuplicateAndMalformedRecords) {
    const vector<byte> original = MakeSyntheticArtifact();
    const auto decode = [&](const vector<byte>& blob, uint8_t fixture, ShaderArtifactDecodeError* error) {
        return DecodeShaderArtifact(
            blob,
            ShaderArtifactDecodeOptions{
                .Target = shader::ShaderTarget::DXIL,
                .ExpectedGpuArtifact = ExpectedArtifact(fixture, shader::ShaderTarget::DXIL)},
            error);
    };

    shader::WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, original.data(), sizeof(envelope));
    auto spirvRootSignature = original;
    envelope.Target = static_cast<uint8_t>(shader::ShaderTarget::SPIRV);
    envelope.GpuArtifact = ExpectedArtifact(0, shader::ShaderTarget::SPIRV);
    envelope.RootSignature = envelope.Bytecode;
    std::memcpy(spirvRootSignature.data(), &envelope, sizeof(envelope));
    ShaderArtifactDecodeError rootError = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(DecodeShaderArtifact(
                     spirvRootSignature,
                     ShaderArtifactDecodeOptions{
                         .Target = shader::ShaderTarget::SPIRV,
                         .ExpectedGpuArtifact = ExpectedArtifact(0, shader::ShaderTarget::SPIRV)},
                     &rootError)
                     .has_value());
    EXPECT_EQ(rootError, ShaderArtifactDecodeError::InvalidRootSignature);

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
    ShaderArtifactDecodeError error = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(decode(duplicateEntry, 0, &error).has_value());
    EXPECT_EQ(error, ShaderArtifactDecodeError::DuplicateEntry);

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
    error = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(decode(duplicateBinding, 0, &error).has_value());
    EXPECT_EQ(error, ShaderArtifactDecodeError::DuplicateBinding);

    vector<shader::WireTypeRecord> types(1);
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
    error = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(decode(invalidType, 0, &error).has_value());
    EXPECT_EQ(error, ShaderArtifactDecodeError::InvalidTypeRecord);

    const auto expectInvalidSyntheticType = [&](auto&& mutate) {
        vector<byte> mutated = original;
        vector<shader::WireTypeRecord> records(1);
        std::memcpy(
            records.data(),
            original.data() + envelope.TypeRecords.Offset,
            envelope.TypeRecords.Size);
        mutate(records.front());
        std::memcpy(
            mutated.data() + envelope.TypeRecords.Offset,
            records.data(),
            envelope.TypeRecords.Size);
        ShaderArtifactDecodeError localError = ShaderArtifactDecodeError::None;
        EXPECT_FALSE(decode(mutated, 0, &localError).has_value());
        EXPECT_EQ(localError, ShaderArtifactDecodeError::InvalidTypeRecord);
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
                .ExpectedGpuArtifact = test::ExpectedGpuArtifact(8, shader::ShaderTarget::DXIL),
                .ExpectedToolchainIdentity = kFixtureToolchainIdentity});
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
    error = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(
        DecodeShaderArtifact(
            invalidVertexInput,
            ShaderArtifactDecodeOptions{
                .Target = shader::ShaderTarget::DXIL,
                .ExpectedGpuArtifact = test::ExpectedGpuArtifact(8, shader::ShaderTarget::DXIL),
            .ExpectedToolchainIdentity = kFixtureToolchainIdentity},
            &error)
            .has_value());
    EXPECT_EQ(error, ShaderArtifactDecodeError::InvalidVertexInput);

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
    error = ShaderArtifactDecodeError::None;
    EXPECT_FALSE(decode(invalidRoot, 0, &error).has_value());
    EXPECT_EQ(error, ShaderArtifactDecodeError::InvalidRootConstant);
}

}  // namespace
}  // namespace radray::render
