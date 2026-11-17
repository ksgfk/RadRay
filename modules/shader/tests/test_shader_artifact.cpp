#include <radray/shader/shader_compiler_contract.h>
#include <radray/shader/shader_artifact.h>
#include <radray/gpu/types.h>

#include "layout_test_data.h"

#include <cstring>
#include <filesystem>
#include <fstream>

#include <radray/logger.h>
#include <gtest/gtest.h>

namespace radray::shader {
namespace {

constexpr GpuArtifactHash kSyntheticArtifact{{0x5a, 0x01}};
constexpr uint32_t kBytecodeSize = 4;
constexpr uint32_t kVertexBit = 1u << static_cast<uint8_t>(ShaderStage::Vertex);
constexpr uint32_t kPixelBit = 1u << static_cast<uint8_t>(ShaderStage::Pixel);

template <typename T>
WireBlobRange AppendRecords(vector<byte>& blob, const vector<T>& records) {
    const auto stride = [&]() { if constexpr (std::is_same_v<T, byte>) return 1u; else return GetShaderMetadataRecordSize(T{}); }();
    const WireBlobRange range{static_cast<uint32_t>(blob.size()), static_cast<uint32_t>(records.size() * stride)};
    blob.resize(blob.size() + range.Size);
    if constexpr (std::is_same_v<T, byte>) {
        std::copy(records.begin(), records.end(), blob.begin() + range.Offset);
    } else {
        for (size_t i = 0; i < records.size(); ++i)
            EXPECT_TRUE(EncodeShaderMetadataRecord(records[i], std::span<byte>{blob}.subspan(range.Offset + i * stride, stride)));
    }
    return range;
}

template <typename T, typename Member>
vector<T> RebaseNames(vector<T> records, Member member) {
    for (T& record : records) {
        (record.*member).Offset += static_cast<uint32_t>(sizeof(WireMetadataEnvelope));
    }
    return records;
}

// Hand-built artifact for decoder rules. Record names are offsets into a pool that Encode places
// right after the header.
struct SyntheticArtifact {
    ShaderTarget Target{ShaderTarget::DXIL};
    uint64_t ToolchainIdentity{kShaderToolchainIdentity};
    vector<ShaderArtifactEntryRecord> Entries{};
    vector<ShaderArtifactBindingRecord> Bindings{};
    vector<ShaderArtifactTypeRecord> Types{};
    vector<ShaderArtifactRootConstantRecord> RootConstants{};
    vector<ShaderArtifactVertexInputRecord> VertexInputs{};
    vector<ShaderArtifactSamplerRecord> Samplers{};
    vector<byte> RootSignature{};
    string Names{};

    WireBlobRange Name(std::string_view text) {
        const WireBlobRange range{static_cast<uint32_t>(Names.size()), static_cast<uint32_t>(text.size())};
        Names.append(text);
        return range;
    }

    void AddEntry(std::string_view name, ShaderStage stage) {
        Entries.push_back({.Name = Name(name), .Stage = static_cast<uint8_t>(stage), .InterfaceOffset = 0, .InterfaceSize = kBytecodeSize});
    }

    ShaderArtifactBindingRecord& AddBinding(std::string_view name, gpu::BindingKind kind, uint32_t group, uint32_t binding) {
        return Bindings.emplace_back(ShaderArtifactBindingRecord{
            .Name = Name(name),
            .Group = group,
            .Binding = binding,
            .Type = EncodeBindingKind(kind).value(),
            .Count = 1,
            .StageMask = kPixelBit,
            .BufferByteSize = kind == gpu::BindingKind::CBuffer ? 16u : 0u});
    }

    // Struct records get no scalar shape; every other kind is a float with one component per 4 bytes.
    uint32_t AddType(std::string_view name, ShaderTypeKind kind, uint32_t size, uint32_t parent = kShaderNoType, uint32_t offset = 0) {
        const bool aggregate = kind == ShaderTypeKind::Struct;
        Types.push_back({.Name = Name(name),
                         .ParentIndex = parent,
                         .Kind = static_cast<uint32_t>(kind),
                         .ElementCount = 1,
                         .Offset = offset,
                         .Size = size,
                         .Stride = 0,
                         .ScalarKind = static_cast<uint32_t>(aggregate ? ShaderScalarKind::None : ShaderScalarKind::Float),
                         .RowCount = aggregate ? 0u : 1u,
                         .ColumnCount = aggregate ? 0u : size / 4,
                         .ElementKind = static_cast<uint32_t>(kind == ShaderTypeKind::Array ? ShaderTypeKind::Vector : kind),
                         .ElementSize = size});
        return static_cast<uint32_t>(Types.size() - 1);
    }

