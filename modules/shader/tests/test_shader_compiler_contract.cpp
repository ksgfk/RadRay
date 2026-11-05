#include <radray/shader/shader_compiler_contract.h>

#include <cstring>

#include <gtest/gtest.h>

namespace radray::shader {
namespace {

constexpr GpuArtifactHash kArtifact{{0x42, 0x17}};

vector<byte> MakeEnvelope(ShaderTarget target) {
    WireMetadataEnvelope envelope{};
    envelope.Target = static_cast<uint8_t>(target);
    envelope.TotalSize = sizeof(envelope) + 4;
    envelope.Bytecode = {sizeof(envelope), 4};
    envelope.GpuArtifact = kArtifact;
    vector<byte> blob(envelope.TotalSize, byte{0});
    std::memcpy(blob.data(), &envelope, sizeof(envelope));
    return blob;
}

template <typename Mutate>
vector<byte> PatchEnvelope(vector<byte> blob, Mutate&& mutate) {
    WireMetadataEnvelope envelope{};
    std::memcpy(&envelope, blob.data(), sizeof(envelope));
    mutate(envelope);
    std::memcpy(blob.data(), &envelope, sizeof(envelope));
    return blob;
}

CompileVariantRequest MakeVariantRequest() {
    return CompileVariantRequest{
        .SourceName = "passes/forward.hlsl",
        .RootSource = {byte{1}},
        .Defines = {{"USE_SHADOWS", "1"}, {"DEBUG_VIEW", "0"}},
        .Assignments = {{"QUALITY", "high"}, {"ALPHA_TEST", "off"}},
        .Targets = ShaderTargetMask::All};
}

SourceContractRequest MakeDiscoveryRequest() {
    return SourceContractRequest{
        .SourceName = "passes/forward.hlsl",
        .RootSource = {byte{1}},
        .Defines = {{"USE_SHADOWS", "1"}, {"DEBUG_VIEW", "0"}},
        .Targets = ShaderTargetMask::All};
}

TEST(RadRayShaderContract, HashHexListsBytesInOrder) {
    Hash128 hash{};
    for (uint8_t index = 0; index < hash.Bytes.size(); ++index) {
        hash.Bytes[index] = index;
    }

    EXPECT_EQ(hash.ToHex(), "000102030405060708090a0b0c0d0e0f");
}

TEST(RadRayShaderContract, LogicalSourceNameAcceptsRelativePath) {
    EXPECT_TRUE(IsLogicalSourceName("forward.hlsl"));
    EXPECT_TRUE(IsLogicalSourceName("passes/forward.hlsl"));
}

TEST(RadRayShaderContract, LogicalSourceNameRejectsPhysicalOrNonCanonicalPath) {
    for (const std::string_view name : {"", "/passes/forward.hlsl", "\\forward.hlsl", "C:/forward.hlsl",
                                        "passes/../forward.hlsl", "passes/./forward.hlsl", "passes//forward.hlsl",
                                        "passes\\forward.hlsl"}) {
        EXPECT_FALSE(IsLogicalSourceName(name)) << name;
    }
}

TEST(RadRayShaderContract, CanonicalizeSortsDefinesAndAssignmentsByName) {
    const auto canonical = CanonicalizeCompileVariantRequest(MakeVariantRequest());

    ASSERT_TRUE(canonical.Succeeded()) << canonical.Diagnostics;
    ASSERT_EQ(canonical.Value->Defines.size(), 2u);
    EXPECT_EQ(canonical.Value->Defines[0].Name, "DEBUG_VIEW");
    EXPECT_EQ(canonical.Value->Defines[1].Name, "USE_SHADOWS");
    ASSERT_EQ(canonical.Value->Assignments.size(), 2u);
    EXPECT_EQ(canonical.Value->Assignments[0].Name, "ALPHA_TEST");
    EXPECT_EQ(canonical.Value->Assignments[1].Name, "QUALITY");
}

TEST(RadRayShaderContract, CanonicalizeRejectsNonLogicalSourceName) {
    CompileVariantRequest request = MakeVariantRequest();
    request.SourceName = "C:/passes/forward.hlsl";

    const auto canonical = CanonicalizeCompileVariantRequest(request);

    EXPECT_FALSE(canonical.Succeeded());
    EXPECT_NE(canonical.Diagnostics.find("SourceName"), string::npos);
}

TEST(RadRayShaderContract, CanonicalizeRejectsDuplicateDefineNames) {
    CompileVariantRequest request = MakeVariantRequest();
    request.Defines.push_back({"DEBUG_VIEW", "1"});

    const auto canonical = CanonicalizeCompileVariantRequest(request);

    EXPECT_FALSE(canonical.Succeeded());
    EXPECT_NE(canonical.Diagnostics.find("Defines"), string::npos);
}

TEST(RadRayShaderContract, CanonicalizeRejectsDuplicateKeywordAssignments) {
    CompileVariantRequest request = MakeVariantRequest();
    request.Assignments.push_back({"QUALITY", "low"});

    const auto canonical = CanonicalizeCompileVariantRequest(request);

    EXPECT_FALSE(canonical.Succeeded());
    EXPECT_NE(canonical.Diagnostics.find("Assignments"), string::npos);
}

TEST(RadRayShaderContract, CanonicalizeRejectsEmptyTargets) {
    CompileVariantRequest request = MakeVariantRequest();
    request.Targets = ShaderTargetMask::None;

    const auto canonical = CanonicalizeCompileVariantRequest(request);

    EXPECT_FALSE(canonical.Succeeded());
    EXPECT_NE(canonical.Diagnostics.find("Targets"), string::npos);
}

TEST(RadRayShaderContract, VariantEncodingIgnoresDefineAndAssignmentOrder) {
    CompileVariantRequest request = MakeVariantRequest();
    const auto first = EncodeCanonicalCompileVariantRequest(request);
    std::swap(request.Defines[0], request.Defines[1]);
    std::swap(request.Assignments[0], request.Assignments[1]);

    const auto reordered = EncodeCanonicalCompileVariantRequest(request);

    ASSERT_TRUE(first.Succeeded()) << first.Diagnostics;
    ASSERT_TRUE(reordered.Succeeded()) << reordered.Diagnostics;
    EXPECT_EQ(*first.Value, *reordered.Value);
}

TEST(RadRayShaderContract, VariantEncodingDependsOnSourceName) {
    CompileVariantRequest request = MakeVariantRequest();
    const auto first = EncodeCanonicalCompileVariantRequest(request);
    request.SourceName = "moved/forward.hlsl";

    const auto moved = EncodeCanonicalCompileVariantRequest(request);

    ASSERT_TRUE(first.Succeeded()) << first.Diagnostics;
    ASSERT_TRUE(moved.Succeeded()) << moved.Diagnostics;
    EXPECT_NE(*first.Value, *moved.Value);
}

TEST(RadRayShaderContract, VariantEncodingReportsCanonicalizationFailure) {
    CompileVariantRequest request = MakeVariantRequest();
    request.SourceName = "../forward.hlsl";

    const auto encoded = EncodeCanonicalCompileVariantRequest(request);

    EXPECT_FALSE(encoded.Succeeded());
    EXPECT_EQ(encoded.Diagnostics, CanonicalizeCompileVariantRequest(request).Diagnostics);
}

TEST(RadRayShaderContract, DiscoveryEncodingIgnoresDefineOrder) {
    SourceContractRequest request = MakeDiscoveryRequest();
    const auto first = EncodeSourceContractRequest(request);
    std::swap(request.Defines[0], request.Defines[1]);

    const auto reordered = EncodeSourceContractRequest(request);

    ASSERT_TRUE(first.Succeeded()) << first.Diagnostics;
    ASSERT_TRUE(reordered.Succeeded()) << reordered.Diagnostics;
    EXPECT_EQ(*first.Value, *reordered.Value);
}

TEST(RadRayShaderContract, DiscoveryEncodingRejectsEmptyRootSource) {
    SourceContractRequest request = MakeDiscoveryRequest();
    request.RootSource.clear();

    const auto encoded = EncodeSourceContractRequest(request);

    EXPECT_FALSE(encoded.Succeeded());
    EXPECT_NE(encoded.Diagnostics.find("RootSource"), string::npos);
}

TEST(RadRayShaderContract, DiscoveryEncodingRejectsDuplicateDefineNames) {
    SourceContractRequest request = MakeDiscoveryRequest();
    request.Defines.push_back({"DEBUG_VIEW", "1"});

    const auto encoded = EncodeSourceContractRequest(request);

    EXPECT_FALSE(encoded.Succeeded());
    EXPECT_NE(encoded.Diagnostics.find("Defines"), string::npos);
}

TEST(RadRayShaderContract, DiscoveryEncodingRejectsEmptyTargets) {
    SourceContractRequest request = MakeDiscoveryRequest();
    request.Targets = ShaderTargetMask::None;

    const auto encoded = EncodeSourceContractRequest(request);

    EXPECT_FALSE(encoded.Succeeded());
    EXPECT_NE(encoded.Diagnostics.find("Targets"), string::npos);
}

TEST(RadRayShaderContract, EnvelopeAcceptsMatchingTargetAndArtifact) {
    EXPECT_TRUE(ValidateWireMetadataEnvelope(MakeEnvelope(ShaderTarget::DXIL), ShaderTarget::DXIL, kArtifact));
    EXPECT_TRUE(ValidateWireMetadataEnvelope(MakeEnvelope(ShaderTarget::SPIRV), ShaderTarget::SPIRV, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsTruncatedHeader) {
    vector<byte> blob = MakeEnvelope(ShaderTarget::DXIL);
    blob.resize(sizeof(WireMetadataEnvelope) - 1);

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsWrongMagic) {
    const auto blob = PatchEnvelope(MakeEnvelope(ShaderTarget::DXIL), [](WireMetadataEnvelope& envelope) {
        envelope.Magic = kShaderDiscoveryWireMagic;
    });

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsOtherSchemaVersions) {
    for (const uint16_t schema : {uint16_t{7}, uint16_t{kShaderMetadataSchemaVersion + 1}}) {
        const auto blob = PatchEnvelope(MakeEnvelope(ShaderTarget::DXIL), [schema](WireMetadataEnvelope& envelope) {
            envelope.SchemaVersion = schema;
        });

        EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact)) << schema;
    }
}

TEST(RadRayShaderContract, EnvelopeRejectsOtherTarget) {
    EXPECT_FALSE(ValidateWireMetadataEnvelope(MakeEnvelope(ShaderTarget::DXIL), ShaderTarget::SPIRV, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsUnexpectedGpuArtifact) {
    GpuArtifactHash other = kArtifact;
    other.Bytes[15] ^= 1;

    EXPECT_FALSE(ValidateWireMetadataEnvelope(MakeEnvelope(ShaderTarget::DXIL), ShaderTarget::DXIL, other));
}

TEST(RadRayShaderContract, EnvelopeRejectsTotalSizeDifferentFromBlob) {
    vector<byte> blob = MakeEnvelope(ShaderTarget::DXIL);
    blob.push_back(byte{0});

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsRangeOutsideBlob) {
    const auto blob = PatchEnvelope(MakeEnvelope(ShaderTarget::DXIL), [](WireMetadataEnvelope& envelope) {
        envelope.Bytecode.Offset = envelope.TotalSize - 1;
    });

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsEmptyBytecode) {
    const auto blob = PatchEnvelope(MakeEnvelope(ShaderTarget::DXIL), [](WireMetadataEnvelope& envelope) {
        envelope.Bytecode.Size = 0;
    });

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsRootSignatureOnSpirv) {
    const auto blob = PatchEnvelope(MakeEnvelope(ShaderTarget::SPIRV), [](WireMetadataEnvelope& envelope) {
        envelope.RootSignature = envelope.Bytecode;
    });

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::SPIRV, kArtifact));
}

TEST(RadRayShaderContract, EnvelopeRejectsSamplerRecordsOnDxil) {
    const auto blob = PatchEnvelope(MakeEnvelope(ShaderTarget::DXIL), [](WireMetadataEnvelope& envelope) {
        envelope.SamplerRecords = envelope.Bytecode;
    });

    EXPECT_FALSE(ValidateWireMetadataEnvelope(blob, ShaderTarget::DXIL, kArtifact));
}

}  // namespace
}  // namespace radray::shader
