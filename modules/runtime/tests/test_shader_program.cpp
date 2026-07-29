// ShaderAsset / ShaderPassProgram: manifest -> Asset -> 变体字节码。
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

/// 一个解析上下文 + (可选) 它借用的 DXC。
///
/// 【为何要把两者绑在一起】: context 只借用 Dxc* 裸指针, 必须比它先死。测试里最容易
/// 犯的错是让 dxc 是局部变量而 context 是成员, 于是先析构 dxc —— 用一个对象把析构
/// 顺序 (声明序 dxc 在前, 逆序析构 context 在前) 固定下来。
class ScopedResolveContext {
public:
    ScopedResolveContext(render::Device& device, ShaderArtifactStaleness staleness, bool withJit) {
        if (withJit) {
            auto dxcResult = render::CreateDxc();
            if (dxcResult.HasValue()) {
                _dxc = dxcResult.Release();
            }
        }
        _context = make_unique<ShaderResolveContext>(
            ShaderResolveSettings{
                .ShaderRoot = GetShaderRoot(),
                .Staleness = staleness,
                .AllowJit = withJit},
            _dxc.get());
        _layouts = make_unique<PipelineLayoutCache>(&device);
    }

    /// withJit == true 但机器上没有 DXC 时为 false, 调用方应 GTEST_SKIP。
    bool HasJit() const noexcept { return _dxc != nullptr; }
    ShaderResolveContext* Get() noexcept { return _context.get(); }
    PipelineLayoutCache& Layouts() noexcept { return *_layouts; }
    ShaderAssetLoadOptions Options() noexcept {
        return ShaderAssetLoadOptions{.Context = _context.get(), .LayoutCache = _layouts.get()};
    }

private:
    shared_ptr<render::Dxc> _dxc;
    /// 【必须声明在 _dxc 之后】: 借用 Dxc*, 析构逆序保证 context 先死。
    unique_ptr<ShaderResolveContext> _context;
    /// 【刻意允许它先于资产死】: 缓存只是非拥有索引, 残留 layout 由 program 的引用计数
    /// 保命 —— 照 RenderSystem 先于 AssetManager 关停的顺序。
    unique_ptr<PipelineLayoutCache> _layouts;
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

/// 同一份文件的不同写法必须得到同一个 id。
///
/// 【不归一化会静默建出两个资产】: 各持一套 PipelineLayout 与字节码缓存, 表现为
/// "shader 编了两遍、layout 缓存命中率减半", 且没有任何报错。
TEST(ShaderAssetIdTest, PathIsNormalizedBeforeHashing) {
    const AssetId plain = MakeShaderAssetId("shaderlib/forward_pipeline/error_pass.shader.json");

    EXPECT_EQ(plain, MakeShaderAssetId("shaderlib/./forward_pipeline/error_pass.shader.json"))
        << "'.' must be removed";
    EXPECT_EQ(plain, MakeShaderAssetId("shaderlib/lighting/../forward_pipeline/error_pass.shader.json"))
        << "'..' must be resolved";
    EXPECT_EQ(plain, MakeShaderAssetId("shaderlib//forward_pipeline//error_pass.shader.json"))
        << "duplicate separators must collapse";
    EXPECT_EQ(plain, MakeShaderAssetId("shaderlib\\forward_pipeline\\error_pass.shader.json"))
        << "the separator flavor must not matter";

    // 相对路径按 CWD 解析, 故显式绝对化后必须仍是同一个 id。
    EXPECT_EQ(
        plain,
        MakeShaderAssetId(
            std::filesystem::current_path() / "shaderlib/forward_pipeline/error_pass.shader.json"));

#if defined(_WIN32)
    // NTFS 路径大小写不敏感, 而 weakly_canonical 不做这层归一化。POSIX 下这条【必须】
    // 不成立 —— 那里大小写是显著的, 故用例本身也分平台。
    EXPECT_EQ(plain, MakeShaderAssetId("ShaderLib/Forward_Pipeline/Error_Pass.Shader.Json"));
#endif
}

/// 命名空间前缀隔离: 抽出公共 MakeAssetIdFromPath 后, 两种资产仍不可撞。
TEST(ShaderAssetIdTest, NamespacedAgainstOtherAssetTypes) {
    const std::filesystem::path path = "shaderlib/forward_pipeline/error_pass.shader.json";
    EXPECT_NE(MakeAssetIdFromPath("shader", path), MakeAssetIdFromPath("image", path));
    EXPECT_EQ(MakeShaderAssetId(path), MakeAssetIdFromPath("shader", path));
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
        _assets.SetRecycler(&_recycler);
    }