    ShaderArtifactRootConstantRecord& AddRootConstant(std::string_view name, uint32_t size) {
        return RootConstants.emplace_back(ShaderArtifactRootConstantRecord{.Name = Name(name), .Size = size, .StageMask = kVertexBit});
    }

    ShaderArtifactVertexInputRecord& AddVertexInput(std::string_view semantic, uint32_t location) {
        return VertexInputs.emplace_back(ShaderArtifactVertexInputRecord{
            .Semantic = Name(semantic),
            .Location = location,
            .ComponentType = static_cast<uint32_t>(ShaderVertexComponentType::Float),
            .ComponentCount = 3});
    }

    vector<byte> Encode() const {
        WireMetadataEnvelope envelope{};
        envelope.Target = static_cast<uint8_t>(Target);
        for (const auto& entry : Entries) envelope.StageMask |= uint8_t{1} << entry.Stage;
        envelope.ToolchainIdentity = ToolchainIdentity;
        envelope.GpuArtifact = kSyntheticArtifact;
        vector<byte> blob(sizeof(envelope));
        const auto* names = reinterpret_cast<const byte*>(Names.data());
        blob.insert(blob.end(), names, names + Names.size());
        envelope.EntryRecords = AppendRecords(blob, RebaseNames(Entries, &ShaderArtifactEntryRecord::Name));
        envelope.BindingRecords = AppendRecords(blob, RebaseNames(Bindings, &ShaderArtifactBindingRecord::Name));
        envelope.TypeRecords = AppendRecords(blob, RebaseNames(Types, &ShaderArtifactTypeRecord::Name));
        envelope.RootConstantRecords = AppendRecords(blob, RebaseNames(RootConstants, &ShaderArtifactRootConstantRecord::Name));
        envelope.VertexInputRecords = AppendRecords(blob, RebaseNames(VertexInputs, &ShaderArtifactVertexInputRecord::Semantic));
        envelope.SamplerRecords = AppendRecords(blob, Samplers);
        envelope.RootSignature = AppendRecords(blob, RootSignature);
        envelope.Bytecode = AppendRecords(blob, vector<byte>(kBytecodeSize, byte{0xcc}));
        envelope.TotalSize = static_cast<uint32_t>(blob.size());
        EncodeShaderMetadataEnvelope(envelope, std::span<byte>{blob}.first(152));
        return blob;
    }
};

SyntheticArtifact MakeArtifact(ShaderTarget target = ShaderTarget::DXIL) {
    SyntheticArtifact artifact{.Target = target};
    artifact.AddEntry("VSMain", ShaderStage::Vertex);
    return artifact;
}

ShaderArtifactDecodeOptions SyntheticOptions(ShaderTarget target) {
    return {.Target = target, .ExpectedGpuArtifact = kSyntheticArtifact, .ExpectedToolchainIdentity = kShaderToolchainIdentity};
}

DiagnosticResult<ShaderArtifactView> Decode(const SyntheticArtifact& artifact) {
    return DecodeShaderArtifact(artifact.Encode(), SyntheticOptions(artifact.Target));
}

template <typename Mutate>
vector<byte> PatchEnvelope(vector<byte> blob, Mutate&& mutate) {
    WireMetadataEnvelope envelope{};
    DecodeShaderMetadataEnvelope(std::span<const byte>{blob}.first(152), envelope);
    mutate(envelope);
    EncodeShaderMetadataEnvelope(envelope, std::span<byte>{blob}.first(152));
    return blob;
}

// Real compiler output, generated ahead of time by tools/compile_test_shaders.py. The decoder is
// trusted with the envelope's own GPU artifact hash because these tests check decoding, not identity.
DiagnosticResult<ShaderArtifactView> DecodeFixture(std::string_view name, ShaderTarget target) {
    const auto path = std::filesystem::path{RADRAY_TEST_SHADER_DIR} / "shader_artifacts" /
                      (string{name} + (target == ShaderTarget::DXIL ? ".dxil.bin" : ".spirv.bin"));
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return {std::nullopt, string{"missing fixture " + path.string()}};
    }
    vector<byte> blob(static_cast<size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
    WireMetadataEnvelope envelope{};
    if (blob.size() >= sizeof(envelope)) {
        DecodeShaderMetadataEnvelope(std::span<const byte>{blob}.first(152), envelope);
    }
    return DecodeShaderArtifact(blob, {.Target = target, .ExpectedGpuArtifact = envelope.GpuArtifact, .ExpectedToolchainIdentity = kShaderToolchainIdentity});
}

std::string_view NameOf(const ShaderArtifactView& artifact, WireBlobRange range) {
    return artifact.GetName(range).value_or(std::string_view{});
}

// --- Envelope and identity ---

TEST(RadRayShaderArtifact, DecodesMinimalArtifact) {
    const auto decoded = Decode(MakeArtifact());

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    ASSERT_EQ(decoded.Value->Entries().size(), 1u);
    EXPECT_EQ(NameOf(*decoded.Value, decoded.Value->Entries()[0].Name), "VSMain");
    EXPECT_EQ(decoded.Value->Bytecode().size(), kBytecodeSize);
    EXPECT_TRUE(decoded.Value->SerializedRootSignature().empty());
}

TEST(RadRayShaderArtifact, CompilerUsesCompactMetadataRecords) {
    const auto artifact = DecodeFixture("layout_constants", ShaderTarget::SPIRV);
    ASSERT_TRUE(artifact.Succeeded()) << artifact.Diagnostics;
    EXPECT_EQ(artifact.Value->Envelope().EntryRecords.Size, artifact.Value->Entries().size() * 20);
    EXPECT_EQ(artifact.Value->Envelope().BindingRecords.Size, artifact.Value->Bindings().size() * 36);
    EXPECT_EQ(artifact.Value->Envelope().TypeRecords.Size, artifact.Value->Types().size() * 44);
}

TEST(RadRayShaderArtifact, RejectsTruncatedBlob) {
    vector<byte> blob = MakeArtifact().Encode();
    blob.resize(sizeof(WireMetadataEnvelope) - 1);

    const auto decoded = DecodeShaderArtifact(blob, SyntheticOptions(ShaderTarget::DXIL));

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("truncated"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsRetiredSchema) {
    const auto blob = PatchEnvelope(MakeArtifact().Encode(), [](WireMetadataEnvelope& envelope) { envelope.SchemaVersion = 8; });

    const auto decoded = DecodeShaderArtifact(blob, SyntheticOptions(ShaderTarget::DXIL));

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("schema"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsRequestedTargetMismatch) {
    const auto decoded = DecodeShaderArtifact(MakeArtifact(ShaderTarget::DXIL).Encode(), SyntheticOptions(ShaderTarget::SPIRV));

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_FALSE(decoded.Diagnostics.empty());
}

TEST(RadRayShaderArtifact, RejectsUnexpectedGpuArtifact) {
    ShaderArtifactDecodeOptions options = SyntheticOptions(ShaderTarget::DXIL);
    options.ExpectedGpuArtifact.Bytes[15] ^= 1;

    const auto decoded = DecodeShaderArtifact(MakeArtifact().Encode(), options);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_FALSE(decoded.Diagnostics.empty());
}

TEST(RadRayShaderArtifact, RejectsToolchainIdentityMismatch) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.ToolchainIdentity = kShaderToolchainIdentity + 1;

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("toolchain"), string::npos);
}

TEST(RadRayShaderArtifact, ZeroExpectedToolchainIdentitySkipsCheck) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.ToolchainIdentity = 0x1234;
    ShaderArtifactDecodeOptions options = SyntheticOptions(ShaderTarget::DXIL);
    options.ExpectedToolchainIdentity = 0;

