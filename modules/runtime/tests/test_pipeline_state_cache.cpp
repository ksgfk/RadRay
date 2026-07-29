// PipelineStateCache: program + 变体 + 固定功能状态 -> graphics PSO, 并按 key 缓存。
//
// 【覆盖重点】是 key 的组成, 不是"能不能建 PSO" —— 后者已由 test_vertical_slice 端到端
// 覆盖 (它还把像素读回来断言了)。本文件逐项验证 key 的每一维真的参与了身份:
//   1. 同 key 命中同一对象;
//   2. 固定功能状态的每一类差异 (primitive / color target) 各自分条;
//   3. 不同 program 分条;
//   4. 不同变体分条 —— 这条验的是 ShaderHash 进了 key, 而 ShaderVariantKey 没有;
//   5. RemovePipelineStatesUsing 按资产逐出;
//   6. 无 DXC 无产物时 miss 返回 nullptr 且不写缓存。
//
// 不参数化后端: PSO key 的行为与后端无关 (两个后端都只在建 PSO 时消费字节码), 跨后端
// 差异由 test_vertical_slice 覆盖。这里取任一可用后端即可。

#include <radray/runtime/gpu_resource.h>
#include <radray/runtime/shader_asset.h>

#include <radray/environment.h>
#include <radray/shader/dxc.h>
#include <radray/render/rhi.h>
#include <radray/runtime/asset_manager.h>
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

constexpr render::TextureFormat kTargetFormat = render::TextureFormat::RGBA8_UNORM;

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

/// 一个 device。本文件只建 RenderPass 与 PSO, 不提交命令, 故不要队列。
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
        instanceDesc.AppName = "radray_pipeline_state_cache_test";
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

/// 卸载时 GPU 对象走这里。本文件不做延迟销毁, 就地释放即可。
class NoopRecycler : public IRenderResourceRecycler {
public:
    void RecycleRenderResource(unique_ptr<render::RenderBase>) noexcept override {}
};

class PipelineStateCacheTest : public testing::Test {
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

        _assets = make_unique<AssetManager>();
        _assets->SetRecycler(&_recycler);

