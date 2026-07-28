// ShaderAsset / ShaderPassProgram (G1 + G4): manifest -> Asset -> 变体字节码。
//
// 【为何不并入 test_shader_asset.cpp】: 那个文件全程不碰 GPU (manifest 解析、变体域、
// artifact 索引、cook 都是纯 CPU)。本文件必须建真实 device —— ShaderAsset 的加载期
// 就要 CreatePipelineLayout。无设备时 GTEST_SKIP, 不让无 GPU 的 CI 变红。
//
// 【覆盖重点】不是"能不能加载", 而是三条设计不变量:
//   1. 加载期不碰 DXC。故 dxc == nullptr + AllowJit == false 也能构造出完整资产
//      (发布包形态), 只是后续解析变体时才失败。
//   2. 字节码缓存在 program 层。ShaderResolver 每次 Resolve 都重新读盘/重编, 若本层
//      不缓存, 重复请求同一变体会重复解析 —— 用变体指针稳定 + 缓存计数验证。
//   3. PipelineLayout 归 program, 经 OnUnload 交 recycler。

#include <radray/runtime/shader_asset.h>

#include <radray/environment.h>
#include <radray/shader/dxc.h>
#include <radray/render/rhi.h>
#include <radray/runtime/render_resource_recycler.h>
#include <radray/types.h>

#include <gtest/gtest.h>

#include <fmt/format.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <system_error>

namespace radray {
namespace {

std::filesystem::path GetProjectRoot() {
    const string fromEnv = GetEnv("RADRAY_PROJECT_DIR");
    if (!fromEnv.empty()) {
        return std::filesystem::path{fromEnv};
    }
#if defined(RADRAY_PROJECT_DIR_DEFAULT)
    return std::filesystem::path{RADRAY_PROJECT_DIR_DEFAULT};
#else
    return {};
#endif
}

std::filesystem::path GetShaderRoot() {
    return GetProjectRoot() / "shaderlib";
}

std::filesystem::path GetErrorPassManifestPath() {
    return GetShaderRoot() / "forward_pipeline" / "error_pass.shader.json";
}

/// 一个 device。本文件只需要 CreatePipelineLayout, 不提交任何命令, 故不要队列。
struct DeviceContext {
    bool VulkanEnvInitialized{false};
    unique_ptr<render::DXGIFactory> Factory;
    shared_ptr<render::Device> Device;

    ~DeviceContext() {
        Device.reset();
        Factory.reset();
#if defined(RADRAY_ENABLE_VULKAN)
        if (VulkanEnvInitialized) {
            render::InstanceVulkan::ShutdownEnv();
        }
#endif
    }
};

/// 建任一可用后端的 device。本文件的断言都与后端无关 (layout 与字节码解析两个后端
/// 同构), 故不参数化 —— 跨后端差异已由 test_vertical_slice 覆盖。
bool TryCreateAnyDevice(DeviceContext& ctx, render::ShaderBlobCategory& outCategory) {
#if defined(RADRAY_ENABLE_D3D12)
    {
        render::DXGIFactoryDescriptor factoryDesc{};
        factoryDesc.IsEnableDebugLayer = false;
        auto factory = render::DXGIFactory::Create(factoryDesc);
        if (factory.HasValue()) {
            ctx.Factory = factory.Release();
            render::D3D12DeviceDescriptor d3d12Desc{};
            d3d12Desc.Factory = ctx.Factory.get();
            auto device = render::Device::Create(render::DeviceDescriptor{d3d12Desc});
            if (device.HasValue()) {
                ctx.Device = device.Release();
                outCategory = render::ShaderBlobCategory::DXIL;
                return true;
            }
            ctx.Factory.reset();
        }
    }
#endif
#if defined(RADRAY_ENABLE_VULKAN)
    {
        render::VulkanInstanceDescriptor instanceDesc{};
        instanceDesc.AppName = "radray_shader_program_test";
        instanceDesc.EngineName = "radray";
        instanceDesc.IsEnableDebugLayer = false;
        auto instance = render::InstanceVulkan::InitEnv(instanceDesc);
        if (instance.HasValue()) {
            ctx.VulkanEnvInitialized = true;
            render::VulkanDeviceDescriptor vkDesc{};
            auto device = render::Device::Create(render::DeviceDescriptor{vkDesc});
            if (device.HasValue()) {
                ctx.Device = device.Release();
                outCategory = render::ShaderBlobCategory::SPIRV;
                return true;
            }
        }
    }
#endif
    return false;
}

/// manifest 副本所在的临时目录。cook 把产物写在 manifest 旁边, 直接烘仓库那份会往
/// 源码树塞产物目录。
class ScopedManifestCopy {
public:
    explicit ScopedManifestCopy(const std::filesystem::path& source) {
        static std::atomic<uint32_t> counter{0};
        std::error_code error;
        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(error);
        if (error) {
            return;
        }
        _dir = tempRoot / fmt::format(
                              "radray_shader_program_{}_{}",
                              std::chrono::steady_clock::now().time_since_epoch().count(),
                              counter.fetch_add(1));
        std::filesystem::create_directories(_dir, error);
        if (error) {
            _dir.clear();
            return;
        }
        _manifest = _dir / source.filename();
        std::filesystem::copy_file(
            source, _manifest, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            _manifest.clear();
        }
    }
    ~ScopedManifestCopy() {
        std::error_code error;
        if (!_dir.empty()) {
            std::filesystem::remove_all(_dir, error);
        }
    }
    ScopedManifestCopy(const ScopedManifestCopy&) = delete;
    ScopedManifestCopy& operator=(const ScopedManifestCopy&) = delete;