    const auto decoded = DecodeShaderArtifact(artifact.Encode(), options);

    EXPECT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
}

TEST(RadRayShaderArtifact, RejectsSectionInsideHeader) {
    const auto blob = PatchEnvelope(MakeArtifact().Encode(), [](WireMetadataEnvelope& envelope) {
        envelope.EntryRecords.Offset = 0;
    });

    EXPECT_FALSE(DecodeShaderArtifact(blob, SyntheticOptions(ShaderTarget::DXIL)).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsOverlappingSections) {
    const auto blob = PatchEnvelope(MakeArtifact().Encode(), [](WireMetadataEnvelope& envelope) {
        envelope.VertexInputRecords = {envelope.Bytecode.Offset, envelope.Bytecode.Size};
    });

    const auto decoded = DecodeShaderArtifact(blob, SyntheticOptions(ShaderTarget::DXIL));

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("overlaps"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsRootSignatureCarrierOnSpirv) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.RootSignature = {byte{1}, byte{2}, byte{3}, byte{4}};

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsSamplerRecordsOnDxil) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::DXIL);
    artifact.Samplers.push_back({});

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, TypedDecoderRejectsOtherTargetOption) {
    const vector<byte> dxil = MakeArtifact(ShaderTarget::DXIL).Encode();
    const vector<byte> spirv = MakeArtifact(ShaderTarget::SPIRV).Encode();

    EXPECT_FALSE(DecodeDxilShaderArtifact(dxil, SyntheticOptions(ShaderTarget::SPIRV)).Succeeded());
    EXPECT_FALSE(DecodeSpirvShaderArtifact(spirv, SyntheticOptions(ShaderTarget::DXIL)).Succeeded());
}