        // 单色 RT 的 render pass。PSO 只需要一个兼容类, 不需要 framebuffer。
        _colorAttachments[0] = render::RenderPassColorAttachmentDescriptor{
            .Format = kTargetFormat,
            .SampleCount = 1,
            .Load = render::LoadAction::Clear,
            .Store = render::StoreAction::Store};
        const render::RenderPassDescriptor passDesc{
            .ColorAttachments = _colorAttachments,
            .DepthStencilAttachment = std::nullopt};
        auto passResult = _ctx.Device->CreateRenderPass(passDesc);
        if (!passResult.HasValue()) {
            GTEST_SKIP() << "CreateRenderPass failed";
        }
        _renderPass = passResult.Release();
    }

    void TearDown() override {
        // PSO 存 RenderPass 与 PipelineLayout 裸指针, 必须先死。这里的顺序就是
        // RenderSystem 析构里那条约束的缩影。
        _cache.reset();
        _assets.reset();
        _renderPass.reset();
    }

    /// 建一个能 JIT 的资产。DXC 不可用时返回空 ref。
    StreamingAssetRef<ShaderAsset> LoadJitAsset() {
        if (JitContext() == nullptr) {
            return nullptr;
        }
        StreamingAssetRef<ShaderAsset> ref = LoadShaderAsset(
            *_assets,
            GetErrorPassManifestPath(),
            ShaderAssetLoadOptions{.Context = JitContext(), .LayoutCache = &Layouts()});
        _assets->Pump();
        return ref;
    }

    /// 惰性建立"开发构建"上下文 (Strict + JIT)。DXC 不可用时返回 nullptr。
    /// 一个 fixture 一个 context —— 这就是真实系统里 RenderSystem 持有的那一份。
    ShaderResolveContext* JitContext() {
        if (_jitContext == nullptr) {
            if (_dxc == nullptr) {
                auto dxcResult = render::CreateDxc();
                if (!dxcResult.HasValue()) {
                    return nullptr;
                }
                _dxc = dxcResult.Release();
            }
            _jitContext = make_unique<ShaderResolveContext>(
                ShaderResolveSettings{
                    .ShaderRoot = GetShaderRoot(),
                    .Staleness = ShaderArtifactStaleness::Strict,
                    .AllowJit = true},
                _dxc.get());
        }
        return _jitContext.get();
    }

    /// "发布包"上下文: 无 DXC、无产物, 故任何解析都会失败。
    ShaderResolveContext& NoJitContext() {
        if (_noJitContext == nullptr) {
            _noJitContext = make_unique<ShaderResolveContext>(
                ShaderResolveSettings{
                    .ShaderRoot = GetShaderRoot(),
                    .Staleness = ShaderArtifactStaleness::Strict,
                    .AllowJit = false},
                nullptr);
        }
        return *_noJitContext;
    }

    PipelineStateCache& Cache() {
        if (_cache == nullptr) {
            _cache = make_unique<PipelineStateCache>(_ctx.Device.get());
        }
        return *_cache;
    }

    /// 一个 fixture 一个 layout 缓存 —— 真实系统里 RenderSystem 持有的那一份。
    PipelineLayoutCache& Layouts() {
        if (_layouts == nullptr) {
            _layouts = make_unique<PipelineLayoutCache>(_ctx.Device.get());
        }
        return *_layouts;
    }

    /// 一份完整的固定功能状态。PipelineStateCache 不做基线合成, 调用方必须给全。
    GraphicsPipelineStateKey MakeKey(ShaderPassProgram* program) const {
        render::PrimitiveState primitive = render::PrimitiveState::Default();
        primitive.UnclippedDepth = false;
        primitive.Cull = render::CullMode::None;
        return GraphicsPipelineStateKey{
            .Program = program,
            .CompatibleRenderPass = _renderPass.get(),
            .Primitive = primitive,
            .DepthStencil = std::nullopt,
            .MultiSample = render::MultiSampleState::Default(),
            .ColorTargets = _colorTargets};
    }

    render::Device& Device() { return *_ctx.Device; }
    render::ShaderBlobCategory Category() const noexcept { return _category; }
    AssetManager& Assets() { return *_assets; }

    std::array<render::ColorTargetState, 1> _colorTargets{
        render::ColorTargetState::Default(kTargetFormat)};

private:
    DeviceContext _ctx;
    render::ShaderBlobCategory _category{render::ShaderBlobCategory::DXIL};
    NoopRecycler _recycler;
    unique_ptr<AssetManager> _assets;
    std::array<render::RenderPassColorAttachmentDescriptor, 1> _colorAttachments{};
    unique_ptr<render::RenderPass> _renderPass;
    unique_ptr<PipelineStateCache> _cache;
    /// 【刻意不要求它后于 _assets 死】: 缓存只是非拥有索引, 残留 layout 由 program 的
    /// 引用计数保命 —— 照 RenderSystem 先于 AssetManager 关停的顺序。
    unique_ptr<PipelineLayoutCache> _layouts;
    shared_ptr<render::Dxc> _dxc;
    /// 【必须声明在 _dxc 之后】: 借用 Dxc*, 析构逆序保证 context 先死。
    unique_ptr<ShaderResolveContext> _jitContext;
    unique_ptr<ShaderResolveContext> _noJitContext;
};