    render::Device& Device() { return *_ctx.Device; }
    render::ShaderBlobCategory Category() const noexcept { return _category; }

    /// 【为何 CreateShaderAsset 需要它】: ShaderContent 只能经 AssetManager::MakeContent
    /// 创建 (recycler 由那里注入), 见 AssetContentKey。这个 manager 不参与 id 去重 ——
    /// CreateShaderAsset 是直建路径, 不走 Load。
    AssetManager& Assets() { return _assets; }
    CountingRecycler& Recycler() { return _recycler; }

private:
    DeviceContext _ctx;
    render::ShaderBlobCategory _category{render::ShaderBlobCategory::DXIL};
    CountingRecycler _recycler;
    /// 【必须声明在 _recycler 之后】: 内容归零时把 GPU 对象交给 recycler, 析构逆序保证
    /// manager (及其持有的资产) 先死。
    AssetManager _assets;
};

TEST_F(ShaderAssetLoadTest, ManifestBecomesAssetWithoutTouchingDxc) {
    // dxc 不给、JIT 不许, 资产仍应构造成功 —— 这就是发布包的加载形态。加载期若偷偷
    // 编译了任何字节码, 这个用例会失败。
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};

    ShaderAssetDiagnostic diagnostic;
    auto asset =
        CreateShaderAsset(Assets(), GetErrorPassManifestPath(), context.Options(), diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    // 内容访问全部经 AcquireContent —— 资产本身刻意不转发, 见 ShaderAsset::AcquireContent。
    ShaderContentRef content = asset->AcquireContent();
    ASSERT_TRUE(content.HasValue());
    EXPECT_EQ(content->GetName(), "ErrorPass");
    EXPECT_TRUE(content->IsValid());
    ASSERT_EQ(content->GetPassCount(), 1u);

    Nullable<ShaderPassProgram*> pass = content->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());
    EXPECT_EQ(pass->GetName(), "Error");
    EXPECT_TRUE(pass->GetPipelineLayout().HasValue()) << "the layout is built at load time";
    // error_pass 只有 POSITION, 但 VertexInput 必须已就绪 —— 它同样是 variant 无关的。
    EXPECT_TRUE(pass->GetVertexInputState().has_value());
    // 加载期不解析任何字节码。
    EXPECT_EQ(pass->GetCachedVariantCount(), 0u);
    EXPECT_EQ(pass->GetCachedBytecodeCount(), 0u);

    EXPECT_FALSE(content->FindPass("NoSuchPass").HasValue());

    // Source 已展开为最终路径 (manifest 里 pass.Source 是空的, 继承资产级)。
    EXPECT_EQ(pass->GetDesc().Source, "forward_pipeline/error_pass.hlsl");
}

TEST_F(ShaderAssetLoadTest, MissingManifestFails) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetShaderRoot() / "forward_pipeline" / "no_such_pass.shader.json",
        context.Options(),
        diagnostic);
    EXPECT_FALSE(asset.HasValue());
    EXPECT_FALSE(diagnostic.Message.empty());
}

/// 没有 context 就没有 include 根、没有过期策略、没有 JIT 许可。旧版在 ShaderRoot
/// 留空时按"父目录的父目录"猜 include 根, 那个兜底恰恰说明当时拿不到唯一真相。
TEST_F(ShaderAssetLoadTest, MissingResolveContextFails) {
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{},
        diagnostic);
    EXPECT_FALSE(asset.HasValue());
    EXPECT_NE(diagnostic.Message.find("Context"), string::npos) << diagnostic.ToString();
}

/// 不提供"绕过缓存直接建 layout"的回退 —— 那会让 layout 的所有权有两条路径。
TEST_F(ShaderAssetLoadTest, MissingLayoutCacheFails) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{.Context = context.Get()},
        diagnostic);
    EXPECT_FALSE(asset.HasValue());
    EXPECT_NE(diagnostic.Message.find("LayoutCache"), string::npos) << diagnostic.ToString();
}

TEST_F(ShaderAssetLoadTest, VariantResolveFailsWithoutJitOrArtifact) {
    // 无产物 + 无 DXC: 加载成功但变体解析必须失败, 且失败不写缓存。
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        context.Options(),
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    ShaderContentRef content = asset->AcquireContent();
    ASSERT_TRUE(content.HasValue());
    Nullable<ShaderPassProgram*> pass = content->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());

    ShaderAssetDiagnostic resolveDiag;
    EXPECT_FALSE(pass->GetOrCreateDefaultVariant(Category(), resolveDiag).HasValue());
    EXPECT_FALSE(resolveDiag.Message.empty());
    EXPECT_EQ(pass->GetCachedVariantCount(), 0u) << "a failed resolve must not cache a variant";
}

