#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/file.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_asset.h>
#include <radray/shader/dxc.h>

namespace radray {
namespace {

class FakeJitCompiler final : public IShaderJitCompiler {
public:
    shader::ShaderHash GetToolchainHash() const noexcept override { return {0x1234, 0x5678}; }

    ShaderJitStageCompileResult CompileStage(
        const ShaderJitStageCompileRequest& request) override {
        if (request.Stage == shader::ShaderStage::Vertex) ++VertexCalls;
        if (request.Stage == shader::ShaderStage::Pixel) ++PixelCalls;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        if (FailPixel && request.Stage == shader::ShaderStage::Pixel) {
            return ShaderJitStageCompileResult{
                .Diagnostics = {shader::ShaderDiagnostic{
                    .Code = shader::ShaderDiagnosticCode::CompilationFailed,
                    .Message = "intentional test failure",
                    .Context = shader::ShaderDiagnosticContext{
                        .Target = request.Target,
                        .PassIndex = request.PassIndex,
                        .VariantDefines = request.FullDefines,
                        .Stage = request.Stage}}}};
        }
        shader::ShaderReflectionDesc reflection;
        shader::ShaderStageInterfaceBuildResult interface;
        if (request.Target == shader::ShaderTarget::DXIL) {
            reflection = shader::HlslShaderDesc{};
            interface = shader::NormalizeHlslInterface(
                std::get<shader::HlslShaderDesc>(reflection),
                request.Stage);
        } else {
            shader::SpirvShaderDesc spirv;
            if (MismatchSpirv && request.Stage == shader::ShaderStage::Pixel) {
                spirv.ResourceBindings.emplace_back(shader::SpirvResourceBinding{
                    .Name = "MismatchedSampler",
                    .Kind = shader::SpirvResourceKind::SeparateSampler,
                    .Set = 7,
                    .Binding = 0,
                    .ArraySize = 1});
            }
            reflection = std::move(spirv);
            interface = shader::NormalizeSpirvInterface(
                std::get<shader::SpirvShaderDesc>(reflection),
                request.Stage);
        }
        if (!interface.Succeeded()) return {.Diagnostics = std::move(interface.Diagnostics)};
        ShaderResolvedStageArtifact artifact{
            .Target = request.Target,
            .PassIndex = request.PassIndex,
            .Stage = request.Stage,
            .Defines = request.ProjectedDefines,
            .EntryPoint = string{*shader::FindShaderEntryPoint(request.Pass, request.Stage)},
            .Bytecode = {
                static_cast<byte>(request.Stage),
                static_cast<byte>(request.ProjectedDefines.size() + 1)},
            .Reflection = reflection,
            .Interface = std::move(*interface.Interface)};
        artifact.BinaryHash = shader::HashShaderBytes(artifact.Bytecode);
        if (CorruptBinaryHash) artifact.BinaryHash.Low ^= 1;
        return {.Artifact = std::move(artifact)};
    }

