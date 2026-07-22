#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <radray/file.h>
#include <radray/runtime/asset_manager.h>
#include <radray/runtime/material_asset.h>
#include <radray/runtime/shader_artifact_resolver.h>
#include <radray/runtime/shader_asset.h>
#include <radray/render/dxc.h>

namespace radray {
namespace {

class FakeJitCompiler final : public IShaderJitCompiler {
public:
    render::ShaderHash GetToolchainHash() const noexcept override { return {0x1234, 0x5678}; }

    ShaderJitStageCompileResult CompileStage(
        const ShaderJitStageCompileRequest& request) override {
        if (request.Stage == render::ShaderStage::Vertex) ++VertexCalls;
        if (request.Stage == render::ShaderStage::Pixel) ++PixelCalls;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
        if (FailPixel && request.Stage == render::ShaderStage::Pixel) {
            return ShaderJitStageCompileResult{
                .Artifact = {},
                .Diagnostics = {render::ShaderDiagnostic{
                    .Code = render::ShaderDiagnosticCode::CompilationFailed,
                    .Message = "intentional test failure",
                    .Context = render::ShaderDiagnosticContext{
                        .Target = request.Target,
                        .PassIndex = request.PassIndex,
                        .VariantDefines = request.FullDefines,
                        .Stage = request.Stage}}}};
        }
        render::ShaderReflectionDesc reflection;
        render::ShaderStageInterfaceBuildResult interface;
        if (request.Target == render::ShaderTarget::DXIL) {
            reflection = render::HlslShaderDesc{};
            interface = render::NormalizeHlslInterface(
                std::get<render::HlslShaderDesc>(reflection),
                request.Stage);
        } else {
            render::SpirvShaderDesc spirv;
            if (MismatchSpirv && request.Stage == render::ShaderStage::Pixel) {
                spirv.ResourceBindings.emplace_back(render::SpirvResourceBinding{
                    .Name = "MismatchedSampler",
                    .Kind = render::SpirvResourceKind::SeparateSampler,
                    .Set = 7,
                    .Binding = 0,
                    .ArraySize = 1,
                    .ImageInfo = {},
                    .HlslType = {}});
            }
            reflection = std::move(spirv);
            interface = render::NormalizeSpirvInterface(
                std::get<render::SpirvShaderDesc>(reflection),
                request.Stage);
        }
        if (!interface.Succeeded()) {
            return {
                .Artifact = {},
                .Diagnostics = std::move(interface.Diagnostics)};
        }
        ShaderResolvedStageArtifact artifact{
            .Target = request.Target,
            .PassIndex = request.PassIndex,
            .Stage = request.Stage,
            .Defines = request.ProjectedDefines,
            .EntryPoint = string{*render::FindShaderEntryPoint(request.Pass, request.Stage)},
            .Bytecode = {
                static_cast<byte>(request.Stage),
                static_cast<byte>(request.ProjectedDefines.size() + 1)},
            .Reflection = reflection,
            .Interface = std::move(*interface.Interface)};
        artifact.BinaryHash = render::HashShaderBytes(artifact.Bytecode);
        if (CorruptBinaryHash) artifact.BinaryHash.Low ^= 1;
        return {
            .Artifact = std::move(artifact),
            .Diagnostics = {}};
    }