TEST_F(PipelineStateCacheTest, SameKeyHitsTheSamePipelineState) {
    StreamingAssetRef<ShaderAsset> asset = LoadJitAsset();
    if (!asset.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    ShaderContentRef content_program = asset->AcquireContent();
    ASSERT_TRUE(content_program.HasValue());
    Nullable<ShaderPassProgram*> program = content_program->FindPass("Error");
    ASSERT_TRUE(program.HasValue());

    const GraphicsPipelineStateKey key = MakeKey(program.Get());
    const ShaderVariantKey variant = program->GetDomain().DefaultVariant();

    ShaderAssetDiagnostic diag;
    Nullable<render::GraphicsPipelineState*> first =
        Cache().GetOrCreateGraphics(asset, key, variant, Category(), diag);
    ASSERT_TRUE(first.HasValue()) << diag.ToString();
    EXPECT_EQ(Cache().GetGraphicsMissCount(), 1u);

    Nullable<render::GraphicsPipelineState*> second =
        Cache().GetOrCreateGraphics(asset, key, variant, Category(), diag);
    ASSERT_TRUE(second.HasValue());
    EXPECT_EQ(second.Get(), first.Get());
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 1u);
    EXPECT_EQ(Cache().GetGraphicsHitCount(), 1u);
    EXPECT_EQ(Cache().GetGraphicsMissCount(), 1u);
}

TEST_F(PipelineStateCacheTest, FixedFunctionStateDifferencesSplitEntries) {
    StreamingAssetRef<ShaderAsset> asset = LoadJitAsset();
    if (!asset.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    ShaderContentRef content_program = asset->AcquireContent();
    ASSERT_TRUE(content_program.HasValue());
    Nullable<ShaderPassProgram*> program = content_program->FindPass("Error");
    ASSERT_TRUE(program.HasValue());

    const ShaderVariantKey variant = program->GetDomain().DefaultVariant();
    ShaderAssetDiagnostic diag;

    const GraphicsPipelineStateKey base = MakeKey(program.Get());
    Nullable<render::GraphicsPipelineState*> noCull =
        Cache().GetOrCreateGraphics(asset, base, variant, Category(), diag);
    ASSERT_TRUE(noCull.HasValue()) << diag.ToString();

    // primitive 差异。这是 MaterialRenderState 想覆盖、却又无人提供基线的那三项之一 ——
    // 本层要求调用方给全, 于是它天然成为 key 的一部分。
    GraphicsPipelineStateKey culled = base;
    culled.Primitive.Cull = render::CullMode::Back;
    Nullable<render::GraphicsPipelineState*> backCull =
        Cache().GetOrCreateGraphics(asset, culled, variant, Category(), diag);
    ASSERT_TRUE(backCull.HasValue()) << diag.ToString();
    EXPECT_NE(backCull.Get(), noCull.Get());
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 2u);

    // color target 差异 (关掉写掩码的一个通道)。span 的内容参与比较, 不是地址。
    std::array<render::ColorTargetState, 1> maskedTargets{
        render::ColorTargetState::Default(kTargetFormat)};
    maskedTargets[0].WriteMask = render::ColorWrite::Red;
    GraphicsPipelineStateKey masked = base;
    masked.ColorTargets = maskedTargets;
    Nullable<render::GraphicsPipelineState*> maskedPso =
        Cache().GetOrCreateGraphics(asset, masked, variant, Category(), diag);
    ASSERT_TRUE(maskedPso.HasValue()) << diag.ToString();
    EXPECT_NE(maskedPso.Get(), noCull.Get());
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 3u);
    EXPECT_EQ(Cache().GetGraphicsHitCount(), 0u) << "all three keys differ";

    // 内容相同但存储不同的 span 必须命中 —— 否则每帧重建一个 vector 就会击穿缓存。
    std::array<render::ColorTargetState, 1> equalTargets{
        render::ColorTargetState::Default(kTargetFormat)};
    GraphicsPipelineStateKey equal = base;
    equal.ColorTargets = equalTargets;
    Nullable<render::GraphicsPipelineState*> equalPso =
        Cache().GetOrCreateGraphics(asset, equal, variant, Category(), diag);
    ASSERT_TRUE(equalPso.HasValue());
    EXPECT_EQ(equalPso.Get(), noCull.Get());
    EXPECT_EQ(Cache().GetGraphicsHitCount(), 1u);
}