    std::atomic<uint32_t> VertexCalls{0};
    std::atomic<uint32_t> PixelCalls{0};
    bool FailPixel{false};
    bool MismatchSpirv{false};
    bool CorruptBinaryHash{false};
};

shader::ShaderBinary MakeSparseBinary(std::string_view sourcePath = "main.hlsl") {
    shader::ShaderBinary binary;
    binary.Asset.AssetId = Guid::NewGuid();
    shader::ShaderPassDesc pass;
    pass.Name = "JitTest";
    pass.SourcePath = sourcePath;
    pass.IsOptimize = true;
    pass.EnableUnbounded = false;
    pass.VariantDomain.KeywordGroups = {shader::ShaderKeywordGroupDesc{
        .Alternatives = {"", "FEATURE=1"},
        .Scope = shader::ShaderKeywordScope::Local,
        .Stages = shader::ShaderStage::Pixel}};
    pass.BakeSet.Variants = {
        shader::ShaderVariantKey{},
        shader::ShaderVariantKey{{"FEATURE=1"}}};
    shader::ShaderGraphicsPassDesc graphics;
    graphics.VertexEntry = "VSMain";
    graphics.PixelEntry = "PSMain";
    pass.Program = std::move(graphics);
    binary.Asset.Passes.emplace_back(std::move(pass));
    return binary;
}

shader::ShaderBinary MakeMultiPassBinary() {
    shader::ShaderBinary binary = MakeSparseBinary();
    shader::ShaderPassDesc second = binary.Asset.Passes.front();
    second.Name = "JitTestSecond";
    second.SourcePath = "second.hlsl";
    binary.Asset.Passes.emplace_back(std::move(second));
    return binary;
}

void AddBakedDefaultProgram(shader::ShaderBinary& binary) {
    shader::HlslShaderDesc reflection;
    const auto payload = shader::SerializeHlslShaderDesc(reflection);
    ASSERT_TRUE(payload.has_value());
    binary.Reflections.emplace_back(shader::ShaderReflectionRecord{
        .Target = shader::ShaderTarget::DXIL,
        .Reflection = reflection,
        .Hash = shader::HashShaderBytes(
            std::as_bytes(std::span{payload->data(), payload->size()}))});

    auto vertex = shader::NormalizeHlslInterface(reflection, shader::ShaderStage::Vertex);
    auto pixel = shader::NormalizeHlslInterface(reflection, shader::ShaderStage::Pixel);
    ASSERT_TRUE(vertex.Succeeded());
    ASSERT_TRUE(pixel.Succeeded());
    binary.StageInterfaces = {*vertex.Interface, *pixel.Interface};
    auto program = shader::MergeGraphicsStageInterfaces(
        binary.StageInterfaces[0],
        binary.StageInterfaces[1]);
    ASSERT_TRUE(program.Succeeded());
    binary.ProgramInterfaces = {*program.Interface};

    for (uint32_t index = 0; index < 2; ++index) {
        const shader::ShaderStage stage = index == 0
                                              ? shader::ShaderStage::Vertex
                                              : shader::ShaderStage::Pixel;
        shader::ShaderStageArtifact artifact{
            .Target = shader::ShaderTarget::DXIL,
            .Category = shader::ShaderBlobCategory::DXIL,
            .PassIndex = 0,
            .Stage = stage,
            .EntryPoint = string{*shader::FindShaderEntryPoint(binary.Asset.Passes[0], stage)},
            .Bytecode = {static_cast<byte>(index + 1)},
            .ReflectionIndex = 0,
            .InterfaceIndex = index};
        artifact.BinaryHash = shader::HashShaderBytes(artifact.Bytecode);
        binary.StageArtifacts.emplace_back(std::move(artifact));
    }
    binary.ProgramVariants.emplace_back(shader::ShaderProgramVariantArtifact{
        .Target = shader::ShaderTarget::DXIL,
        .PassIndex = 0,
        .StageArtifactIndices = {0, 1},
        .InterfaceIndex = 0});
}

std::filesystem::path MakeSourceTree() {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / fmt::format("radray_jit_test_{}", Guid::NewGuid());
    EXPECT_TRUE(WriteTextFile(root / "common.hlsl", "float4 SharedColor() { return 1.0; }\n"));
    EXPECT_TRUE(WriteTextFile(
        root / "main.hlsl",
        "#include \"common.hlsl\"\n"
        "float4 VSMain(float4 p : POSITION) : SV_Position { return p; }\n"
        "float4 PSMain() : SV_Target0 { return SharedColor(); }\n"));
    EXPECT_TRUE(WriteTextFile(
        root / "second.hlsl",
        "#include \"common.hlsl\"\n"
        "float4 VSMain(float4 p : POSITION) : SV_Position { return p * 2.0; }\n"
        "float4 PSMain() : SV_Target0 { return SharedColor() * 0.5; }\n"));
    return root;
}

ShaderArtifactResolver::Config MakeConfig(
    const std::filesystem::path& root,
    bool disk = false) {
    return ShaderArtifactResolver::Config{
        .ShaderRoot = root,
        .DiskCacheDirectory = root / "cache",
        .EnableDiskCache = disk};
}

}  // namespace

TEST(ShaderArtifactResolverTest, CoalescesProgramsAndStageProjectedVariants) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary binary = MakeSparseBinary();
    ASSERT_TRUE(binary.IsValid());
    ShaderAsset asset{std::move(binary)};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {});
    auto duplicate = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {});
    ASSERT_TRUE(first.get().Succeeded());
    ASSERT_TRUE(duplicate.get().Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);
    EXPECT_EQ(resolver.GetProgramCacheSize(), 1u);
    EXPECT_EQ(resolver.GetStageCacheSize(), 2u);

    auto keyword = resolver.ResolveAsync(
                               asset, shader::ShaderTarget::DXIL, 0, {"FEATURE=1"})
                       .get();
    ASSERT_TRUE(keyword.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 2u);
    EXPECT_EQ(resolver.GetStageCacheSize(), 3u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, ResolvedProgramInitializesAJitOnlyMaterialWithoutMutatingShaderAsset) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary binary = MakeSparseBinary();
    ASSERT_TRUE(binary.IsValid());
    AssetManager assets;
    StreamingAssetRef<ShaderAsset> shaderRef =
        assets.AddReady(Guid::NewGuid(), make_unique<ShaderAsset>(std::move(binary)));
    MaterialAsset material{shaderRef};
    EXPECT_FALSE(material.IsReady());

    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};
    auto resolved = resolver.ResolveAsync(
                                *shaderRef.Get(),
                                shader::ShaderTarget::DXIL,
                                0,
                                {})
                        .get();
    ASSERT_TRUE(resolved.Succeeded());
    ASSERT_TRUE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{resolved.Program.get(), 1}));
    EXPECT_TRUE(material.IsReady());
    EXPECT_TRUE(material.GetParameters().GetLayout().Empty());
    EXPECT_TRUE(material.HasCompleteParametersFor(resolved.Program->Interface));
    EXPECT_TRUE(shaderRef->GetBinary().ProgramVariants.empty());

    ShaderResolvedProgram differentSource = *resolved.Program;
    differentSource.Defines = {"FEATURE=1"};
    differentSource.SourceIdentity.Low ^= 1;
    EXPECT_FALSE(material.ApplyResolvedPrograms(
        std::span<const ShaderResolvedProgram>{&differentSource, 1}));
    EXPECT_TRUE(material.IsReady());

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, CachesFailuresWithoutRetryingEveryRequest) {
    const std::filesystem::path root = MakeSourceTree();
    ShaderAsset asset{MakeSparseBinary()};
    auto compiler = make_shared<FakeJitCompiler>();
    compiler->FailPixel = true;
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    auto second = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_FALSE(first.Succeeded());
    ASSERT_FALSE(second.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, RejectsMalformedSuccessfulCompilerArtifacts) {
    const std::filesystem::path root = MakeSourceTree();
    ShaderAsset asset{MakeSparseBinary()};
    auto compiler = make_shared<FakeJitCompiler>();
    compiler->CorruptBinaryHash = true;
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    auto cached = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_FALSE(first.Succeeded());
    ASSERT_FALSE(cached.Succeeded());
    ASSERT_FALSE(first.Diagnostics.empty());
    EXPECT_EQ(
        first.Diagnostics.front().Code,
        shader::ShaderDiagnosticCode::CompilationFailed);
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, CapturesSourceIdentityPerAssetPass) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary binary = MakeMultiPassBinary();
    ASSERT_TRUE(binary.IsValid());
    ShaderAsset asset{std::move(binary)};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    auto second = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 1, {}).get();
    ASSERT_TRUE(first.Succeeded());
    ASSERT_TRUE(second.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 2u);
    EXPECT_EQ(compiler->PixelCalls.load(), 2u);
    EXPECT_EQ(resolver.GetProgramCacheSize(), 2u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, SourceChangesRequireANewImmutableAsset) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary manifest = MakeSparseBinary();
    ShaderAsset oldAsset{manifest};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};
    ASSERT_TRUE(resolver.ResolveAsync(oldAsset, shader::ShaderTarget::DXIL, 0, {}).get().Succeeded());

    ASSERT_TRUE(WriteTextFile(root / "common.hlsl", "float4 SharedColor() { return 0.5; }\n"));
    ASSERT_TRUE(resolver.ResolveAsync(oldAsset, shader::ShaderTarget::DXIL, 0, {}).get().Succeeded());
    auto oldMiss = resolver.ResolveAsync(
                               oldAsset, shader::ShaderTarget::DXIL, 0, {"FEATURE=1"})
                       .get();
    ASSERT_FALSE(oldMiss.Succeeded());
    ASSERT_FALSE(oldMiss.Diagnostics.empty());
    EXPECT_EQ(oldMiss.Diagnostics.front().Code, shader::ShaderDiagnosticCode::SourceUnavailable);

    ShaderAsset newAsset{std::move(manifest)};
    auto recompiled = resolver.ResolveAsync(
                                  newAsset, shader::ShaderTarget::DXIL, 0, {"FEATURE=1"})
                          .get();
    ASSERT_TRUE(recompiled.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 2u);
    EXPECT_EQ(compiler->PixelCalls.load(), 2u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, CookedSourceIdentityPreventsMixingBakedAndChangedJitCode) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary binary = MakeSparseBinary();
    auto identity = shader::ComputeShaderSourceIdentity(
        root,
        binary.Asset.Passes.front());
    ASSERT_TRUE(identity.Succeeded());
    binary.Asset.Passes.front().SourceIdentity = identity.Identity->Hash;
    AddBakedDefaultProgram(binary);
    ASSERT_TRUE(binary.IsValid());

    ShaderAsset oldAsset{std::move(binary)};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};
    ASSERT_TRUE(WriteTextFile(
        root / "common.hlsl",
        "float4 SharedColor() { return 0.125; }\n"));

    auto staleMiss = resolver.ResolveAsync(
                                 oldAsset,
                                 shader::ShaderTarget::DXIL,
                                 0,
                                 {"FEATURE=1"})
                         .get();
    ASSERT_FALSE(staleMiss.Succeeded());
    ASSERT_FALSE(staleMiss.Diagnostics.empty());
    EXPECT_EQ(
        staleMiss.Diagnostics.front().Code,
        shader::ShaderDiagnosticCode::SourceUnavailable);
    EXPECT_EQ(compiler->VertexCalls.load(), 0u);
    EXPECT_EQ(compiler->PixelCalls.load(), 0u);

    shader::ShaderBinary currentManifest = MakeSparseBinary();
    auto currentIdentity = shader::ComputeShaderSourceIdentity(
        root,
        currentManifest.Asset.Passes.front());
    ASSERT_TRUE(currentIdentity.Succeeded());
    currentManifest.Asset.Passes.front().SourceIdentity = currentIdentity.Identity->Hash;
    ShaderAsset currentAsset{std::move(currentManifest)};
    auto current = resolver.ResolveAsync(
                               currentAsset,
                               shader::ShaderTarget::DXIL,
                               0,
                               {"FEATURE=1"})
                       .get();
    ASSERT_TRUE(current.Succeeded());
    EXPECT_EQ(current.Program->SourceIdentity, currentIdentity.Identity->Hash);
    EXPECT_NE(current.Program->ProgramIdentity, shader::ShaderHash{});

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, PersistsSuccessfulProgramsToDisk) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary manifest = MakeSparseBinary();
    {
        ShaderAsset asset{manifest};
        auto compiler = make_shared<FakeJitCompiler>();
        ShaderArtifactResolver resolver{MakeConfig(root, true), compiler};
        auto result = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
        ASSERT_TRUE(result.Succeeded());
        EXPECT_EQ(result.Source, ShaderArtifactSource::Jit);
    }
    {
        ShaderAsset asset{std::move(manifest)};
        auto compiler = make_shared<FakeJitCompiler>();
        ShaderArtifactResolver resolver{MakeConfig(root, true), compiler};
        auto result = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
        ASSERT_TRUE(result.Succeeded());
        EXPECT_EQ(result.Source, ShaderArtifactSource::DiskCache);
        EXPECT_EQ(compiler->VertexCalls.load(), 0u);
        EXPECT_EQ(compiler->PixelCalls.load(), 0u);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, RejectsDiskCacheWithWrongEmbeddedProgramKey) {
    const std::filesystem::path root = MakeSourceTree();
    shader::ShaderBinary manifest = MakeSparseBinary();
    {
        ShaderAsset asset{manifest};
        auto compiler = make_shared<FakeJitCompiler>();
        ShaderArtifactResolver resolver{MakeConfig(root, true), compiler};
        ASSERT_TRUE(resolver.ResolveAsync(
                                asset,
                                shader::ShaderTarget::DXIL,
                                0,
                                {})
                        .get()
                        .Succeeded());
    }

    vector<std::filesystem::path> cacheFiles;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root / "cache")) {
        if (entry.path().extension() == ".rrjit") cacheFiles.emplace_back(entry.path());
    }
    ASSERT_EQ(cacheFiles.size(), 1u);
    auto cacheBinary = shader::ReadShaderBinary(cacheFiles.front());
    ASSERT_TRUE(cacheBinary.has_value());
    cacheBinary->Asset.AssetId = Guid::NewGuid();
    ASSERT_TRUE(shader::WriteShaderBinary(cacheFiles.front(), *cacheBinary));

    ShaderAsset asset{std::move(manifest)};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root, true), compiler};
    auto rebuilt = resolver.ResolveAsync(
                               asset,
                               shader::ShaderTarget::DXIL,
                               0,
                               {})
                       .get();
    ASSERT_TRUE(rebuilt.Succeeded());
    EXPECT_EQ(rebuilt.Source, ShaderArtifactSource::Jit);
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, UsesCapturedDiskCacheBeforeRequiringLiveSource) {
    const std::filesystem::path root = MakeSourceTree();
    ShaderAsset asset{MakeSparseBinary()};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{MakeConfig(root, true), compiler};
    ASSERT_TRUE(resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get().Succeeded());
    ASSERT_EQ(compiler->VertexCalls.load(), 1u);
    ASSERT_EQ(compiler->PixelCalls.load(), 1u);

    resolver.ClearMemoryCache();
    std::error_code ignored;
    ASSERT_TRUE(std::filesystem::remove(root / "main.hlsl", ignored));
    ASSERT_FALSE(ignored);
    ASSERT_TRUE(std::filesystem::remove(root / "common.hlsl", ignored));
    ASSERT_FALSE(ignored);
    auto cached = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_TRUE(cached.Succeeded());
    EXPECT_EQ(cached.Source, ShaderArtifactSource::DiskCache);
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);

    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, SourceIdentityTracksTransitiveIncludes) {
    const std::filesystem::path root = MakeSourceTree();
    const shader::ShaderPassDesc pass = MakeSparseBinary().Asset.Passes.front();
    auto first = ComputeShaderSourceIdentity(root, pass);
    ASSERT_TRUE(first.Succeeded());
    EXPECT_EQ(first.Identity->Dependencies.size(), 2u);
    ASSERT_TRUE(WriteTextFile(root / "common.hlsl", "float4 SharedColor() { return 0.25; }\n"));
    auto second = ComputeShaderSourceIdentity(root, pass);
    ASSERT_TRUE(second.Succeeded());
    EXPECT_NE(first.Identity->Hash, second.Identity->Hash);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST(ShaderArtifactResolverTest, RejectsCanonicalMismatchBetweenJitTargets) {
    const std::filesystem::path root = MakeSourceTree();
    ShaderAsset asset{MakeSparseBinary()};
    auto compiler = make_shared<FakeJitCompiler>();
    compiler->MismatchSpirv = true;
    ShaderArtifactResolver resolver{MakeConfig(root), compiler};

    auto dxil = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_TRUE(dxil.Succeeded());
    auto spirv = resolver.ResolveAsync(asset, shader::ShaderTarget::SPIRV, 0, {}).get();
    ASSERT_FALSE(spirv.Succeeded());
    ASSERT_EQ(spirv.Diagnostics.size(), 1u);
    EXPECT_EQ(spirv.Diagnostics.front().Code, shader::ShaderDiagnosticCode::InterfaceMismatch);
    EXPECT_EQ(spirv.Diagnostics.front().Context.Target, shader::ShaderTarget::SPIRV);
    EXPECT_EQ(spirv.Diagnostics.front().Context.PassIndex, 0u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

#if defined(RADRAY_ENABLE_SHADER_JIT) && defined(RADRAY_ENABLE_SPIRV_CROSS)
TEST(ShaderArtifactResolverTest, DxcJitProducesEquivalentDxilAndSpirvInterfaces) {
    const std::filesystem::path root = MakeSourceTree();
    ShaderAsset asset{MakeSparseBinary()};
    auto dxc = shader::CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    ShaderArtifactResolver resolver{
        MakeConfig(root),
        CreateDxcShaderJitCompiler(dxc.Release())};
    auto dxil = resolver.ResolveAsync(asset, shader::ShaderTarget::DXIL, 0, {}).get();
    auto spirv = resolver.ResolveAsync(asset, shader::ShaderTarget::SPIRV, 0, {}).get();
    ASSERT_TRUE(dxil.Succeeded()) << (dxil.Diagnostics.empty() ? "" : dxil.Diagnostics.front().Message);
    ASSERT_TRUE(spirv.Succeeded()) << (spirv.Diagnostics.empty() ? "" : spirv.Diagnostics.front().Message);
    EXPECT_EQ(dxil.Program->Interface, spirv.Program->Interface);
    EXPECT_EQ(asset.GetBinary().ProgramVariants.size(), 0u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

}  // namespace radray