    std::atomic<uint32_t> VertexCalls{0};
    std::atomic<uint32_t> PixelCalls{0};
    bool FailPixel{false};
    bool MismatchSpirv{false};
    bool CorruptBinaryHash{false};
};

render::ShaderBinary MakeSparseBinary(std::string_view sourcePath = "main.hlsl") {
    render::ShaderBinary binary;
    binary.Asset.AssetId = Guid::NewGuid();
    render::ShaderPassDesc pass;
    pass.Name = "JitTest";
    pass.SourcePath = sourcePath;
    pass.IsOptimize = true;
    pass.EnableUnbounded = false;
    pass.VariantDomain.KeywordGroups = {render::ShaderKeywordGroupDesc{
        .Alternatives = {"", "FEATURE=1"},
        .Scope = render::ShaderKeywordScope::Local,
        .Stages = render::ShaderStage::Pixel}};
    pass.BakeSet.Variants = {
        render::ShaderVariantKey{},
        render::ShaderVariantKey{{"FEATURE=1"}}};
    render::ShaderGraphicsPassDesc graphics;
    graphics.VertexEntry = "VSMain";
    graphics.PixelEntry = "PSMain";
    pass.Program = std::move(graphics);
    binary.Asset.Passes.emplace_back(std::move(pass));
    return binary;
}

render::ShaderBinary MakeMultiPassBinary() {
    render::ShaderBinary binary = MakeSparseBinary();
    render::ShaderPassDesc second = binary.Asset.Passes.front();
    second.Name = "JitTestSecond";
    second.SourcePath = "second.hlsl";
    binary.Asset.Passes.emplace_back(std::move(second));
    return binary;
}

void AddBakedDefaultProgram(render::ShaderBinary& binary) {
    render::HlslShaderDesc reflection;
    const auto payload = render::SerializeHlslShaderDesc(reflection);
    ASSERT_TRUE(payload.has_value());
    binary.Reflections.emplace_back(render::ShaderReflectionRecord{
        .Target = render::ShaderTarget::DXIL,
        .Reflection = reflection,
        .Hash = render::HashShaderBytes(
            std::as_bytes(std::span{payload->data(), payload->size()}))});

    auto vertex = render::NormalizeHlslInterface(reflection, render::ShaderStage::Vertex);
    auto pixel = render::NormalizeHlslInterface(reflection, render::ShaderStage::Pixel);
    ASSERT_TRUE(vertex.Succeeded());
    ASSERT_TRUE(pixel.Succeeded());
    binary.StageInterfaces = {*vertex.Interface, *pixel.Interface};
    auto program = render::MergeGraphicsStageInterfaces(
        binary.StageInterfaces[0],
        binary.StageInterfaces[1]);
    ASSERT_TRUE(program.Succeeded());
    binary.ProgramInterfaces = {*program.Interface};

    for (uint32_t index = 0; index < 2; ++index) {
        const render::ShaderStage stage = index == 0
                                              ? render::ShaderStage::Vertex
                                              : render::ShaderStage::Pixel;
        render::ShaderStageArtifact artifact{
            .Target = render::ShaderTarget::DXIL,
            .Category = render::ShaderBlobCategory::DXIL,
            .PassIndex = 0,
            .Stage = stage,
            .Defines = {},
            .EntryPoint = string{*render::FindShaderEntryPoint(binary.Asset.Passes[0], stage)},
            .Bytecode = {static_cast<byte>(index + 1)},
            .ReflectionIndex = 0,
            .InterfaceIndex = index};
        artifact.BinaryHash = render::HashShaderBytes(artifact.Bytecode);
        binary.StageArtifacts.emplace_back(std::move(artifact));
    }
    binary.ProgramVariants.emplace_back(render::ShaderProgramVariantArtifact{
        .Target = render::ShaderTarget::DXIL,
        .PassIndex = 0,
        .Defines = {},
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

class ShaderArtifactResolverTest : public testing::Test {
protected:
    StreamingAssetRef<ShaderAsset> AddShaderAsset(
        render::ShaderBinary binary,
        AssetId id = Guid::NewGuid()) {
        return _assets.AddReady(
            id,
            make_unique<ShaderAsset>(std::move(binary)));
    }

    AssetManager _assets;
};

}  // namespace

TEST_F(ShaderArtifactResolverTest, CoalescesProgramsAndStageProjectedVariants) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary binary = MakeSparseBinary();
    ASSERT_TRUE(binary.IsValid());
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(std::move(binary));
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {});
    auto duplicate = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {});
    ASSERT_TRUE(first.get().Succeeded());
    ASSERT_TRUE(duplicate.get().Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);
    EXPECT_EQ(resolver.GetProgramCacheSize(), 1u);
    EXPECT_EQ(resolver.GetStageCacheSize(), 2u);

    auto keyword = resolver.ResolveAsync(
                               *asset, render::ShaderTarget::DXIL, 0, {"FEATURE=1"})
                       .get();
    ASSERT_TRUE(keyword.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 2u);
    EXPECT_EQ(resolver.GetStageCacheSize(), 3u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, RejectsUnmanagedJitOnlyAsset) {
    const std::filesystem::path root = MakeSourceTree();
    ShaderAsset asset{MakeSparseBinary()};
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};

