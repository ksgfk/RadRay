#include <radray/shader_compiler/client.h>

#include <gtest/gtest.h>
#include <cstring>

#if defined(_WIN32)
#include <radray/dynamic_library.h>

#include <windows.h>
#include <objidl.h>
#include <unknwn.h>

#include <dxc/dxcapi.h>
#include <dxc/dxcapi_radrayext.h>
#include <wrl/client.h>
#endif

namespace radray::shader_compiler {
namespace {

TEST(RadRayShaderCompilerClient, LoadsCanonicalLibraryAndRejectsMissingLibrary) {
    Client client;
    ASSERT_TRUE(client.IsAvailable())
        << "RadRay DXC client could not load the canonical dxcompiler library";

    Client missing{"radray_missing_shader_compiler_probe"};
    EXPECT_FALSE(missing.IsAvailable());
    EXPECT_FALSE(missing.GetToolchainIdentity());
}

TEST(RadRayShaderCompilerClient, LoadedPackageExposesMatchingAbi) {
#if defined(_WIN32)
    DynamicLibrary compilerLibrary{"dxcompiler"};
    ASSERT_TRUE(compilerLibrary.IsValid());

    using DxcCreateInstanceFunction = decltype(&DxcCreateInstance);
    const DxcCreateInstanceFunction createInstance =
        compilerLibrary.GetFunction<DxcCreateInstanceFunction>("DxcCreateInstance");
    ASSERT_NE(createInstance, nullptr);

    Microsoft::WRL::ComPtr<shader::IRadRayDxcCompiler> compiler;
    ASSERT_TRUE(SUCCEEDED(createInstance(
        shader::CLSID_RadRayDxcCompiler,
        shader::IID_IRadRayDxcCompiler,
        reinterpret_cast<void**>(compiler.GetAddressOf()))));

    shader::RadRayDxcAbiInfo info{};
    ASSERT_TRUE(SUCCEEDED(compiler->GetAbiInfo(&info)));
    EXPECT_EQ(info.AbiVersion, shader::kShaderCompilerAbiVersion);
    EXPECT_EQ(info.MetadataSchemaVersion, shader::kShaderMetadataSchemaVersion);
    EXPECT_EQ(info.ToolchainMajor, 1);
    EXPECT_EQ(info.ToolchainMinor, 9);
    bool hasToolchainIdentity = false;
    for (const uint8_t value : info.ToolchainIdentity.Bytes)
        hasToolchainIdentity |= value != 0;
    EXPECT_TRUE(hasToolchainIdentity);
    Client client;
    shader::Hash128 expected{};
    std::memcpy(expected.Bytes.data(), &info.ToolchainIdentity, sizeof(expected.Bytes));
    EXPECT_EQ(client.GetToolchainIdentity(), expected);
    EXPECT_TRUE(client.IsAvailable());
    EXPECT_EQ(client.GetToolchainIdentity(), expected);
#else
    GTEST_SKIP() << "RadRay DXC fork ABI is only loadable on Windows";
#endif
}

TEST(RadRayShaderCompilerClient, FailuresReturnContextAndNoPartialValue) {
    Client missing{"radray_missing_shader_compiler_probe"};
    shader::CompileVariantRequest request{
        .SourceName = "fixtures/unavailable.hlsl",
        .RootSource = {byte{1}},
        .Defines = {},
        .Assignments = {},
        .Targets = shader::ShaderTargetMask::All};
    const auto compiled = missing.CompileVariant(request, {});
    EXPECT_FALSE(compiled.Succeeded());
    EXPECT_NE(compiled.Diagnostics.find(request.SourceName), string::npos);
    EXPECT_NE(compiled.Diagnostics.find("unavailable"), string::npos);

    const auto discovered = missing.DiscoverSourceContract(
        request.SourceName, request.RootSource, shader::ShaderTarget::DXIL, {});
    EXPECT_FALSE(discovered.Succeeded());
    EXPECT_NE(discovered.Diagnostics.find(request.SourceName), string::npos);
    EXPECT_NE(discovered.Diagnostics.find("unavailable"), string::npos);

    const auto invalidContract = DecodeWireShaderContract({});
    EXPECT_FALSE(invalidContract.Succeeded());
    EXPECT_NE(invalidContract.Diagnostics.find("header"), string::npos);

    request.SourceName = "../invalid.hlsl";
    const auto invalidRequest = missing.CompileVariant(request, {});
    EXPECT_FALSE(invalidRequest.Succeeded());
    EXPECT_NE(invalidRequest.Diagnostics.find("SourceName"), string::npos);
}

}  // namespace
}  // namespace radray::shader_compiler