TEST_F(ShaderAssetLoadTest, JitVariantIsCachedAndStable) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, true};
    if (!context.HasJit()) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        context.Options(),
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    ShaderContentRef content = asset->AcquireContent();
    ASSERT_TRUE(content.HasValue());
    Nullable<ShaderPassProgram*> pass = content->FindPass("Error");
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
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, true};
    if (!context.HasJit()) {
        GTEST_SKIP() << "DXC is unavailable";
    }

    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        context.Options(),
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    ShaderContentRef content = asset->AcquireContent();
    ASSERT_TRUE(content.HasValue());
    Nullable<ShaderPassProgram*> pass = content->FindPass("Error");
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

    // context 不给 dxc: 发布包里 DXC 根本不存在。给了指针再关 AllowJit 只测到
    // "我们没去用它"。
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Lenient, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        workspace.ManifestPath(),
        context.Options(),
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    ShaderContentRef content = asset->AcquireContent();
    ASSERT_TRUE(content.HasValue());
    Nullable<ShaderPassProgram*> pass = content->FindPass("Error");
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

/// 【layout 不走 recycler】: 它是引用计数的共享对象, 归零即从 PipelineLayoutCache 摘除
/// 并销毁 (见 pipeline_layout_cache.h)。这里守住的是"OnUnload 确实放开了那份引用",
/// 观测点是缓存里的条目数, 不是 recycler 计数。
TEST_F(ShaderAssetLoadTest, OnUnloadReleasesSharedPipelineLayout) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        context.Options(),
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();
    ASSERT_EQ(context.Layouts().GetLayoutCount(), 1u) << "one pass -> one layout";

    // OnUnload 只放开槽位那份内容引用。此处无人持有内容, 故它随即归零并释放 layout。
    CountingRecycler recycler;
    asset->OnUnload(recycler);
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 0u)
        << "the layout must self-destruct once the last program drops its reference";
    EXPECT_EQ(recycler.Count, 0u) << "the shared layout must not go through the recycler";
    EXPECT_FALSE(asset->HasContent());
    EXPECT_FALSE(asset->AcquireContent().HasValue());
}

/// 【守的不变量】: 有人持有内容时, OnUnload 不得释放 GPU 资源。
///
/// 这是内容/槽位分离要买到的核心性质 —— 分离前 OnUnload 会无条件 clear passes,
/// 于是任何缓存了 ShaderPassProgram* 的地方 (PSO 缓存条目) 立刻悬垂。
TEST_F(ShaderAssetLoadTest, ContentOutlivesTheSlotWhileSomeoneHoldsIt) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        context.Options(),
        diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();
    ASSERT_EQ(context.Layouts().GetLayoutCount(), 1u);

    ShaderContentRef held = asset->AcquireContent();
    ASSERT_TRUE(held.HasValue());
    EXPECT_EQ(held->GetRefCount(), 2u) << "asset's one + this local one";

    CountingRecycler recycler;
    asset->OnUnload(recycler);
    EXPECT_FALSE(asset->HasContent()) << "the slot dropped its reference";

    // 内容仍活着, 里面的 program 与 layout 都还能安全使用。
    EXPECT_EQ(held->GetRefCount(), 1u);
    EXPECT_TRUE(held->IsValid());
    Nullable<ShaderPassProgram*> pass = held->FindPass("Error");
    ASSERT_TRUE(pass.HasValue());
    EXPECT_TRUE(pass->GetPipelineLayout().HasValue());
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 1u)
        << "the layout must still be alive while the content is held";

    // 放开最后一份引用, 此刻才真正释放。
    held.Reset();
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 0u);
}

/// 【守的不变量】: 内容可以比创建它的 AssetManager 活得久。
///
/// 这是 AssetContent 自持 recycler 指针的全部理由 (见 asset.h)。若归零时回头向
/// AssetManager 索取 recycler, 本用例就会在 manager 已析构的情况下解引用悬垂指针 ——
/// 分离也就白做了: 内容重新依赖 AssetManager 存活。
TEST_F(ShaderAssetLoadTest, ContentOutlivesTheAssetManagerItself) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    CountingRecycler recycler;
    ShaderContentRef held;
    {
        // 【局部 manager, 刻意先于 recycler 与 held 析构】: 这正是 app 的关停顺序 ——
        // GpuSystem (recycler) 最后销毁, 见 Application::Shutdown。
        AssetManager assets;
        assets.SetRecycler(&recycler);
        ShaderAssetDiagnostic diagnostic;
        auto asset = CreateShaderAsset(assets, GetErrorPassManifestPath(), context.Options(), diagnostic);
        ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();
        held = asset->AcquireContent();
        ASSERT_TRUE(held.HasValue());
    }
    // manager 与资产都没了, 内容仍是完好的。
    ASSERT_EQ(held->GetRefCount(), 1u);
    EXPECT_TRUE(held->IsValid());
    EXPECT_TRUE(held->FindPass("Error").HasValue());
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 1u);

    // 归零走的是内容自己记下的那份 recycler, 而不是任何还需要 manager 的路径。
    held.Reset();
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 0u);
}