    ShaderArtifactResolutionResult result =
        resolver.ResolveAsync(asset, render::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_FALSE(result.Succeeded());
    ASSERT_EQ(result.Diagnostics.size(), 1u);
    EXPECT_EQ(result.Diagnostics.front().Code, render::ShaderDiagnosticCode::SourceUnavailable);
    EXPECT_EQ(compiler->VertexCalls.load(), 0u);
    EXPECT_EQ(compiler->PixelCalls.load(), 0u);
    EXPECT_EQ(resolver.GetCapturedSourceIdentityCount(), 0u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, ResolvedProgramInitializesAJitOnlyMaterialWithoutMutatingShaderAsset) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary binary = MakeSparseBinary();
    ASSERT_TRUE(binary.IsValid());
    StreamingAssetRef<ShaderAsset> shaderRef = AddShaderAsset(std::move(binary));
    MaterialAsset material{shaderRef};
    EXPECT_FALSE(material.IsReady());

    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};
    auto resolved = resolver.ResolveAsync(
                                *shaderRef.Get(),
                                render::ShaderTarget::DXIL,
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

TEST_F(ShaderArtifactResolverTest, CachesFailuresWithoutRetryingEveryRequest) {
    const std::filesystem::path root = MakeSourceTree();
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(MakeSparseBinary());
    auto compiler = make_shared<FakeJitCompiler>();
    compiler->FailPixel = true;
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    auto second = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_FALSE(first.Succeeded());
    ASSERT_FALSE(second.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, RejectsMalformedSuccessfulCompilerArtifacts) {
    const std::filesystem::path root = MakeSourceTree();
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(MakeSparseBinary());
    auto compiler = make_shared<FakeJitCompiler>();
    compiler->CorruptBinaryHash = true;
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    auto cached = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_FALSE(first.Succeeded());
    ASSERT_FALSE(cached.Succeeded());
    ASSERT_FALSE(first.Diagnostics.empty());
    EXPECT_EQ(
        first.Diagnostics.front().Code,
        render::ShaderDiagnosticCode::CompilationFailed);
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, CapturesSourceIdentityPerAssetPass) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary binary = MakeMultiPassBinary();
    ASSERT_TRUE(binary.IsValid());
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(std::move(binary));
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};

    auto first = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    auto second = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 1, {}).get();
    ASSERT_TRUE(first.Succeeded());
    ASSERT_TRUE(second.Succeeded());
    EXPECT_EQ(compiler->VertexCalls.load(), 2u);
    EXPECT_EQ(compiler->PixelCalls.load(), 2u);
    EXPECT_EQ(resolver.GetProgramCacheSize(), 2u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, SourceChangesRequireANewImmutableAsset) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary manifest = MakeSparseBinary();
    const AssetId assetId = Guid::NewGuid();
    StreamingAssetRef<ShaderAsset> oldAsset = AddShaderAsset(manifest, assetId);
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};
    ASSERT_TRUE(resolver.ResolveAsync(*oldAsset, render::ShaderTarget::DXIL, 0, {}).get().Succeeded());
    ASSERT_EQ(resolver.GetCapturedSourceIdentityCount(), 1u);

    ASSERT_TRUE(WriteTextFile(root / "common.hlsl", "float4 SharedColor() { return 0.5; }\n"));
    ASSERT_TRUE(resolver.ResolveAsync(*oldAsset, render::ShaderTarget::DXIL, 0, {}).get().Succeeded());
    auto oldMiss = resolver.ResolveAsync(
                               *oldAsset, render::ShaderTarget::DXIL, 0, {"FEATURE=1"})
                       .get();
    ASSERT_FALSE(oldMiss.Succeeded());
    ASSERT_FALSE(oldMiss.Diagnostics.empty());
    EXPECT_EQ(oldMiss.Diagnostics.front().Code, render::ShaderDiagnosticCode::SourceUnavailable);