TEST_F(PipelineStateCacheTest, DifferentVariantSplitsEntriesThroughBytecodeHash) {
    StreamingAssetRef<ShaderAsset> asset = LoadJitAsset();
    if (!asset.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    ShaderContentRef content_program = asset->AcquireContent();
    ASSERT_TRUE(content_program.HasValue());
    Nullable<ShaderPassProgram*> program = content_program->FindPass("Error");
    ASSERT_TRUE(program.HasValue());

    const GraphicsPipelineStateKey key = MakeKey(program.Get());
    const ShaderVariantKey defaultVariant = program->GetDomain().DefaultVariant();
    ShaderAssetDiagnostic diag;

    Nullable<render::GraphicsPipelineState*> base =
        Cache().GetOrCreateGraphics(asset, key, defaultVariant, Category(), diag);
    ASSERT_TRUE(base.HasValue()) << diag.ToString();

    // _POINT_SHADOWS 只作用于 Pixel, 故 VS 的投影结果不变、PS 的 artifact key 改变
    // (defines 参与 ShaderArtifactKeyParams)。PSO 因 PS 的 ShaderHash 不同而分条 ——
    // 这验证的是字节码 hash 而非 ShaderVariantKey 进了 key。
    const std::optional<ShaderVariantKey> shadowed =
        program->GetDomain().WithKeyword(defaultVariant, "_POINT_SHADOWS", true);
    ASSERT_TRUE(shadowed.has_value());

    Nullable<render::GraphicsPipelineState*> shadowedPso =
        Cache().GetOrCreateGraphics(asset, key, shadowed.value(), Category(), diag);
    ASSERT_TRUE(shadowedPso.HasValue()) << diag.ToString();
    EXPECT_NE(shadowedPso.Get(), base.Get());
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 2u);
    EXPECT_EQ(Cache().GetGraphicsHitCount(), 0u);
}

TEST_F(PipelineStateCacheTest, DifferentProgramSplitsEntries) {
    StreamingAssetRef<ShaderAsset> first = LoadJitAsset();
    if (!first.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    // 同一 manifest 再建一份【独立】资产 (直建, 不经 AssetManager::Load 故不按 id 去重),
    // 得到另一个 program 指针。program 指针代表 PipelineLayout + vertex input, 故必须分条。
    // 注意仍要传 AssetManager —— 内容只能由它创建 (见 AssetContentKey), 去重发生在 Load,
    // 与 MakeContent 无关。
    ShaderAssetDiagnostic diag;
    auto secondAsset = CreateShaderAsset(
        Assets(),
        GetErrorPassManifestPath(),
        ShaderAssetLoadOptions{.Context = &NoJitContext(), .LayoutCache = &Layouts()},
        diag);
    ASSERT_TRUE(secondAsset.HasValue()) << diag.ToString();

    ShaderContentRef content_firstProgram = first->AcquireContent();
    ASSERT_TRUE(content_firstProgram.HasValue());

    Nullable<ShaderPassProgram*> firstProgram = content_firstProgram->FindPass("Error");
    ShaderContentRef content_secondProgram = secondAsset->AcquireContent();
    ASSERT_TRUE(content_secondProgram.HasValue());
    Nullable<ShaderPassProgram*> secondProgram = content_secondProgram->FindPass("Error");
    ASSERT_TRUE(firstProgram.HasValue());
    ASSERT_TRUE(secondProgram.HasValue());
    ASSERT_NE(firstProgram.Get(), secondProgram.Get());
    // 两份资产是同一 manifest, 布局逐字节相同, 故【共享同一个 layout】。这正是
    // "program 相同则 layout 相同, 反之不成立"那条单向关系: layout 相同不足以合并 PSO
    // 条目, program 指针仍须分条。
    EXPECT_EQ(firstProgram->GetPipelineLayout().Get(), secondProgram->GetPipelineLayout().Get());
    EXPECT_EQ(Layouts().GetLayoutCount(), 1u);

    const ShaderVariantKey variant = firstProgram->GetDomain().DefaultVariant();
    Nullable<render::GraphicsPipelineState*> firstPso =
        Cache().GetOrCreateGraphics(first, MakeKey(firstProgram.Get()), variant, Category(), diag);
    ASSERT_TRUE(firstPso.HasValue()) << diag.ToString();

    // 第二份资产无 DXC 无产物, 解析必失败 —— 顺带验证 miss 路径不写缓存。
    ShaderAssetDiagnostic secondDiag;
    Nullable<render::GraphicsPipelineState*> secondPso = Cache().GetOrCreateGraphics(
        nullptr, MakeKey(secondProgram.Get()), variant, Category(), secondDiag);
    EXPECT_FALSE(secondPso.HasValue());
    EXPECT_FALSE(secondDiag.Message.empty());
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 1u)
        << "a failed resolve must not add an entry";

    // OnUnload 前手动收尾, 避免 layout 泄漏到 device 之后。
    NoopRecycler recycler;
    secondAsset->OnUnload(recycler);
}