TEST(RadRayShaderArtifact, TypedDecoderKeepsGenericDiagnostics) {
    const auto generic = DecodeShaderArtifact({}, SyntheticOptions(ShaderTarget::DXIL));
    const auto dxil = DecodeDxilShaderArtifact({}, SyntheticOptions(ShaderTarget::DXIL));

    EXPECT_FALSE(dxil.Succeeded());
    EXPECT_EQ(dxil.Diagnostics, generic.Diagnostics);
}

// --- Entries ---

TEST(RadRayShaderArtifact, FindStageBytecodeReturnsEntrySlice) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.Entries[0].InterfaceOffset = 1;
    artifact.Entries[0].InterfaceSize = 2;

    const auto decoded = Decode(artifact);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    const auto vertex = decoded.Value->FindStageBytecode(ShaderStage::Vertex);
    ASSERT_TRUE(vertex.has_value());
    EXPECT_EQ(vertex->data(), decoded.Value->Bytecode().data() + 1);
    EXPECT_EQ(vertex->size(), 2u);
    EXPECT_FALSE(decoded.Value->FindStageBytecode(ShaderStage::Pixel).has_value());
}

TEST(RadRayShaderArtifact, RejectsDuplicateEntryNames) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddEntry("VSMain", ShaderStage::Pixel);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("duplicate entry"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsEntryOutsideBytecode) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.Entries[0].InterfaceOffset = 1;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, GetNameRejectsRangeOutsideBlob) {
    const auto decoded = Decode(MakeArtifact());

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    EXPECT_FALSE(decoded.Value->GetName({decoded.Value->Envelope().TotalSize, 1}).has_value());
}

// --- Bindings ---

TEST(RadRayShaderArtifact, FindBindingLooksUpDeclarationName) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 1, 3);

    const auto decoded = Decode(artifact);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    const auto binding = decoded.Value->FindBinding("Albedo");
    ASSERT_TRUE(binding.has_value());
    EXPECT_EQ(binding->Name, "Albedo");
    EXPECT_EQ(binding->Record.Group, 1u);
    EXPECT_EQ(binding->Record.Binding, 3u);
    EXPECT_FALSE(decoded.Value->FindBinding("Missing").has_value());
}

TEST(RadRayShaderArtifact, DxilAllowsSameRegisterInDifferentNamespaces) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::DXIL);
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0);
    artifact.AddBinding("Linear", gpu::BindingKind::Sampler, 0, 0);

    const auto decoded = Decode(artifact);

    EXPECT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
}

TEST(RadRayShaderArtifact, DxilRejectsSameRegisterInSameNamespace) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::DXIL);
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0);
    artifact.AddBinding("Normals", gpu::BindingKind::StructuredBuffer, 0, 0);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("duplicate binding"), string::npos);
}

TEST(RadRayShaderArtifact, SpirvRejectsSameBindingAcrossKinds) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0);
    artifact.AddBinding("Linear", gpu::BindingKind::Sampler, 0, 0);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("duplicate binding"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsDuplicateBindingNames) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0);
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 1);

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, BindingKindsUseSchema10CodesInBothDirections) {
    using Gpu = gpu::BindingKind;
    constexpr std::pair<uint32_t, Gpu> cases[]{
        {1, Gpu::CBuffer},
        {2, Gpu::TypedBuffer},
        {3, Gpu::RWTypedBuffer},
        {4, Gpu::StructuredBuffer},
        {5, Gpu::RWStructuredBuffer},
        {6, Gpu::RawBuffer},
        {7, Gpu::RWRawBuffer},
        {8, Gpu::Texture},
        {9, Gpu::RWTexture},
        {10, Gpu::Sampler},
    };
    for (const auto& [wireCode, kind] : cases) {
        EXPECT_EQ(DecodeBindingKind(wireCode), kind) << wireCode;
        EXPECT_EQ(EncodeBindingKind(kind), wireCode) << wireCode;
    }
    EXPECT_FALSE(DecodeBindingKind(0));
    EXPECT_FALSE(DecodeBindingKind(11));
    EXPECT_FALSE(DecodeBindingKind(UINT32_MAX));
    EXPECT_FALSE(EncodeBindingKind(static_cast<Gpu>(10)));
    EXPECT_FALSE(EncodeBindingKind(static_cast<Gpu>(UINT32_MAX)));
}