    const AssetHandle oldHandle = oldAsset->GetAssetHandle();
    oldAsset.Reset();
    _assets.Unload(assetId);
    ASSERT_FALSE(_assets.IsHandleValid(oldHandle));
    StreamingAssetRef<ShaderAsset> newAsset = AddShaderAsset(std::move(manifest), assetId);
    ASSERT_NE(newAsset->GetAssetHandle(), oldHandle);
    auto recompiled = resolver.ResolveAsync(
                                  *newAsset, render::ShaderTarget::DXIL, 0, {"FEATURE=1"})
                          .get();
    ASSERT_TRUE(recompiled.Succeeded());
    EXPECT_EQ(resolver.GetCapturedSourceIdentityCount(), 1u);
    EXPECT_EQ(compiler->VertexCalls.load(), 2u);
    EXPECT_EQ(compiler->PixelCalls.load(), 2u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, CookedSourceIdentityPreventsMixingBakedAndChangedJitCode) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary binary = MakeSparseBinary();
    auto identity = render::ComputeShaderSourceIdentity(
        root,
        binary.Asset.Passes.front());
    ASSERT_TRUE(identity.Succeeded());
    binary.Asset.Passes.front().SourceIdentity = identity.Identity->Hash;
    AddBakedDefaultProgram(binary);
    ASSERT_TRUE(binary.IsValid());

