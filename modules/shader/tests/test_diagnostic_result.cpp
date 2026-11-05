#include <radray/shader/diagnostic_result.h>

#include <gtest/gtest.h>

namespace radray::shader {
namespace {

TEST(RadRayShaderDiagnostics, SucceededMeansValueIsPresent) {
    const DiagnosticResult<int> success{42, "pass.hlsl:3: warning: unused"};
    const DiagnosticResult<int> failure{std::nullopt, "pass.hlsl:3: error: bad"};

    EXPECT_TRUE(success.Succeeded());
    EXPECT_FALSE(failure.Succeeded());
}

TEST(RadRayShaderDiagnostics, AppendSeparatesMessagesWithNewline) {
    string diagnostics;

    AppendDiagnostic(diagnostics, "first");
    AppendDiagnostic(diagnostics, "second");

    EXPECT_EQ(diagnostics, "first\nsecond");
}

TEST(RadRayShaderDiagnostics, AppendReusesTrailingNewline) {
    string diagnostics = "first\n";

    AppendDiagnostic(diagnostics, "second");

    EXPECT_EQ(diagnostics, "first\nsecond");
}

TEST(RadRayShaderDiagnostics, AppendIgnoresEmptyMessage) {
    string diagnostics = "first";

    AppendDiagnostic(diagnostics, "");

    EXPECT_EQ(diagnostics, "first");
}

}  // namespace
}  // namespace radray::shader