TEST(RadRayShaderArtifact, RejectsUnknownBindingKind) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0).Type = 0;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, AcceptsRootDescriptorForSingleBuffer) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Instances", gpu::BindingKind::StructuredBuffer, 0, 0).Placement = static_cast<uint32_t>(ShaderBindingPlacement::RootDescriptor);

    const auto decoded = Decode(artifact);

    EXPECT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
}

TEST(RadRayShaderArtifact, RejectsRootDescriptorForTexture) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0).Placement = static_cast<uint32_t>(ShaderBindingPlacement::RootDescriptor);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("placement"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsRootDescriptorArray) {
    SyntheticArtifact artifact = MakeArtifact();
    ShaderArtifactBindingRecord& binding = artifact.AddBinding("Instances", gpu::BindingKind::StructuredBuffer, 0, 0);
    binding.Placement = static_cast<uint32_t>(ShaderBindingPlacement::RootDescriptor);
    binding.Count = 2;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, DxilAcceptsStaticSamplerPlacement) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::DXIL);
    artifact.AddBinding("Shadow", gpu::BindingKind::Sampler, 0, 0).Placement = static_cast<uint32_t>(ShaderBindingPlacement::StaticSampler);

    const auto decoded = Decode(artifact);

    EXPECT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
}

TEST(RadRayShaderArtifact, SpirvRejectsStaticSamplerPlacement) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.AddBinding("Shadow", gpu::BindingKind::Sampler, 0, 0).Placement = static_cast<uint32_t>(ShaderBindingPlacement::StaticSampler);

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, SpirvSamplerReferencesImmutableState) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.Samplers.push_back({.MagFilter = 1, .MinFilter = 1, .CompareEnable = 1, .CompareOp = 3});
    artifact.AddBinding("Shadow", gpu::BindingKind::Sampler, 0, 0).SamplerIndex = 0;

    const auto decoded = Decode(artifact);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    const auto binding = decoded.Value->FindBinding("Shadow");
    ASSERT_TRUE(binding.has_value());
    EXPECT_EQ(decoded.Value->Samplers()[binding->Record.SamplerIndex], artifact.Samplers[0]);
}

TEST(RadRayShaderArtifact, RejectsSamplerIndexOutsideSamplerRecords) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.AddBinding("Shadow", gpu::BindingKind::Sampler, 0, 0).SamplerIndex = 0;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsSamplerIndexOnNonSampler) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.Samplers.push_back({});
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0).SamplerIndex = 0;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsSamplerStateOutsideVulkanRange) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.Samplers.push_back({.AddressModeU = kShaderSamplerMaxAddressMode + 1});

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("sampler record"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsSamplerWithInvertedLodRange) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::SPIRV);
    artifact.Samplers.push_back({.MinLod = 2.0f, .MaxLod = 1.0f});

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

// --- Payload owners and type tree ---

TEST(RadRayShaderArtifact, CBuffersMayShareOnePayloadRoot) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Shared", ShaderTypeKind::Struct, 16);
    artifact.AddType("Value", ShaderTypeKind::Vector, 16, root);
    artifact.AddBinding("First", gpu::BindingKind::CBuffer, 0, 0).TypeIndex = root;
    artifact.AddBinding("Second", gpu::BindingKind::CBuffer, 1, 0).TypeIndex = root;

    const auto decoded = Decode(artifact);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    const auto first = decoded.Value->FindBinding("First");
    const auto second = decoded.Value->FindBinding("Second");
    ASSERT_TRUE(first.has_value() && second.has_value());
    EXPECT_EQ(first->Record.TypeIndex, root);
    EXPECT_EQ(second->Record.TypeIndex, root);
}