    bool IsValid() const noexcept { return !_manifest.empty(); }
    const std::filesystem::path& ManifestPath() const noexcept { return _manifest; }

private:
    std::filesystem::path _dir;
    std::filesystem::path _manifest;
};

/// 记录被回收的 GPU 对象数, 并真的释放它们。
class CountingRecycler : public IRenderResourceRecycler {
public:
    void RecycleRenderResource(unique_ptr<render::RenderBase> obj) noexcept override {
        if (obj != nullptr) {
            ++Count;
        }
    }

    uint32_t Count{0};
};

// ============================ AssetId ============================

TEST(ShaderAssetIdTest, PathDerivedAndNamespaced) {
    const AssetId a = MakeShaderAssetId("shaderlib/forward_pipeline/error_pass.shader.json");
    const AssetId b = MakeShaderAssetId("shaderlib/forward_pipeline/error_pass.shader.json");
    EXPECT_EQ(a, b) << "the same path must map to the same id";

    const AssetId c = MakeShaderAssetId("shaderlib/forward_pipeline/forward_pass.shader.json");
    EXPECT_NE(a, c);

    // 源码树与输出目录各有一份 manifest, 它们是两份文件, 故 id 必须不同。
    const AssetId d = MakeShaderAssetId("build/shaderlib/forward_pipeline/error_pass.shader.json");
    EXPECT_NE(a, d);
}

// ============================ 加载 ============================

class ShaderAssetLoadTest : public testing::Test {
protected:
    void SetUp() override {
        if (GetProjectRoot().empty()) {
            GTEST_SKIP() << "the project root is unknown";
        }
        if (!std::filesystem::is_regular_file(GetErrorPassManifestPath())) {
            GTEST_SKIP() << "the error_pass manifest is missing";
        }
        if (!TryCreateAnyDevice(_ctx, _category)) {
            GTEST_SKIP() << "no render backend is available on this machine";
        }
    }