TEST_F(PipelineStateCacheTest, RemovePipelineStatesUsingEvictsByAsset) {
    StreamingAssetRef<ShaderAsset> asset = LoadJitAsset();
    if (!asset.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    ShaderContentRef content_program = asset->AcquireContent();
    ASSERT_TRUE(content_program.HasValue());
    Nullable<ShaderPassProgram*> program = content_program->FindPass("Error");
    ASSERT_TRUE(program.HasValue());

    const ShaderVariantKey variant = program->GetDomain().DefaultVariant();
    ShaderAssetDiagnostic diag;

    GraphicsPipelineStateKey key = MakeKey(program.Get());
    ASSERT_TRUE(Cache().GetOrCreateGraphics(asset, key, variant, Category(), diag).HasValue())
        << diag.ToString();
    key.Primitive.Cull = render::CullMode::Back;
    ASSERT_TRUE(Cache().GetOrCreateGraphics(asset, key, variant, Category(), diag).HasValue())
        << diag.ToString();
    ASSERT_EQ(Cache().GetGraphicsPipelineStateCount(), 2u);

    EXPECT_EQ(Cache().RemovePipelineStatesUsing(nullptr), 0u);

    ShaderAsset* raw = asset.Get();
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(Cache().RemovePipelineStatesUsing(raw), 2u);
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 0u);

    // 逐出后资产不再被 PSO 钉住, 显式 Unload 可以安全回收 layout。
    Assets().Unload(raw->GetAssetId());
    EXPECT_EQ(asset.Get(), nullptr);
}