TEST(RadRayShaderArtifact, RejectsCBufferWithoutPayloadRoot) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Constants", gpu::BindingKind::CBuffer, 0, 0);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("type owner"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsCBufferOwnedByMemberRecord) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 16);
    const uint32_t member = artifact.AddType("Value", ShaderTypeKind::Vector, 16, root);
    artifact.AddBinding("Constants", gpu::BindingKind::CBuffer, 0, 0).TypeIndex = member;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsPayloadOwnerOnNonCBuffer) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 16);
    artifact.AddType("Value", ShaderTypeKind::Vector, 16, root);
    artifact.AddBinding("Albedo", gpu::BindingKind::Texture, 0, 0).TypeIndex = root;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, ArtifactAndLayoutAgreeOnMatrixArrayAndInvalidTypeGraphs) {
    auto layout = test::MatrixLayout();
    auto check = [&](bool expected) {
        auto artifact = MakeArtifact();
        for (const auto& t : layout.Types) {
            artifact.Types.push_back({
                .Name = artifact.Name(t.Name), .ParentIndex = t.ParentIndex.value_or(kShaderNoType),
                .Kind = static_cast<uint32_t>(t.Kind), .ElementCount = t.ElementCount, .Offset = t.Offset,
                .Size = t.Size, .Stride = t.Stride, .Flags = t.RowMajor ? kShaderTypeFlagRowMajor : 0u,
                .TypeIndex = t.TypeIndex.value_or(kShaderNoType), .ScalarKind = static_cast<uint32_t>(t.ScalarKind),
                .RowCount = t.RowCount, .ColumnCount = t.ColumnCount, .ElementKind = static_cast<uint32_t>(t.ElementKind),
                .ElementSize = t.ElementSize, .MatrixStride = t.MatrixStride});
        }
        EXPECT_EQ(Decode(artifact).Succeeded(), expected);
        EXPECT_EQ(ValidateShaderPipelineLayoutDesc(layout), expected);
    };
    check(true);
    auto overlapping = layout.Types[1];
    overlapping.Name = "Other";
    layout.Types.push_back(overlapping);
    check(false);
    layout.Types.pop_back();
    layout.Types[1] = {.Name = "Child", .ParentIndex = 0, .Size = 92, .ElementSize = 92, .TypeIndex = 0};
    check(false);
}

TEST(RadRayShaderArtifact, ArtifactAndLayoutAgreeOnVertexSignatures) {
    auto artifact = MakeArtifact();
    ShaderVariantInterfaceDesc program{.Name = "Program", .DXIL = ShaderVariantTargetInterfaceDesc{.Stages = gpu::ShaderStage::Vertex}};
    auto add = [&](const char* name, uint32_t location) {
        artifact.AddVertexInput(name, location);
        program.DXIL->VertexInputs.push_back({name, 0, location, ShaderVertexComponentType::Float, 3});
    };
    add("Position", 0);
    EXPECT_TRUE(Decode(artifact).Succeeded());
    EXPECT_TRUE(ValidateShaderVariantInterfaceDesc(program));
    add("Position", 1);
    EXPECT_FALSE(Decode(artifact).Succeeded());
    EXPECT_FALSE(ValidateShaderVariantInterfaceDesc(program));
}

TEST(RadRayShaderArtifact, RejectsUnknownTypeKind) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddType("Root", ShaderTypeKind::Struct, 16);
    artifact.Types[0].Kind = 0;

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("type record"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsReservedTypeFlags) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 4);
    artifact.AddType("Value", ShaderTypeKind::Scalar, 4, root);
    auto bytes = artifact.Encode();
    WireMetadataEnvelope envelope;
    DecodeShaderMetadataEnvelope(std::span<const byte>{bytes}.first(152), envelope);
    bytes[envelope.TypeRecords.Offset + 44 + 14] |= byte{1};  // Reserved shape bit 16.
    EXPECT_FALSE(DecodeShaderArtifact(bytes, SyntheticOptions(artifact.Target)).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsMemberOutsideParent) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 16);
    artifact.AddType("Value", ShaderTypeKind::Vector, 16, root, 4);

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsOverlappingSiblings) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 32);
    artifact.AddType("First", ShaderTypeKind::Vector, 16, root, 0);
    artifact.AddType("Second", ShaderTypeKind::Vector, 16, root, 8);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("overlaps sibling"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsDuplicateSiblingNames) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 32);
    artifact.AddType("Value", ShaderTypeKind::Vector, 16, root, 0);
    artifact.AddType("Value", ShaderTypeKind::Vector, 16, root, 16);

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsCyclicParentChain) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Leaf", ShaderTypeKind::Struct, 16);
    artifact.AddType("Value", ShaderTypeKind::Vector, 16, root);
    const uint32_t first = artifact.AddType("First", ShaderTypeKind::Struct, 16, 3);
    artifact.AddType("Second", ShaderTypeKind::Struct, 16, first);
    artifact.Types[2].TypeIndex = root;
    artifact.Types[3].TypeIndex = root;

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("parent"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsArraySizeOtherThanLastElementEnd) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("Root", ShaderTypeKind::Struct, 64);
    const uint32_t array = artifact.AddType("Corners", ShaderTypeKind::Array, 64, root);
    artifact.Types[array].Stride = 16;
    artifact.Types[array].ElementCount = 3;
    artifact.Types[array].ElementSize = 16;
    artifact.Types[array].ColumnCount = 4;

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("array size"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsLegacyLeafPaddingOnBothTargets) {
    for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
        SyntheticArtifact artifact = MakeArtifact(target);
        const uint32_t root = artifact.AddType("Lighting", ShaderTypeKind::Struct, 28);
        const uint32_t padded = artifact.AddType("Direction0", ShaderTypeKind::Vector, 16, root, 0);
        artifact.Types[padded].Stride = 12;
        artifact.Types[padded].ColumnCount = 3;
        artifact.AddType("Direction1", ShaderTypeKind::Vector, 12, root, 16);

        const auto decoded = Decode(artifact);

        EXPECT_FALSE(decoded.Succeeded()) << static_cast<uint32_t>(target) << ": " << decoded.Diagnostics;
    }
}