    render::Device& Device() { return *_ctx.Device; }
    render::ShaderBlobCategory Category() const noexcept { return _category; }

private:
    DeviceContext _ctx;
    render::ShaderBlobCategory _category{render::ShaderBlobCategory::DXIL};
};

TEST_F(ShaderAssetLoadTest, ManifestBecomesAssetWithoutTouchingDxc) {
    // dxc 不给、JIT 不许, 资产仍应构造成功 —— 这就是发布包的加载形态。加载期若偷偷
    // 编译了任何字节码, 这个用例会失败。
    const ShaderAssetLoadOptions options{
        .ShaderRoot = GetShaderRoot(),
        .Staleness = ShaderArtifactStaleness::Strict,
        .AllowJit = false,
        .Dxc = nullptr};

    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(Device(), GetErrorPassManifestPath(), options, diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    EXPECT_EQ(asset->GetName(), "ErrorPass");
    EXPECT_TRUE(asset->IsValid());
    ASSERT_EQ(asset->GetPassCount(), 1u);

    Nullable<ShaderPassProgram*> pass = asset->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());
    EXPECT_EQ(pass->GetName(), "Error");
    EXPECT_TRUE(pass->GetPipelineLayout().HasValue()) << "the layout is built at load time";
    // error_pass 只有 POSITION, 但 VertexInput 必须已就绪 —— 它同样是 variant 无关的。
    EXPECT_TRUE(pass->GetVertexInputState().has_value());
    // 加载期不解析任何字节码。
    EXPECT_EQ(pass->GetCachedVariantCount(), 0u);
    EXPECT_EQ(pass->GetCachedBytecodeCount(), 0u);

    EXPECT_FALSE(asset->FindPass("NoSuchPass").HasValue());

    // Source 已展开为最终路径 (manifest 里 pass.Source 是空的, 继承资产级)。
    EXPECT_EQ(pass->GetDesc().Source, "forward_pipeline/error_pass.hlsl");
}

TEST_F(ShaderAssetLoadTest, MissingManifestFails) {
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Device(),
        GetShaderRoot() / "forward_pipeline" / "no_such_pass.shader.json",
        ShaderAssetLoadOptions{.ShaderRoot = GetShaderRoot()},
        diagnostic);
    EXPECT_FALSE(asset.HasValue());
    EXPECT_FALSE(diagnostic.Message.empty());
}

TEST_F(ShaderAssetLoadTest, VariantResolveFailsWithoutJitOrArtifact) {
    // 无产物 + 无 DXC: 加载成功但变体解析必须失败, 且失败不写缓存。
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Device(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{
            .ShaderRoot = GetShaderRoot(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = false,
            .Dxc = nullptr},
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    Nullable<ShaderPassProgram*> pass = asset->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());

    ShaderAssetDiagnostic resolveDiag;
    EXPECT_FALSE(pass->GetOrCreateDefaultVariant(Category(), resolveDiag).HasValue());
    EXPECT_FALSE(resolveDiag.Message.empty());
    EXPECT_EQ(pass->GetCachedVariantCount(), 0u) << "a failed resolve must not cache a variant";
}

TEST_F(ShaderAssetLoadTest, JitVariantIsCachedAndStable) {
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Device(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{
            .ShaderRoot = GetShaderRoot(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = true,
            .Dxc = dxc.get()},
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    Nullable<ShaderPassProgram*> pass = asset->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());

    ShaderAssetDiagnostic resolveDiag;
    Nullable<const ShaderProgramVariant*> first =
        pass->GetOrCreateDefaultVariant(Category(), resolveDiag);
    ASSERT_TRUE(first.HasValue()) << resolveDiag.ToString();

    // VS + PS。
    EXPECT_EQ(first->Stages().size(), 2u);
    for (const ShaderProgramVariant::StageBlob& blob : first->Stages()) {
        ASSERT_NE(blob.Bytecode, nullptr);
        EXPECT_FALSE(blob.Bytecode->Data.empty());
        EXPECT_EQ(blob.Bytecode->Category, Category());
        EXPECT_EQ(blob.Bytecode->Source, ShaderBytecodeSource::Jit);
    }
    EXPECT_EQ(
        first->FindEntryPoint(render::ShaderStage::Vertex).value_or(std::string_view{}),
        "VSMain");
    EXPECT_EQ(
        first->FindEntryPoint(render::ShaderStage::Pixel).value_or(std::string_view{}),
        "PSMain");
    EXPECT_TRUE(first->FindBytecode(render::ShaderStage::Compute) == nullptr);

    EXPECT_EQ(pass->GetCachedVariantCount(), 1u);
    EXPECT_EQ(pass->GetCachedBytecodeCount(), 2u);

    // 二次请求必须命中缓存: 指针相同, 且没有新增条目。若本层不缓存, resolver 会
    // 重新编译一遍并返回不同的存储。
    Nullable<const ShaderProgramVariant*> second =
        pass->GetOrCreateDefaultVariant(Category(), resolveDiag);
    ASSERT_TRUE(second.HasValue());
    EXPECT_EQ(first.Get(), second.Get());
    EXPECT_EQ(pass->GetCachedVariantCount(), 1u);
    EXPECT_EQ(pass->GetCachedBytecodeCount(), 2u);
}

TEST_F(ShaderAssetLoadTest, StageBytecodeIsSharedAcrossVariantsThatProjectTheSame) {
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Device(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{
            .ShaderRoot = GetShaderRoot(),
            .Staleness = ShaderArtifactStaleness::Strict,
            .AllowJit = true,
            .Dxc = dxc.get()},
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    Nullable<ShaderPassProgram*> pass = asset->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());

    // error_pass 的两组 keyword 都只作用于 Pixel, 故开启 _POINT_SHADOWS 后 Vertex
    // 的投影结果不变 —— VS 字节码必须被复用, 而不是重编一份。这是 ProjectToStage
    // 那条不变量在 program 层的体现。
    ShaderAssetDiagnostic resolveDiag;
    Nullable<const ShaderProgramVariant*> base =
        pass->GetOrCreateDefaultVariant(Category(), resolveDiag);
    ASSERT_TRUE(base.HasValue()) << resolveDiag.ToString();
    const ShaderBytecode* baseVs = base->FindBytecode(render::ShaderStage::Vertex).Get();
    ASSERT_NE(baseVs, nullptr);

    const std::optional<ShaderVariantKey> shadowed =
        pass->GetDomain().WithKeyword(
            pass->GetDomain().DefaultVariant(), "_POINT_SHADOWS", true);
    ASSERT_TRUE(shadowed.has_value());

    Nullable<const ShaderProgramVariant*> variant =
        pass->GetOrCreateVariant(shadowed.value(), Category(), resolveDiag);
    ASSERT_TRUE(variant.HasValue()) << resolveDiag.ToString();
    EXPECT_NE(base.Get(), variant.Get()) << "two variants are two entries";

    EXPECT_EQ(variant->FindBytecode(render::ShaderStage::Vertex).Get(), baseVs)
        << "the vertex stage projects identically, so the bytecode must be shared";

    EXPECT_EQ(pass->GetCachedVariantCount(), 2u);
    // VS 共用一条, PS 两条。
    EXPECT_EQ(pass->GetCachedBytecodeCount(), 3u);
}

TEST_F(ShaderAssetLoadTest, CookedArtifactResolvesWithoutDxc) {
#if !defined(RADRAY_ENABLE_SHADER_JIT)
    GTEST_SKIP() << "cook requires shader JIT support";
#else
    auto dxcResult = render::CreateDxc();
    if (!dxcResult.HasValue()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    shared_ptr<render::Dxc> dxc = dxcResult.Release();

    ScopedManifestCopy workspace{GetErrorPassManifestPath()};
    ASSERT_TRUE(workspace.IsValid());

    const vector<render::ShaderBlobCategory> categories{Category()};
    const ShaderCookResult cook = CookShaderAssetFile(
        *dxc,
        ShaderCookOptions{
            .ShaderRoot = GetShaderRoot(),
            .ManifestPath = workspace.ManifestPath(),
            .Categories = categories,
            .ValidateReflection = true,
            .Incremental = false});
    string cookErrors;
    for (const ShaderAssetDiagnostic& d : cook.Diagnostics) {
        cookErrors += d.ToString() + "\n";
    }
    ASSERT_TRUE(cook.Succeeded()) << cookErrors;

    // dxc 传 nullptr: 发布包里 DXC 根本不存在。
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Device(),
        workspace.ManifestPath(),
        ShaderAssetLoadOptions{
            .ShaderRoot = GetShaderRoot(),
            .Staleness = ShaderArtifactStaleness::Lenient,
            .AllowJit = false,
            .Dxc = nullptr},
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    Nullable<ShaderPassProgram*> pass = asset->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());

    ShaderAssetDiagnostic resolveDiag;
    Nullable<const ShaderProgramVariant*> variant =
        pass->GetOrCreateDefaultVariant(Category(), resolveDiag);
    ASSERT_TRUE(variant.HasValue()) << resolveDiag.ToString();
    for (const ShaderProgramVariant::StageBlob& blob : variant->Stages()) {
        ASSERT_NE(blob.Bytecode, nullptr);
        EXPECT_EQ(blob.Bytecode->Source, ShaderBytecodeSource::Artifact)
            << "falling back to JIT here would make the release path untested";
        EXPECT_TRUE(cook.Index.Find(blob.Bytecode->Key).HasValue());
    }
#endif
}

TEST_F(ShaderAssetLoadTest, OnUnloadHandsPipelineLayoutToRecycler) {
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Device(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{
            .ShaderRoot = GetShaderRoot(),
            .AllowJit = false,
            .Dxc = nullptr},
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    CountingRecycler recycler;
    asset->OnUnload(recycler);
    // 一个 pass -> 一个 PipelineLayout。
    EXPECT_EQ(recycler.Count, 1u);
    EXPECT_FALSE(asset->IsValid());
    EXPECT_FALSE(asset->FindPass("Error").HasValue());
}

TEST_F(ShaderAssetLoadTest, LoadsThroughAssetManager) {
    AssetManager assetManager;
    CountingRecycler recycler;
    assetManager.SetRecycler(&recycler);

    const std::filesystem::path manifestPath = GetErrorPassManifestPath();
    StreamingAssetRef<ShaderAsset> ref = LoadShaderAsset(
        assetManager,
        Device(),
        manifestPath,
        ShaderAssetLoadOptions{
            .ShaderRoot = GetShaderRoot(),
            .AllowJit = false,
            .Dxc = nullptr});

    // 加载协程无挂起点, 一次 Pump 即达终态 —— 这就是"加载期不碰 DXC"的可观测证据:
    // 若它同步跑了 JIT, 这里同样会 Ready, 但泵线程会被卡住数百毫秒。
    assetManager.Pump();
    ASSERT_TRUE(ref.IsReady()) << "the shader asset did not become ready after one pump";

    ShaderAsset* asset = ref.Get();
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->GetName(), "ErrorPass");
    EXPECT_TRUE(asset->FindPass("Error").HasValue());
    EXPECT_EQ(asset->GetAssetId(), MakeShaderAssetId(manifestPath));

    // 按 id 去重: 同一路径再加载应命中同一 slot。
    StreamingAssetRef<ShaderAsset> again = LoadShaderAsset(
        assetManager,
        Device(),
        manifestPath,
        ShaderAssetLoadOptions{.ShaderRoot = GetShaderRoot(), .AllowJit = false});
    EXPECT_EQ(again.GetHandle(), ref.GetHandle());
    EXPECT_EQ(assetManager.GetAssetCount(), 1u);

    assetManager.Unload(asset->GetAssetId());
    EXPECT_EQ(recycler.Count, 1u) << "OnUnload must route the layout through the recycler";
    EXPECT_EQ(ref.Get(), nullptr);
}

TEST_F(ShaderAssetLoadTest, AssetManagerReportsFailureForMissingManifest) {
    AssetManager assetManager;
    StreamingAssetRef<ShaderAsset> ref = LoadShaderAsset(
        assetManager,
        Device(),
        GetShaderRoot() / "forward_pipeline" / "no_such_pass.shader.json",
        ShaderAssetLoadOptions{.ShaderRoot = GetShaderRoot()});
    assetManager.Pump();
    EXPECT_TRUE(ref.IsFaulted());
    EXPECT_EQ(ref.Get(), nullptr);
}

}  // namespace
}  // namespace radray