TEST_F(ShaderAssetLoadTest, LoadsThroughAssetManager) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    AssetManager assetManager;
    CountingRecycler recycler;
    assetManager.SetRecycler(&recycler);

    const std::filesystem::path manifestPath = GetErrorPassManifestPath();
    StreamingAssetRef<ShaderAsset> ref = LoadShaderAsset(
        assetManager,
        manifestPath,
        context.Options());

    // 加载协程无挂起点, 一次 Pump 即达终态 —— 这就是"加载期不碰 DXC"的可观测证据:
    // 若它同步跑了 JIT, 这里同样会 Ready, 但泵线程会被卡住数百毫秒。
    assetManager.Pump();
    ASSERT_TRUE(ref.IsReady()) << "the shader asset did not become ready after one pump";

    ShaderAsset* asset = ref.Get();
    ASSERT_NE(asset, nullptr);
    ShaderContentRef content = asset->AcquireContent();
    ASSERT_TRUE(content.HasValue());
    EXPECT_EQ(content->GetName(), "ErrorPass");
    EXPECT_TRUE(content->FindPass("Error").HasValue());
    EXPECT_EQ(asset->GetAssetId(), MakeShaderAssetId(manifestPath));

    // 按 id 去重: 同一路径再加载应命中同一 slot。
    StreamingAssetRef<ShaderAsset> again = LoadShaderAsset(
        assetManager,
        manifestPath,
        context.Options());
    EXPECT_EQ(again.GetHandle(), ref.GetHandle());
    EXPECT_EQ(assetManager.GetAssetCount(), 1u);

    assetManager.Unload(asset->GetAssetId());
    EXPECT_EQ(ref.Get(), nullptr) << "the slot is gone";
    // 【本地仍持有 content, 故 layout 必须还活着】: Unload 销毁的是槽位, 不是内容。
    // 分离之前这里是 0 —— 那时 OnUnload 无条件清掉 passes, 于是任何缓存了
    // ShaderPassProgram* 的地方立刻悬垂。
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 1u)
        << "the content is still held, so its layout must stay alive";

    content.Reset();
    EXPECT_EQ(context.Layouts().GetLayoutCount(), 0u)
        << "dropping the last content reference must release the shared layout";
}

/// options 必须在【发起加载之前】被校验。
///
/// 【为何这条不能靠 CreateShaderAsset 的校验覆盖】: AssetManager::Load 命中既有 slot 时
/// 协程一次都不 resume, 那些校验根本不执行。故本用例的重点是"空 options 连 slot 都不该
/// 占", 而不是"加载会失败"。
TEST_F(ShaderAssetLoadTest, InvalidOptionsAreRejectedBeforeOccupyingASlot) {
    // 用 fixture 的 manager: 它已装配 recycler。裸 AssetManager 现在会在建内容时 abort
    // (无兜底, 见 AssetManager::GetRecycler), 而每个 fixture 实例本就是全新的 manager,
    // 故"不占 id"这条断言不受影响。
    AssetManager& assetManager = Assets();
    const std::filesystem::path manifestPath = GetErrorPassManifestPath();

    StreamingAssetRef<ShaderAsset> noOptions =
        LoadShaderAsset(assetManager, manifestPath, ShaderAssetLoadOptions{});
    EXPECT_FALSE(noOptions.IsValid());
    // 【不占 id 是关键】: 若这里发起了一个注定 Faulted 的加载, id 会被占住, 下面拿对
    // options 的重试就会被 dedup 命中那个坏 slot。
    EXPECT_EQ(assetManager.GetAssetCount(), 0u);

    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    StreamingAssetRef<ShaderAsset> noCache = LoadShaderAsset(
        assetManager,
        manifestPath,
        ShaderAssetLoadOptions{.Context = context.Get()});
    EXPECT_FALSE(noCache.IsValid());
    EXPECT_EQ(assetManager.GetAssetCount(), 0u);

    // 同一 id 拿对 options 重试必须成功 —— 证明前两次真的没留下痕迹。
    StreamingAssetRef<ShaderAsset> good =
        LoadShaderAsset(assetManager, manifestPath, context.Options());
    assetManager.Pump();
    ASSERT_TRUE(good.IsReady());
    EXPECT_EQ(assetManager.GetAssetCount(), 1u);

    assetManager.Unload(good.GetAssetId());
}