TEST(RadRayShaderArtifact, RejectsLegacyPaddedScalarLeaf) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::DXIL);
    const uint32_t root = artifact.AddType("Lighting", ShaderTypeKind::Struct, 28);
    const uint32_t padded = artifact.AddType("Brightness", ShaderTypeKind::Scalar, 12, root, 4);
    artifact.Types[padded].Stride = 4;
    artifact.Types[padded].ColumnCount = 1;
    artifact.AddType("Tint", ShaderTypeKind::Vector, 12, root, 16);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
}

TEST(RadRayShaderArtifact, DxilRejectsPaddedMatrixLeaf) {
    SyntheticArtifact artifact = MakeArtifact(ShaderTarget::DXIL);
    const uint32_t root = artifact.AddType("Object", ShaderTypeKind::Struct, 80);
    const uint32_t matrix = artifact.AddType("Transform", ShaderTypeKind::Matrix, 80, root);
    artifact.Types[matrix].Stride = 64;
    artifact.Types[matrix].RowCount = 4;
    artifact.Types[matrix].ColumnCount = 4;

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("type record"), string::npos);
}

// --- Root constants ---

TEST(RadRayShaderArtifact, RootConstantMayOwnPayloadRoot) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("PushData", ShaderTypeKind::Struct, 16);
    artifact.AddType("Tint", ShaderTypeKind::Vector, 16, root);
    artifact.AddRootConstant("Push", 16).TypeIndex = root;

    const auto decoded = Decode(artifact);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    ASSERT_EQ(decoded.Value->RootConstants().size(), 1u);
    EXPECT_EQ(NameOf(*decoded.Value, decoded.Value->RootConstants()[0].Name), "Push");
    EXPECT_EQ(decoded.Value->RootConstants()[0].TypeIndex, root);
}

TEST(RadRayShaderArtifact, RejectsRootConstantSizeNotMultipleOfFour) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddRootConstant("Push", 6);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("root constant"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsRootConstantPayloadLargerThanRange) {
    SyntheticArtifact artifact = MakeArtifact();
    const uint32_t root = artifact.AddType("PushData", ShaderTypeKind::Struct, 16);
    artifact.AddType("Tint", ShaderTypeKind::Vector, 16, root);
    artifact.AddRootConstant("Push", 12).TypeIndex = root;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

TEST(RadRayShaderArtifact, RejectsRootConstantNamedLikeBinding) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddBinding("Shared", gpu::BindingKind::Texture, 0, 0);
    artifact.AddRootConstant("Shared", 16);

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

// --- Vertex inputs ---

TEST(RadRayShaderArtifact, RejectsVertexInputsSharingLocation) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddVertexInput("POSITION", 0);
    artifact.AddVertexInput("NORMAL", 0);

    const auto decoded = Decode(artifact);

    EXPECT_FALSE(decoded.Succeeded());
    EXPECT_NE(decoded.Diagnostics.find("vertex input"), string::npos);
}

TEST(RadRayShaderArtifact, RejectsVertexInputWithMoreThanFourComponents) {
    SyntheticArtifact artifact = MakeArtifact();
    artifact.AddVertexInput("POSITION", 0).ComponentCount = 5;

    EXPECT_FALSE(Decode(artifact).Succeeded());
}

// --- Compiler-generated fixtures ---

TEST(RadRayShaderArtifact, DecodesCompilerFixturesForBothTargets) {
    for (const std::string_view name : {"texture_sampler", "shadow_static_sampler", "spirv_push_constant", "nested_types", "compute"}) {
        for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
            SCOPED_TRACE(radray::format("{} target={}", name, static_cast<uint32_t>(target)));

            const auto decoded = DecodeFixture(name, target);

            ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
            ASSERT_FALSE(decoded.Value->Entries().empty());
            for (const ShaderArtifactEntryRecord& entry : decoded.Value->Entries()) {
                const auto stage = decoded.Value->FindStageBytecode(static_cast<ShaderStage>(entry.Stage));
                ASSERT_TRUE(stage.has_value());
                EXPECT_FALSE(stage->empty());
            }
        }
    }
}