    StreamingAssetRef<ShaderAsset> oldAsset = AddShaderAsset(std::move(binary));
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};
    ASSERT_TRUE(WriteTextFile(
        root / "common.hlsl",
        "float4 SharedColor() { return 0.125; }\n"));

    auto staleMiss = resolver.ResolveAsync(
                                 *oldAsset,
                                 render::ShaderTarget::DXIL,
                                 0,
                                 {"FEATURE=1"})
                         .get();
    ASSERT_FALSE(staleMiss.Succeeded());
    ASSERT_FALSE(staleMiss.Diagnostics.empty());
    EXPECT_EQ(
        staleMiss.Diagnostics.front().Code,
        render::ShaderDiagnosticCode::SourceUnavailable);
    EXPECT_EQ(compiler->VertexCalls.load(), 0u);
    EXPECT_EQ(compiler->PixelCalls.load(), 0u);

    render::ShaderBinary currentManifest = MakeSparseBinary();
    auto currentIdentity = render::ComputeShaderSourceIdentity(
        root,
        currentManifest.Asset.Passes.front());
    ASSERT_TRUE(currentIdentity.Succeeded());
    currentManifest.Asset.Passes.front().SourceIdentity = currentIdentity.Identity->Hash;
    StreamingAssetRef<ShaderAsset> currentAsset = AddShaderAsset(std::move(currentManifest));
    auto current = resolver.ResolveAsync(
                               *currentAsset,
                               render::ShaderTarget::DXIL,
                               0,
                               {"FEATURE=1"})
                       .get();
    ASSERT_TRUE(current.Succeeded());
    EXPECT_EQ(current.Program->SourceIdentity, currentIdentity.Identity->Hash);
    EXPECT_NE(current.Program->ProgramIdentity, render::ShaderHash{});

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, PersistsSuccessfulProgramsToDisk) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary manifest = MakeSparseBinary();
    {
        StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(manifest);
        auto compiler = make_shared<FakeJitCompiler>();
        ShaderArtifactResolver resolver{_assets, MakeConfig(root, true), compiler};
        auto result = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
        ASSERT_TRUE(result.Succeeded());
        EXPECT_EQ(result.Source, ShaderArtifactSource::Jit);
    }
    {
        StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(std::move(manifest));
        auto compiler = make_shared<FakeJitCompiler>();
        ShaderArtifactResolver resolver{_assets, MakeConfig(root, true), compiler};
        auto result = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
        ASSERT_TRUE(result.Succeeded());
        EXPECT_EQ(result.Source, ShaderArtifactSource::DiskCache);
        EXPECT_EQ(compiler->VertexCalls.load(), 0u);
        EXPECT_EQ(compiler->PixelCalls.load(), 0u);
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, RejectsDiskCacheWithWrongEmbeddedProgramKey) {
    const std::filesystem::path root = MakeSourceTree();
    render::ShaderBinary manifest = MakeSparseBinary();
    {
        StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(manifest);
        auto compiler = make_shared<FakeJitCompiler>();
        ShaderArtifactResolver resolver{_assets, MakeConfig(root, true), compiler};
        ASSERT_TRUE(resolver.ResolveAsync(
                                *asset,
                                render::ShaderTarget::DXIL,
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
    auto cacheBinary = render::ReadShaderBinary(cacheFiles.front());
    ASSERT_TRUE(cacheBinary.has_value());
    cacheBinary->Asset.AssetId = Guid::NewGuid();
    ASSERT_TRUE(render::WriteShaderBinary(cacheFiles.front(), *cacheBinary));

    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(std::move(manifest));
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root, true), compiler};
    auto rebuilt = resolver.ResolveAsync(
                               *asset,
                               render::ShaderTarget::DXIL,
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

TEST_F(ShaderArtifactResolverTest, UsesCapturedDiskCacheBeforeRequiringLiveSource) {
    const std::filesystem::path root = MakeSourceTree();
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(MakeSparseBinary());
    auto compiler = make_shared<FakeJitCompiler>();
    ShaderArtifactResolver resolver{_assets, MakeConfig(root, true), compiler};
    ASSERT_TRUE(resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get().Succeeded());
    ASSERT_EQ(compiler->VertexCalls.load(), 1u);
    ASSERT_EQ(compiler->PixelCalls.load(), 1u);

    resolver.ClearMemoryCache();
    std::error_code ignored;
    ASSERT_TRUE(std::filesystem::remove(root / "main.hlsl", ignored));
    ASSERT_FALSE(ignored);
    ASSERT_TRUE(std::filesystem::remove(root / "common.hlsl", ignored));
    ASSERT_FALSE(ignored);
    auto cached = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_TRUE(cached.Succeeded());
    EXPECT_EQ(cached.Source, ShaderArtifactSource::DiskCache);
    EXPECT_EQ(compiler->VertexCalls.load(), 1u);
    EXPECT_EQ(compiler->PixelCalls.load(), 1u);

    std::filesystem::remove_all(root, ignored);
}

TEST_F(ShaderArtifactResolverTest, SourceIdentityTracksTransitiveIncludes) {
    const std::filesystem::path root = MakeSourceTree();
    const render::ShaderPassDesc pass = MakeSparseBinary().Asset.Passes.front();
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

TEST_F(ShaderArtifactResolverTest, RejectsCanonicalMismatchBetweenJitTargets) {
    const std::filesystem::path root = MakeSourceTree();
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(MakeSparseBinary());
    auto compiler = make_shared<FakeJitCompiler>();
    compiler->MismatchSpirv = true;
    ShaderArtifactResolver resolver{_assets, MakeConfig(root), compiler};

    auto dxil = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    ASSERT_TRUE(dxil.Succeeded());
    auto spirv = resolver.ResolveAsync(*asset, render::ShaderTarget::SPIRV, 0, {}).get();
    ASSERT_FALSE(spirv.Succeeded());
    ASSERT_EQ(spirv.Diagnostics.size(), 1u);
    EXPECT_EQ(spirv.Diagnostics.front().Code, render::ShaderDiagnosticCode::InterfaceMismatch);
    EXPECT_EQ(spirv.Diagnostics.front().Context.Target, render::ShaderTarget::SPIRV);
    EXPECT_EQ(spirv.Diagnostics.front().Context.PassIndex, 0u);

    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

#if defined(RADRAY_ENABLE_SHADER_JIT) && defined(RADRAY_ENABLE_SPIRV_CROSS)
TEST_F(ShaderArtifactResolverTest, DxcJitProducesEquivalentDxilAndSpirvInterfaces) {
    const std::filesystem::path root = MakeSourceTree();
    StreamingAssetRef<ShaderAsset> asset = AddShaderAsset(MakeSparseBinary());
    auto dxc = render::CreateDxc();
    ASSERT_TRUE(dxc.HasValue());
    ShaderArtifactResolver resolver{
        _assets,
        MakeConfig(root),
        CreateDxcShaderJitCompiler(dxc.Release())};
    auto dxil = resolver.ResolveAsync(*asset, render::ShaderTarget::DXIL, 0, {}).get();
    auto spirv = resolver.ResolveAsync(*asset, render::ShaderTarget::SPIRV, 0, {}).get();
    ASSERT_TRUE(dxil.Succeeded()) << (dxil.Diagnostics.empty() ? "" : dxil.Diagnostics.front().Message);
    ASSERT_TRUE(spirv.Succeeded()) << (spirv.Diagnostics.empty() ? "" : spirv.Diagnostics.front().Message);
    EXPECT_EQ(dxil.Program->Interface, spirv.Program->Interface);
    EXPECT_EQ(asset->GetBinary().ProgramVariants.size(), 0u);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif

}  // namespace radray