/// 资产记下自己是用哪一份共享设施建的, 使 dedup 命中可核对。
TEST_F(ShaderAssetLoadTest, AssetRecordsTheSharedFacilitiesItWasBuiltWith) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diagnostic;
    auto asset = CreateShaderAsset(Assets(), GetErrorPassManifestPath(), context.Options(), diagnostic);
    ASSERT_TRUE(asset.HasValue()) << diagnostic.ToString();

    EXPECT_EQ(asset->GetResolveContext().Get(), context.Get());
    EXPECT_EQ(asset->GetLayoutCache().Get(), &context.Layouts());

    CountingRecycler recycler;
    asset->OnUnload(recycler);
}

TEST_F(ShaderAssetLoadTest, AssetManagerReportsFailureForMissingManifest) {
    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    AssetManager& assetManager = Assets();
    StreamingAssetRef<ShaderAsset> ref = LoadShaderAsset(
        assetManager,
        GetShaderRoot() / "forward_pipeline" / "no_such_pass.shader.json",
        context.Options());
    assetManager.Pump();
    EXPECT_TRUE(ref.IsFaulted());
    EXPECT_EQ(ref.Get(), nullptr);
}

/// 两份不同的 manifest 共享一个 context, 于是共享源码缓存。
///
/// 【这是拆出 ShaderResolveContext 的实质收益, 必须有用例守住】: error_pass 与
/// forward_pass 的 include 闭包高度重叠 (前者是后者的子集)。若缓存仍留在
/// ShaderResolver 上 (一份 manifest 一个), 重叠的头文件会被逐份重读。这里断言第二份
/// 资产的解析【没有】把文件读次数翻倍。
TEST_F(ShaderAssetLoadTest, TwoAssetsShareOneSourceCache) {
    const std::filesystem::path forwardManifest =
        GetShaderRoot() / "forward_pipeline" / "forward_pass.shader.json";
    if (!std::filesystem::is_regular_file(forwardManifest)) {
        GTEST_SKIP() << "the forward_pass manifest is missing";
    }

    ScopedResolveContext context{Device(), ShaderArtifactStaleness::Strict, false};
    ShaderAssetDiagnostic diag;

    // 先算 forward_pass (较大的闭包), 再算 error_pass (它的子集)。
    const std::optional<ShaderHash> forwardIdentity =
        context.Get()->GetSourceIdentity("forward_pipeline/forward_pass.hlsl", diag);
    ASSERT_TRUE(forwardIdentity.has_value()) << diag.ToString();
    const uint32_t readsAfterForward = context.Get()->GetSourceCacheStats().FileReads;
    ASSERT_GT(readsAfterForward, 1u) << "forward_pass has a multi-file include closure";

    const std::optional<ShaderHash> errorIdentity =
        context.Get()->GetSourceIdentity("forward_pipeline/error_pass.hlsl", diag);
    ASSERT_TRUE(errorIdentity.has_value()) << diag.ToString();
    EXPECT_NE(forwardIdentity.value(), errorIdentity.value());

    // error_pass 自己的入口文件是新的, 但它 include 的头已在缓存里。故增量必须远小于
    // 它的闭包规模 —— 若缓存没生效, 增量会等于整个闭包。
    const uint32_t readsAfterError = context.Get()->GetSourceCacheStats().FileReads;
    EXPECT_LT(readsAfterError - readsAfterForward, readsAfterForward)
        << "the shared headers must not be re-read for the second manifest";

    // 两份资产各持一个 resolver, 但共享这一个 context。
    auto forwardAsset = CreateShaderAsset(Assets(), forwardManifest, context.Options(), diag);
    ASSERT_TRUE(forwardAsset.HasValue()) << diag.ToString();
    auto errorAsset =
        CreateShaderAsset(Assets(), GetErrorPassManifestPath(), context.Options(), diag);
    ASSERT_TRUE(errorAsset.HasValue()) << diag.ToString();

    CountingRecycler recycler;
    forwardAsset->OnUnload(recycler);
    errorAsset->OnUnload(recycler);
}

}  // namespace
}  // namespace radray