TEST(RadRayShaderArtifact, FixtureBindingsUseTargetCoordinates) {
    struct Expected {
        ShaderTarget Target;
        uint32_t Group;
        uint32_t Binding;
    };
    for (const Expected expected : {Expected{ShaderTarget::DXIL, 0, 0}, Expected{ShaderTarget::SPIRV, 1, 7}}) {
        const auto decoded = DecodeFixture("texture_sampler", expected.Target);

        ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
        const auto texture = decoded.Value->FindBinding("AlbedoTexture");
        ASSERT_TRUE(texture.has_value());
        EXPECT_EQ(DecodeBindingKind(texture->Record.Type), gpu::BindingKind::Texture);
        EXPECT_EQ(texture->Record.Group, expected.Group);
        EXPECT_EQ(texture->Record.Binding, expected.Binding);
        EXPECT_EQ(texture->Record.StageMask, kPixelBit);
    }
}

TEST(RadRayShaderArtifact, FixtureComputeArtifactHasOnlyComputeStage) {
    const auto decoded = DecodeFixture("compute", ShaderTarget::DXIL);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    EXPECT_TRUE(decoded.Value->FindStageBytecode(ShaderStage::Compute).has_value());
    EXPECT_FALSE(decoded.Value->FindStageBytecode(ShaderStage::Vertex).has_value());
}

TEST(RadRayShaderArtifact, FixtureDxilStaticSamplerLivesInRootSignature) {
    const auto decoded = DecodeFixture("shadow_static_sampler", ShaderTarget::DXIL);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    const auto sampler = decoded.Value->FindBinding("ShadowSampler");
    ASSERT_TRUE(sampler.has_value());
    EXPECT_EQ(sampler->Record.Placement, static_cast<uint32_t>(ShaderBindingPlacement::StaticSampler));
    EXPECT_FALSE(decoded.Value->SerializedRootSignature().empty());
    EXPECT_TRUE(decoded.Value->Samplers().empty());
}

TEST(RadRayShaderArtifact, FixtureSpirvStaticSamplerKeepsFullState) {
    const auto decoded = DecodeFixture("shadow_static_sampler", ShaderTarget::SPIRV);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    const auto sampler = decoded.Value->FindBinding("ShadowSampler");
    ASSERT_TRUE(sampler.has_value());
    EXPECT_EQ(sampler->Record.Placement, static_cast<uint32_t>(ShaderBindingPlacement::Table));
    ASSERT_LT(sampler->Record.SamplerIndex, decoded.Value->Samplers().size());
    const ShaderArtifactSamplerRecord& state = decoded.Value->Samplers()[sampler->Record.SamplerIndex];
    // FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT in Vulkan terms.
    EXPECT_EQ(state.MagFilter, 1u);
    EXPECT_EQ(state.MinFilter, 1u);
    EXPECT_EQ(state.MipmapMode, 0u);
    EXPECT_EQ(state.CompareEnable, 1u);
    EXPECT_TRUE(decoded.Value->SerializedRootSignature().empty());
}

TEST(RadRayShaderArtifact, FixtureCBufferOwnsPayloadRoot) {
    for (const ShaderTarget target : {ShaderTarget::DXIL, ShaderTarget::SPIRV}) {
        const auto decoded = DecodeFixture("nested_types", target);

        ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
        const auto constants = decoded.Value->FindBinding("Constants");
        ASSERT_TRUE(constants.has_value());
        const auto types = decoded.Value->Types();
        ASSERT_LT(constants->Record.TypeIndex, types.size());
        EXPECT_EQ(NameOf(*decoded.Value, types[constants->Record.TypeIndex].Name), "NestedRoot");
        EXPECT_EQ(types[constants->Record.TypeIndex].ParentIndex, kShaderNoType);
    }
}

TEST(RadRayShaderArtifact, FixtureSpirvPushBlockOwnsPayloadRoot) {
    const auto decoded = DecodeFixture("spirv_push_constant", ShaderTarget::SPIRV);

    ASSERT_TRUE(decoded.Succeeded()) << decoded.Diagnostics;
    ASSERT_EQ(decoded.Value->RootConstants().size(), 1u);
    const ShaderArtifactRootConstantRecord& push = decoded.Value->RootConstants()[0];
    EXPECT_EQ(NameOf(*decoded.Value, push.Name), "PushConstants");
    EXPECT_EQ(push.StageMask, 1u << static_cast<uint8_t>(ShaderStage::Pixel));
    ASSERT_LT(push.TypeIndex, decoded.Value->Types().size());
    EXPECT_EQ(NameOf(*decoded.Value, decoded.Value->Types()[push.TypeIndex].Name), "PushData");
}

}  // namespace
}  // namespace radray::shader
