#include <radray/shader/shader_artifact.h>

#include <gtest/gtest.h>

namespace radray::shader {
namespace {

TEST(RadRayShaderDiagnostics, EncodingPreservesValidationReasonWithoutPartialBytes) {
    CompileVariantRequest request{
        .SourceName = "C:/absolute.hlsl",
        .RootSource = {},
        .Defines = {},
        .Assignments = {},
        .Targets = ShaderTargetMask::All};
    const auto canonical = CanonicalizeCompileVariantRequest(request);
    const auto encoded = EncodeCanonicalCompileVariantRequest(request);
    EXPECT_FALSE(canonical.Succeeded());
    EXPECT_FALSE(encoded.Succeeded());
    EXPECT_EQ(encoded.Diagnostics, canonical.Diagnostics);
    EXPECT_NE(encoded.Diagnostics.find("SourceName"), string::npos);

    request.SourceName = "fixtures/valid.hlsl";
    request.Defines = {{"MODE", "0"}, {"MODE", "1"}};
    const auto duplicateDefines = EncodeCanonicalCompileVariantRequest(request);
    EXPECT_FALSE(duplicateDefines.Succeeded());
    EXPECT_NE(duplicateDefines.Diagnostics.find("Defines"), string::npos);

    request.Defines.clear();
    request.Assignments = {{"QUALITY", "low"}, {"QUALITY", "high"}};
    const auto duplicateAssignments = CanonicalizeCompileVariantRequest(request);
    EXPECT_FALSE(duplicateAssignments.Succeeded());
    EXPECT_NE(duplicateAssignments.Diagnostics.find("Assignments"), string::npos);

    SourceContractRequest discovery{
        .SourceName = request.SourceName,
        .RootSource = {},
        .Defines = {},
        .Targets = ShaderTargetMask::DXIL};
    const auto emptySource = EncodeSourceContractRequest(discovery);
    EXPECT_FALSE(emptySource.Succeeded());
    EXPECT_NE(emptySource.Diagnostics.find("RootSource"), string::npos);
    discovery.RootSource = {byte{1}};
    discovery.Targets = ShaderTargetMask::None;
    const auto missingTarget = EncodeSourceContractRequest(discovery);
    EXPECT_FALSE(missingTarget.Succeeded());
    EXPECT_NE(missingTarget.Diagnostics.find("Targets"), string::npos);
}

TEST(RadRayShaderDiagnostics, SuccessfulResultRetainsOrderedDiagnosticText) {
    DiagnosticResult<int> result{42, {}};
    AppendDiagnostic(result.Diagnostics, "pass.hlsl:2: warning: first");
    AppendDiagnostic(result.Diagnostics, "pass.hlsl:3: warning: second\n");
    AppendDiagnostic(result.Diagnostics, "");
    AppendDiagnostic(result.Diagnostics, "pass.hlsl:4: note: context");
    EXPECT_TRUE(result.Succeeded());
    ASSERT_TRUE(result.Value.has_value());
    EXPECT_EQ(*result.Value, 42);
    EXPECT_EQ(result.Diagnostics,
              "pass.hlsl:2: warning: first\n"
              "pass.hlsl:3: warning: second\n"
              "pass.hlsl:4: note: context");
}

TEST(RadRayShaderDiagnostics, TypedDecoderPreservesTruncationDiagnostic) {
    const ShaderArtifactDecodeOptions options{.Target = ShaderTarget::DXIL};
    const auto generic = DecodeShaderArtifact({}, options);
    const auto dxil = DecodeDxilShaderArtifact({}, options);
    EXPECT_FALSE(generic.Succeeded());
    EXPECT_FALSE(dxil.Succeeded());
    EXPECT_EQ(dxil.Diagnostics, generic.Diagnostics);
    EXPECT_NE(dxil.Diagnostics.find("truncated"), string::npos);
    EXPECT_NE(dxil.Diagnostics.find("got 0"), string::npos);
}

}  // namespace
}  // namespace radray::shader