/// 【守的不变量】: 强制 Unload 销毁资产槽位后, 缓存里的 PSO 仍能安全使用 —— 它引用的
/// PipelineLayout 由本条目独立持有的一份引用保命。
///
/// 【为何 Ref 不够】: Unload 无视引用计数销毁槽位, ShaderPassProgram 随之析构并放开它那
/// 份 layout 引用。若条目只有 StreamingAssetRefAny, 计数就此归零、layout 被 Destroy,
/// 而后端 PSO 里的裸指针 (D3D12 的 RootSigD3D12*) 仍会在下次 bind 时被解引用。
TEST_F(PipelineStateCacheTest, PipelineStateKeepsItsLayoutAliveAcrossForcedUnload) {
    StreamingAssetRef<ShaderAsset> asset = LoadJitAsset();
    if (!asset.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    ShaderContentRef content_program = asset->AcquireContent();
    ASSERT_TRUE(content_program.HasValue());
    Nullable<ShaderPassProgram*> program = content_program->FindPass("Error");
    ASSERT_TRUE(program.HasValue());

    const SharedPipelineLayoutRef sharedLayout = program->GetSharedPipelineLayout();
    ASSERT_TRUE(sharedLayout.HasValue());
    render::PipelineLayout* layoutObject = sharedLayout->Get();
    ASSERT_NE(layoutObject, nullptr);

    const ShaderVariantKey variant = program->GetDomain().DefaultVariant();
    ShaderAssetDiagnostic diag;
    const GraphicsPipelineStateKey key = MakeKey(program.Get());
    render::GraphicsPipelineState* pso =
        Cache().GetOrCreateGraphics(asset, key, variant, Category(), diag).Get();
    ASSERT_NE(pso, nullptr) << diag.ToString();

    // 本地这份 + program 那份 + 缓存条目那份。缓存条目【确实】独立持有一份, 这是本用例
    // 的核心断言 —— 若不持有, 计数只会是 2。
    EXPECT_EQ(sharedLayout->GetRefCount(), 3u);

    const AssetId id = asset->GetAssetId();
    Assets().Unload(id);
    ASSERT_EQ(asset.Get(), nullptr) << "Unload 应当销毁槽位";

    // Unload 只销毁槽位, 不销毁内容: 本地还握着 content_program, 故 program 及它那份
    // layout 引用都还在, 计数不变。
    EXPECT_EQ(sharedLayout->GetRefCount(), 3u) << "内容仍被持有, program 不该析构";

    // 放开本地这份内容引用。计数【仍是 3】: 缓存条目自己也持有一份内容引用
    // (GraphicsEntry::Content), 故 ShaderContent 连同它的 ShaderPassProgram 都还活着,
    // program 那份 layout 引用也就没放开。这正是条目该有的样子 —— 它的 Program 是指向
    // 内容内部的裸指针, 不保内容就会悬垂。
    content_program.Reset();
    EXPECT_EQ(sharedLayout->GetRefCount(), 3u) << "缓存条目独立保住了内容";
    EXPECT_EQ(sharedLayout->Get(), layoutObject);
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 1u);

    // 条目消失后才轮到最后一份: 只剩本地这个 sharedLayout, 且 layout 对象【自始至终
    // 没被销毁过】(地址不变) —— 强制 Unload 期间后端 PSO 里的裸指针一直有效。
    Cache().Clear();
    EXPECT_EQ(sharedLayout->GetRefCount(), 1u);
    EXPECT_EQ(sharedLayout->Get(), layoutObject);
}

TEST_F(PipelineStateCacheTest, InvalidKeyIsRejectedWithoutTouchingTheDevice) {
    StreamingAssetRef<ShaderAsset> asset = LoadJitAsset();
    if (!asset.IsReady()) {
        GTEST_SKIP() << "DXC is unavailable";
    }
    ShaderContentRef content_program = asset->AcquireContent();
    ASSERT_TRUE(content_program.HasValue());
    Nullable<ShaderPassProgram*> program = content_program->FindPass("Error");
    ASSERT_TRUE(program.HasValue());

    const ShaderVariantKey variant = program->GetDomain().DefaultVariant();

    ShaderAssetDiagnostic diag;
    GraphicsPipelineStateKey noProgram = MakeKey(program.Get());
    noProgram.Program = nullptr;
    EXPECT_FALSE(Cache().GetOrCreateGraphics(asset, noProgram, variant, Category(), diag).HasValue());
    EXPECT_FALSE(diag.Message.empty());

    ShaderAssetDiagnostic passDiag;
    GraphicsPipelineStateKey noRenderPass = MakeKey(program.Get());
    noRenderPass.CompatibleRenderPass = nullptr;
    EXPECT_FALSE(
        Cache().GetOrCreateGraphics(asset, noRenderPass, variant, Category(), passDiag).HasValue());
    EXPECT_FALSE(passDiag.Message.empty());

    // 两次都在解析变体之前就被拒, 故连 miss 都不该计。
    EXPECT_EQ(Cache().GetGraphicsMissCount(), 0u);
    EXPECT_EQ(Cache().GetGraphicsPipelineStateCount(), 0u);
}

}  // namespace
}  // namespace radray
